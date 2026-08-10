from __future__ import annotations

import threading
import time
from collections.abc import Callable
from dataclasses import replace

from backend.protocols.rtk_v1 import (
    RtkSnapshot,
    with_fix,
    with_localization_status,
    with_status,
)


class RtkBridge:
    """订阅RTK ROS消息并生成不含质量判断的最新值快照。"""

    def __init__(
        self,
        snapshot_sink: Callable[[RtkSnapshot], None],
        status_topic: str = "/capture/rtk/status",
        fix_topic: str = "/capture/rtk/fix",
        localization_status_topic: str = "/capture/localization/status",
    ) -> None:
        self._snapshot_sink = snapshot_sink
        self._status_topic = status_topic
        self._fix_topic = fix_topic
        self._localization_status_topic = localization_status_topic
        self._thread: threading.Thread | None = None
        self._started = threading.Event()
        self._stop_requested = threading.Event()
        self._executor: object | None = None
        self._snapshot = RtkSnapshot()
        self._sequence = 0
        self.error: str | None = None

    def start(self, timeout_seconds: float = 3.0) -> bool:
        if self._thread is not None:
            return self.error is None
        self._thread = threading.Thread(
            target=self._run,
            name="rtk-ros",
            daemon=True,
        )
        self._thread.start()
        if not self._started.wait(timeout_seconds):
            self.error = "RTK ROS桥启动超时"
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
                self.error = "RTK ROS桥线程未在限定时间内退出"

    def _run(self) -> None:
        context = None
        node = None
        executor = None
        try:
            import rclpy
            from interfaces.msg import LocalizationStatus
            from interfaces.msg import RtkStatus
            from rclpy.context import Context
            from rclpy.executors import SingleThreadedExecutor
            from rclpy.node import Node
            from rclpy.qos import (
                DurabilityPolicy,
                HistoryPolicy,
                QoSProfile,
                ReliabilityPolicy,
            )
            from sensor_msgs.msg import NavSatFix

            context = Context()
            rclpy.init(context=context)
            node = Node("web_rtk_bridge", context=context)
            qos = QoSProfile(
                reliability=ReliabilityPolicy.RELIABLE,
                durability=DurabilityPolicy.VOLATILE,
                history=HistoryPolicy.KEEP_LAST,
                depth=5,
            )
            node.create_subscription(RtkStatus, self._status_topic, self._on_status, qos)
            node.create_subscription(NavSatFix, self._fix_topic, self._on_fix, qos)
            node.create_subscription(
                LocalizationStatus,
                self._localization_status_topic,
                self._on_localization_status,
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

    def _emit(self) -> None:
        self._snapshot = replace(
            self._snapshot,
            sequence=self._sequence,
            emitted_at_ns=time.time_ns(),
        )
        self._sequence = (self._sequence + 1) & 0xFFFFFFFF
        self._snapshot_sink(self._snapshot)

    def _on_status(self, message: object) -> None:
        self._snapshot = with_status(self._snapshot, message)
        self._emit()

    def _on_fix(self, message: object) -> None:
        self._snapshot = with_fix(self._snapshot, message)
        self._emit()

    def _on_localization_status(self, message: object) -> None:
        self._snapshot = with_localization_status(self._snapshot, message)
        self._emit()
