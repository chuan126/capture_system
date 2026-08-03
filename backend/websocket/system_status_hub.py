from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass, field

from backend.protocols.system_status_v1 import SystemStatusSnapshot


class SystemStatusClientLimitReachedError(RuntimeError):
    pass


@dataclass(eq=False, slots=True)
class SystemStatusSession:
    queue: asyncio.Queue[SystemStatusSnapshot] = field(
        default_factory=lambda: asyncio.Queue(maxsize=1)
    )


class SystemStatusHub:
    def __init__(self, max_clients: int = 4, stale_after_seconds: float = 3.0) -> None:
        self._max_clients = max_clients
        self._stale_after_seconds = stale_after_seconds
        self._sessions: set[SystemStatusSession] = set()
        self._latest: SystemStatusSnapshot | None = None
        self._last_publish_monotonic: float | None = None
        self._ros_available = False
        self._ros_error: str | None = None

    def set_ros_availability(self, available: bool, error: str | None = None) -> None:
        self._ros_available = available
        self._ros_error = error

    def publish(self, snapshot: SystemStatusSnapshot) -> None:
        self._latest = snapshot
        self._last_publish_monotonic = time.monotonic()
        for session in tuple(self._sessions):
            if session.queue.full():
                try:
                    session.queue.get_nowait()
                except asyncio.QueueEmpty:
                    pass
            session.queue.put_nowait(snapshot)

    def register(self) -> SystemStatusSession:
        if len(self._sessions) >= self._max_clients:
            raise SystemStatusClientLimitReachedError()
        session = SystemStatusSession()
        self._sessions.add(session)
        if self._latest is not None:
            session.queue.put_nowait(self._latest)
        return session

    def unregister(self, session: SystemStatusSession) -> None:
        self._sessions.discard(session)

    def current_status(self) -> dict[str, str]:
        if not self._ros_available:
            detail = "系统状态ROS桥不可用"
            if self._ros_error:
                detail += f"：{self._ros_error}"
            return {"type": "status", "state": "ros_unavailable", "reason": "ROS_BRIDGE_START_FAILED", "detail": detail}
        if self._last_publish_monotonic is None:
            return {"type": "status", "state": "waiting", "reason": "NONE", "detail": "正在等待系统诊断"}
        if time.monotonic() - self._last_publish_monotonic > self._stale_after_seconds:
            return {"type": "status", "state": "degraded", "reason": "DIAGNOSTICS_STALE", "detail": "系统诊断已超时"}
        return {"type": "status", "state": "streaming", "reason": "NONE", "detail": "系统诊断接收正常"}
