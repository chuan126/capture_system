from __future__ import annotations

import math
import struct
import threading
import time
from collections.abc import Callable

from backend.protocols.cloud_preview_v1 import (
    PCV1_FLAG_SENSOR_STAMP_VALID,
    PCV1_HEADER_BYTES,
    PCV1_MAGIC,
    PCV1_POINT_STRIDE,
    PCV1_VERSION,
    CloudPreviewFrame,
)

_HEADER_STRUCT = struct.Struct("<4sHHIQI")
_FLOAT3 = struct.Struct("<fff")
DEV_RAW_PREVIEW_MAX_POINTS = 10_000


def encode_raw_cloud_preview(message: object, sequence: int, max_points: int = DEV_RAW_PREVIEW_MAX_POINTS) -> CloudPreviewFrame:
    """Extract x/y/z from arbitrary PointCloud2 fields and uniformly downsample for browser preview."""
    fields = {str(getattr(field, "name")): field for field in getattr(message, "fields")}
    missing = [name for name in ("x", "y", "z") if name not in fields]
    if missing:
        raise ValueError(f"原始PointCloud2缺少字段：{','.join(missing)}")
    if bool(getattr(message, "is_bigendian", False)):
        raise ValueError("开发原始点云预览暂不支持大端PointCloud2")

    offsets = [int(getattr(fields[name], "offset")) for name in ("x", "y", "z")]
    datatypes = [int(getattr(fields[name], "datatype")) for name in ("x", "y", "z")]
    # sensor_msgs/PointField.FLOAT32 == 7
    if datatypes != [7, 7, 7]:
        raise ValueError("原始PointCloud2的x/y/z必须为FLOAT32")

    point_step = int(getattr(message, "point_step"))
    width = int(getattr(message, "width"))
    height = int(getattr(message, "height"))
    row_step = int(getattr(message, "row_step"))
    total_points = max(0, width * height)
    raw = memoryview(getattr(message, "data"))
    if point_step <= 0 or total_points <= 0:
        sampled = b""
        sampled_count = 0
    else:
        stride = max(1, math.ceil(total_points / max_points))
        output = bytearray(min(total_points, max_points) * PCV1_POINT_STRIDE)
        sampled_count = 0
        for index in range(0, total_points, stride):
            row = index // max(width, 1)
            column = index % max(width, 1)
            base = row * row_step + column * point_step
            if base + point_step > len(raw):
                break
            x = struct.unpack_from("<f", raw, base + offsets[0])[0]
            y = struct.unpack_from("<f", raw, base + offsets[1])[0]
            z = struct.unpack_from("<f", raw, base + offsets[2])[0]
            if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(z)):
                continue
            _FLOAT3.pack_into(output, sampled_count * PCV1_POINT_STRIDE, x, y, z)
            sampled_count += 1
            if sampled_count >= max_points:
                break
        sampled = bytes(output[: sampled_count * PCV1_POINT_STRIDE])

    stamp = getattr(getattr(message, "header"), "stamp")
    sensor_stamp_ns = int(getattr(stamp, "sec")) * 1_000_000_000 + int(getattr(stamp, "nanosec"))
    sequence_u32 = sequence & 0xFFFFFFFF
    header = _HEADER_STRUCT.pack(
        PCV1_MAGIC,
        PCV1_VERSION,
        PCV1_FLAG_SENSOR_STAMP_VALID,
        sequence_u32,
        sensor_stamp_ns,
        sampled_count,
    )
    return CloudPreviewFrame(
        sequence=sequence_u32,
        sensor_stamp_ns=sensor_stamp_ns,
        point_count=sampled_count,
        frame_id=str(getattr(getattr(message, "header"), "frame_id")),
        binary=header + sampled,
        coordinate_mode="sensor",
        max_points=max_points,
    )


class DevRawCloudPreviewBridge:
    """Development-only raw PointCloud2 preview bridge, rate-limited before Python conversion."""

    def __init__(
        self,
        frame_sink: Callable[[CloudPreviewFrame], None],
        topic: str = "/capture/lidar/points_raw",
        max_rate_hz: float = 5.0,
    ) -> None:
        self._frame_sink = frame_sink
        self._topic = topic
        self._min_interval = 1.0 / max(max_rate_hz, 0.1)
        self._thread: threading.Thread | None = None
        self._started = threading.Event()
        self._stop_requested = threading.Event()
        self._executor: object | None = None
        self._sequence = 0
        self._last_encoded = 0.0
        self.error: str | None = None

    def start(self, timeout_seconds: float = 3.0) -> bool:
        if self._thread is not None:
            return self.error is None
        self._thread = threading.Thread(target=self._run, name="dev-raw-cloud-ros", daemon=True)
        self._thread.start()
        if not self._started.wait(timeout_seconds):
            self.error = "原始点云开发预览ROS桥启动超时"
            return False
        return self.error is None

    def stop(self, timeout_seconds: float = 3.0) -> None:
        self._stop_requested.set()
        if self._executor is not None:
            try:
                self._executor.wake()
            except Exception:
                pass
        if self._thread is not None:
            self._thread.join(timeout_seconds)

    def _run(self) -> None:
        context = node = executor = None
        try:
            import rclpy
            from rclpy.context import Context
            from rclpy.executors import SingleThreadedExecutor
            from rclpy.node import Node
            from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy
            from sensor_msgs.msg import PointCloud2

            context = Context()
            rclpy.init(context=context)
            node = Node("web_dev_raw_cloud_bridge", context=context)
            qos = QoSProfile(
                reliability=ReliabilityPolicy.RELIABLE,
                durability=DurabilityPolicy.VOLATILE,
                history=HistoryPolicy.KEEP_LAST,
                depth=1,
            )
            node.create_subscription(PointCloud2, self._topic, self._on_cloud, qos)
            executor = SingleThreadedExecutor(context=context)
            executor.add_node(node)
            self._executor = executor
            self._started.set()
            while not self._stop_requested.is_set():
                executor.spin_once(timeout_sec=0.1)
        except Exception as exception:
            self.error = f"{type(exception).__name__}: {exception}"
            self._started.set()
        finally:
            self._executor = None
            if executor is not None:
                try:
                    if node is not None:
                        executor.remove_node(node)
                    executor.shutdown(timeout_sec=1.0)
                except Exception:
                    pass
            if node is not None:
                try:
                    node.destroy_node()
                except Exception:
                    pass
            if context is not None:
                try:
                    context.try_shutdown()
                except Exception:
                    pass

    def _on_cloud(self, message: object) -> None:
        now = time.monotonic()
        if now - self._last_encoded < self._min_interval:
            return
        self._last_encoded = now
        try:
            frame = encode_raw_cloud_preview(message, self._sequence)
        except Exception as exception:
            self.error = f"{type(exception).__name__}: {exception}"
            return
        self._sequence = (self._sequence + 1) & 0xFFFFFFFF
        self._frame_sink(frame)
