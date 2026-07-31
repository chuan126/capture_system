from __future__ import annotations

import threading
from collections.abc import Callable

from backend.protocols.cloud_preview_v1 import (
    CloudPreviewFrame,
    encode_cloud_preview,
)


class CloudPreviewBridge:
    """在专用ROS上下文中订阅轻量点云，并把编码结果交给Web事件循环。"""

    def __init__(
        self,
        frame_sink: Callable[[CloudPreviewFrame], None],
        topic: str = "/capture/visualization/cloud_preview",
    ) -> None:
        self._frame_sink = frame_sink
        self._topic = topic
        self._thread: threading.Thread | None = None
        self._started = threading.Event()
        self._stop_requested = threading.Event()
        self._executor: object | None = None
        self._sequence = 0
        self.error: str | None = None

    def start(self, timeout_seconds: float = 3.0) -> bool:
        if self._thread is not None:
            return self.error is None

        self._thread = threading.Thread(
            target=self._run,
            name="cloud-preview-ros",
            daemon=True,
        )
        self._thread.start()
        if not self._started.wait(timeout_seconds):
            self.error = "ROS桥启动超时"
            return False
        return self.error is None

    def stop(self, timeout_seconds: float = 3.0) -> None:
        self._stop_requested.set()
        executor = self._executor
        if executor is not None:
            try:
                executor.wake()
            except Exception:
                pass

        if self._thread is not None:
            self._thread.join(timeout_seconds)
            if self._thread.is_alive() and self.error is None:
                self.error = "ROS桥线程未在限定时间内退出"

    def _run(self) -> None:
        context = None
        node = None
        executor = None

        try:
            import rclpy
            from rclpy.context import Context
            from rclpy.executors import SingleThreadedExecutor
            from rclpy.node import Node
            from rclpy.qos import (
                DurabilityPolicy,
                HistoryPolicy,
                QoSProfile,
                ReliabilityPolicy,
            )
            from sensor_msgs.msg import PointCloud2

            context = Context()
            rclpy.init(context=context)
            node = Node("web_cloud_preview_bridge", context=context)
            qos = QoSProfile(
                reliability=ReliabilityPolicy.BEST_EFFORT,
                durability=DurabilityPolicy.VOLATILE,
                history=HistoryPolicy.KEEP_LAST,
                depth=1,
            )
            node.create_subscription(
                PointCloud2,
                self._topic,
                self._on_cloud,
                qos,
            )
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
        frame = encode_cloud_preview(message, self._sequence)
        self._sequence = (self._sequence + 1) & 0xFFFFFFFF
        self._frame_sink(frame)
