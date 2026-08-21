from __future__ import annotations

import math
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any


@dataclass(slots=True)
class _TopicState:
    key: str
    topic: str
    message_type: str
    received_count: int = 0
    last_received_monotonic: float | None = None
    last_received_ns: int | None = None
    last_sensor_stamp_ns: int | None = None
    timestamps: deque[float] = field(default_factory=lambda: deque(maxlen=256))
    extra: dict[str, object] = field(default_factory=dict)

    def reset(self) -> None:
        self.received_count = 0
        self.last_received_monotonic = None
        self.last_received_ns = None
        self.last_sensor_stamp_ns = None
        self.timestamps.clear()
        self.extra = {}

    def update(self, message: object, extra: dict[str, object] | None = None) -> None:
        now_monotonic = time.monotonic()
        self.received_count += 1
        self.last_received_monotonic = now_monotonic
        self.last_received_ns = time.time_ns()
        self.timestamps.append(now_monotonic)
        self.last_sensor_stamp_ns = _message_stamp_ns(message)
        if extra is not None:
            self.extra = extra

    def to_dict(self, now_monotonic: float) -> dict[str, object]:
        rate_hz = 0.0
        if len(self.timestamps) >= 2:
            elapsed = self.timestamps[-1] - self.timestamps[0]
            if elapsed > 0:
                rate_hz = (len(self.timestamps) - 1) / elapsed
        age_ms: float | None = None
        if self.last_received_monotonic is not None:
            age_ms = max(0.0, (now_monotonic - self.last_received_monotonic) * 1000.0)
        return {
            "key": self.key,
            "topic": self.topic,
            "message_type": self.message_type,
            "received_count": self.received_count,
            "rate_hz": round(rate_hz, 3),
            "last_received_ns": self.last_received_ns,
            "last_sensor_stamp_ns": self.last_sensor_stamp_ns,
            "age_ms": None if age_ms is None else round(age_ms, 1),
            "state": "waiting" if age_ms is None else "stale" if age_ms > 1500.0 else "streaming",
            **self.extra,
        }


def _message_stamp_ns(message: object) -> int | None:
    try:
        stamp = getattr(getattr(message, "header"), "stamp")
        return int(getattr(stamp, "sec")) * 1_000_000_000 + int(getattr(stamp, "nanosec"))
    except Exception:
        return None


def _finite_or_none(value: object) -> float | None:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


class DevTelemetryBridge:
    """Development-only ROS topic counters and latest diagnostic values."""

    def __init__(self, idle_timeout_seconds: float = 3.0) -> None:
        self._thread: threading.Thread | None = None
        self._started = threading.Event()
        self._stop_requested = threading.Event()
        self._executor: object | None = None
        self._lock = threading.Lock()
        self._lifecycle_lock = threading.Lock()
        self._idle_timeout_seconds = max(0.1, float(idle_timeout_seconds))
        self._idle_deadline_monotonic: float | None = None
        self.error: str | None = None
        self._states = {
            "raw_cloud": _TopicState("raw_cloud", "/capture/lidar/points_raw", "sensor_msgs/msg/PointCloud2"),
            "odometry": _TopicState("odometry", "/capture/odometry/high_rate", "nav_msgs/msg/Odometry"),
            "compensated_cloud": _TopicState("compensated_cloud", "/capture/lidar/points_compensated_enu", "sensor_msgs/msg/PointCloud2"),
            "clearance": _TopicState("clearance", "/capture/clearance/result", "interfaces/msg/ClearanceResult"),
            "rtk_fix": _TopicState("rtk_fix", "/capture/rtk/fix", "sensor_msgs/msg/NavSatFix"),
            "rtk_status": _TopicState("rtk_status", "/capture/rtk/status", "interfaces/msg/RtkStatus"),
            "task_status": _TopicState("task_status", "/capture/task/status", "interfaces/msg/TaskStatus"),
            "recording_status": _TopicState("recording_status", "/capture/recording/status", "interfaces/msg/RecordingStatus"),
        }

    @property
    def active(self) -> bool:
        thread = self._thread
        return bool(thread is not None and thread.is_alive() and self._started.is_set() and self.error is None)

    def start(self, timeout_seconds: float = 3.0) -> bool:
        """启动持续诊断桥；主要供测试或显式生命周期管理使用。"""
        return self._ensure_started(timeout_seconds=timeout_seconds, idle_deadline=None)

    def touch(self, timeout_seconds: float = 3.0) -> bool:
        """为开发页面续租诊断桥；页面停止轮询后自动释放高频ROS订阅。"""
        deadline = time.monotonic() + self._idle_timeout_seconds
        return self._ensure_started(timeout_seconds=timeout_seconds, idle_deadline=deadline)

    def _ensure_started(self, *, timeout_seconds: float, idle_deadline: float | None) -> bool:
        with self._lifecycle_lock:
            self._idle_deadline_monotonic = idle_deadline
            if self._thread is not None and self._thread.is_alive():
                started_event = self._started
            else:
                self._started.clear()
                self._stop_requested.clear()
                self.error = None
                with self._lock:
                    for state in self._states.values():
                        state.reset()
                self._thread = threading.Thread(target=self._run, name="dev-telemetry-ros", daemon=True)
                self._thread.start()
                started_event = self._started

        if not started_event.wait(timeout_seconds):
            self.error = "开发诊断ROS桥启动超时"
            return False
        return self.error is None

    def stop(self, timeout_seconds: float = 3.0) -> None:
        with self._lifecycle_lock:
            self._idle_deadline_monotonic = None
            self._stop_requested.set()
            executor = self._executor
            thread = self._thread
        if executor is not None:
            try:
                executor.wake()
            except Exception:
                pass
        if thread is not None:
            thread.join(timeout_seconds)
        with self._lifecycle_lock:
            if self._thread is thread and (thread is None or not thread.is_alive()):
                self._thread = None
                self._started.clear()

    def snapshot(self) -> dict[str, object]:
        now_monotonic = time.monotonic()
        with self._lock:
            topics = {key: state.to_dict(now_monotonic) for key, state in self._states.items()}
        return {
            "bridge_available": self.active,
            "bridge_error": self.error,
            "emitted_at_ns": time.time_ns(),
            "topics": topics,
        }

    def _record(self, key: str, message: object, extra: dict[str, object] | None = None) -> None:
        with self._lock:
            self._states[key].update(message, extra)

    def _run(self) -> None:
        context = node = executor = None
        try:
            import rclpy
            from interfaces.msg import ClearanceResult, RecordingStatus, RtkStatus, TaskStatus
            from nav_msgs.msg import Odometry
            from rclpy.context import Context
            from rclpy.executors import SingleThreadedExecutor
            from rclpy.node import Node
            from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy
            from sensor_msgs.msg import NavSatFix, PointCloud2

            context = Context()
            rclpy.init(context=context)
            node = Node("web_dev_telemetry_bridge", context=context)
            reliable = QoSProfile(
                reliability=ReliabilityPolicy.RELIABLE,
                durability=DurabilityPolicy.VOLATILE,
                history=HistoryPolicy.KEEP_LAST,
                depth=5,
            )
            point_cloud_qos = QoSProfile(
                reliability=ReliabilityPolicy.RELIABLE,
                durability=DurabilityPolicy.VOLATILE,
                history=HistoryPolicy.KEEP_LAST,
                depth=2,
            )

            node.create_subscription(
                PointCloud2,
                "/capture/lidar/points_raw",
                lambda msg: self._record("raw_cloud", msg, {
                    "point_count": int(getattr(msg, "width", 0)) * int(getattr(msg, "height", 1)),
                    "frame_id": str(getattr(getattr(msg, "header"), "frame_id")),
                    "point_step": int(getattr(msg, "point_step", 0)),
                    "data_bytes": len(getattr(msg, "data", b"")),
                }),
                point_cloud_qos,
            )
            node.create_subscription(
                Odometry,
                "/capture/odometry/high_rate",
                lambda msg: self._record("odometry", msg),
                reliable,
            )
            node.create_subscription(
                PointCloud2,
                "/capture/lidar/points_compensated_enu",
                lambda msg: self._record("compensated_cloud", msg, {
                    "point_count": int(getattr(msg, "width", 0)) * int(getattr(msg, "height", 1)),
                    "frame_id": str(getattr(getattr(msg, "header"), "frame_id")),
                }),
                reliable,
            )
            node.create_subscription(
                ClearanceResult,
                "/capture/clearance/result",
                lambda msg: self._record("clearance", msg, {
                    "valid": bool(getattr(msg, "valid", False)),
                    "lidar_to_top_m": _finite_or_none(getattr(msg, "lidar_to_top_m", None)),
                    "ransac_plane_count": int(getattr(msg, "ransac_plane_count", 0)),
                    "surface_count": int(getattr(msg, "surface_count", 0)),
                    "candidate_count": int(getattr(msg, "candidate_count", 0)),
                    "selected_inlier_count": int(getattr(msg, "selected_inlier_count", 0)),
                    "selected_area_m2": _finite_or_none(getattr(msg, "selected_area_m2", None)),
                    "selected_tilt_deg": _finite_or_none(getattr(msg, "selected_tilt_deg", None)),
                    "residual_p95_m": _finite_or_none(getattr(msg, "residual_p95_m", None)),
                    "valid_point_ratio": _finite_or_none(getattr(msg, "valid_point_ratio", None)),
                    "processing_time_ms": _finite_or_none(getattr(msg, "processing_time_ms", None)),
                    "invalid_reason": str(getattr(msg, "invalid_reason", "")),
                }),
                reliable,
            )
            node.create_subscription(
                NavSatFix,
                "/capture/rtk/fix",
                lambda msg: self._record("rtk_fix", msg, {
                    "fix_status": int(getattr(getattr(msg, "status"), "status", -1)),
                    "latitude": _finite_or_none(getattr(msg, "latitude", None)),
                    "longitude": _finite_or_none(getattr(msg, "longitude", None)),
                    "altitude": _finite_or_none(getattr(msg, "altitude", None)),
                }),
                reliable,
            )
            node.create_subscription(
                RtkStatus,
                "/capture/rtk/status",
                lambda msg: self._record("rtk_status", msg, {
                    "satellite_count": int(getattr(msg, "satellite_count", 0)),
                    "hdop": _finite_or_none(getattr(msg, "hdop", None)),
                    "pdop": _finite_or_none(getattr(msg, "pdop", None)),
                    "gps_state": int(getattr(msg, "gps_state", 0)),
                }),
                reliable,
            )
            node.create_subscription(
                TaskStatus,
                "/capture/task/status",
                lambda msg: self._record("task_status", msg, {
                    "task_id": str(getattr(msg, "task_id", "")),
                    "status": str(getattr(msg, "status", "")),
                    "operation_phase": str(getattr(msg, "operation_phase", "")),
                    "status_revision": int(getattr(msg, "status_revision", 0)),
                }),
                reliable,
            )
            node.create_subscription(
                RecordingStatus,
                "/capture/recording/status",
                lambda msg: self._record("recording_status", msg, {
                    "task_id": str(getattr(msg, "task_id", "")),
                    "status": str(getattr(msg, "state", "")),
                    "message": str(getattr(msg, "message", "")),
                    "total_samples": int(getattr(msg, "total_samples", 0)),
                    "valid_samples": int(getattr(msg, "valid_samples", 0)),
                    "invalid_samples": int(getattr(msg, "invalid_samples", 0)),
                    "recording_path": str(getattr(msg, "recording_path", "")),
                }),
                reliable,
            )

            executor = SingleThreadedExecutor(context=context)
            executor.add_node(node)
            self._executor = executor
            self._started.set()
            while not self._stop_requested.is_set():
                executor.spin_once(timeout_sec=0.1)
                deadline = self._idle_deadline_monotonic
                if deadline is not None and time.monotonic() >= deadline:
                    break
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
