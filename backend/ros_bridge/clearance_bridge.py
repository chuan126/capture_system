from __future__ import annotations

import threading
from collections.abc import Callable

from backend.protocols.clearance_v1 import ClearanceSnapshot, from_ros_message


class ClearanceBridge:
    """订阅净空结果并机械映射为浏览器快照。"""

    def __init__(
        self,
        snapshot_sink: Callable[[ClearanceSnapshot], None],
        result_topic: str = "/capture/clearance/result",
    ) -> None:
        self._snapshot_sink = snapshot_sink
        self._result_topic = result_topic
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
            target=self._run, name="clearance-result-ros", daemon=True
        )
        self._thread.start()
        if not self._started.wait(timeout_seconds):
            self.error = "净空结果ROS桥启动超时"
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
            if self._thread.is_alive() and self.error is None:
                self.error = "净空结果ROS桥线程未在限定时间内退出"

    def _run(self) -> None:
        context = node = executor = None
        try:
            import rclpy
            from interfaces.msg import ClearanceResult
            from rclpy.context import Context
            from rclpy.executors import SingleThreadedExecutor
            from rclpy.node import Node
            from rclpy.qos import (
                DurabilityPolicy,
                HistoryPolicy,
                QoSProfile,
                ReliabilityPolicy,
            )

            context = Context()
            rclpy.init(context=context)
            node = Node("web_clearance_bridge", context=context)
            qos = QoSProfile(
                reliability=ReliabilityPolicy.RELIABLE,
                durability=DurabilityPolicy.VOLATILE,
                history=HistoryPolicy.KEEP_LAST,
                depth=2,
            )
            node.create_subscription(
                ClearanceResult, self._result_topic, self._on_result, qos
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

    def _on_result(self, message: object) -> None:
        snapshot = from_ros_message(message, self._sequence)
        self._sequence = (self._sequence + 1) & 0xFFFFFFFF
        self._snapshot_sink(snapshot)
