from __future__ import annotations

import threading
import time
from collections.abc import Callable

from backend.protocols.system_status_v1 import (
    DIAGNOSTIC_NAMES,
    DeviceStatus,
    SystemStatusSnapshot,
    device_status,
)


class SystemStatusBridge:
    """将system_monitor统一诊断机械映射为网页快照。"""

    def __init__(
        self,
        snapshot_sink: Callable[[SystemStatusSnapshot], None],
        diagnostics_topic: str = "/capture/system/diagnostics",
    ) -> None:
        self._snapshot_sink = snapshot_sink
        self._diagnostics_topic = diagnostics_topic
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
            target=self._run, name="system-status-ros", daemon=True
        )
        self._thread.start()
        if not self._started.wait(timeout_seconds):
            self.error = "系统状态ROS桥启动超时"
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
                self.error = "系统状态ROS桥线程未在限定时间内退出"

    def _run(self) -> None:
        context = node = executor = None
        try:
            import rclpy
            from diagnostic_msgs.msg import DiagnosticArray
            from rclpy.context import Context
            from rclpy.executors import SingleThreadedExecutor
            from rclpy.node import Node

            context = Context()
            rclpy.init(context=context)
            node = Node("web_system_status_bridge", context=context)
            node.create_subscription(
                DiagnosticArray, self._diagnostics_topic, self._on_diagnostics, 5
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

    def _on_diagnostics(self, message: object) -> None:
        devices: dict[str, DeviceStatus] = {
            key: DeviceStatus() for key in DIAGNOSTIC_NAMES.values()
        }
        for status in getattr(message, "status"):
            key = DIAGNOSTIC_NAMES.get(str(getattr(status, "name")))
            if key is not None:
                devices[key] = device_status(status)
        snapshot = SystemStatusSnapshot(
            sequence=self._sequence,
            emitted_at_ns=time.time_ns(),
            lidar=devices["lidar"],
            rtk=devices["rtk"],
            controller=devices["controller"],
            storage=devices["storage"],
        )
        self._sequence = (self._sequence + 1) & 0xFFFFFFFF
        self._snapshot_sink(snapshot)
