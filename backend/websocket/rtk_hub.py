from __future__ import annotations

import asyncio
from dataclasses import dataclass, field

from backend.protocols.rtk_v1 import RtkSnapshot


class RtkClientLimitReachedError(RuntimeError):
    """RTK浏览器客户端已经达到配置上限。"""


@dataclass(eq=False, slots=True)
class RtkSession:
    """单个浏览器连接的容量为1的最新快照队列。"""

    queue: asyncio.Queue[RtkSnapshot] = field(
        default_factory=lambda: asyncio.Queue(maxsize=1)
    )


class RtkHub:
    """在FastAPI事件循环内广播RTK最新值。"""

    def __init__(self, max_clients: int = 4) -> None:
        self._max_clients = max_clients
        self._sessions: set[RtkSession] = set()
        self._latest_snapshot: RtkSnapshot | None = None
        self._ros_available = False
        self._ros_error: str | None = None
        self.overwritten_snapshots = 0

    @property
    def client_count(self) -> int:
        return len(self._sessions)

    def set_ros_availability(
        self,
        available: bool,
        error: str | None = None,
    ) -> None:
        self._ros_available = available
        self._ros_error = error

    def publish(self, snapshot: RtkSnapshot) -> None:
        self._latest_snapshot = snapshot
        for session in tuple(self._sessions):
            if session.queue.full():
                try:
                    session.queue.get_nowait()
                    self.overwritten_snapshots += 1
                except asyncio.QueueEmpty:
                    pass
            session.queue.put_nowait(snapshot)

    def register(self) -> RtkSession:
        if len(self._sessions) >= self._max_clients:
            raise RtkClientLimitReachedError("RTK客户端数量已达到上限")

        session = RtkSession()
        self._sessions.add(session)
        if self._latest_snapshot is not None:
            session.queue.put_nowait(self._latest_snapshot)
        return session

    def unregister(self, session: RtkSession) -> None:
        self._sessions.discard(session)

    def current_status(self) -> dict[str, str]:
        if not self._ros_available:
            detail = "RTK ROS桥不可用"
            if self._ros_error:
                detail = f"RTK ROS桥不可用：{self._ros_error}"
            return {
                "type": "status",
                "state": "ros_unavailable",
                "reason": "ROS_BRIDGE_START_FAILED",
                "detail": detail,
            }
        if self._latest_snapshot is None:
            return {
                "type": "status",
                "state": "waiting",
                "reason": "NONE",
                "detail": "正在等待RTK数据",
            }
        return {
            "type": "status",
            "state": "streaming",
            "reason": "NONE",
            "detail": "RTK数据接收正常",
        }
