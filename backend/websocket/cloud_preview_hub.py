from __future__ import annotations

import asyncio
from dataclasses import dataclass, field

from backend.protocols.cloud_preview_v1 import (
    CloudPreviewFrame,
    status_message,
)


class ClientLimitReachedError(RuntimeError):
    """点云浏览器客户端已经达到配置上限。"""


@dataclass(eq=False, slots=True)
class CloudPreviewSession:
    """单个浏览器连接的容量为1的最新帧队列。"""

    queue: asyncio.Queue[CloudPreviewFrame] = field(
        default_factory=lambda: asyncio.Queue(maxsize=1)
    )


class CloudPreviewHub:
    """在FastAPI事件循环内广播共享PCV1帧。"""

    def __init__(
        self,
        max_clients: int = 4,
    ) -> None:
        self._max_clients = max_clients
        self._sessions: set[CloudPreviewSession] = set()
        self._latest_frame: CloudPreviewFrame | None = None
        self._ros_available = False
        self._ros_error: str | None = None
        self.overwritten_frames = 0

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

    def clear_latest_frame(self) -> None:
        """清除停用前缓存，避免下一次按需启动先发送旧点云。"""
        self._latest_frame = None

    def publish(self, frame: CloudPreviewFrame) -> None:
        self._latest_frame = frame

        for session in tuple(self._sessions):
            if session.queue.full():
                try:
                    session.queue.get_nowait()
                    self.overwritten_frames += 1
                except asyncio.QueueEmpty:
                    pass
            session.queue.put_nowait(frame)

    def register(self) -> CloudPreviewSession:
        if len(self._sessions) >= self._max_clients:
            raise ClientLimitReachedError("点云预览客户端数量已达到上限")

        session = CloudPreviewSession()
        self._sessions.add(session)
        if self._latest_frame is not None:
            session.queue.put_nowait(self._latest_frame)
        return session

    def unregister(self, session: CloudPreviewSession) -> None:
        self._sessions.discard(session)

    def current_status(self) -> dict[str, str]:
        if not self._ros_available:
            detail = "ROS桥不可用"
            if self._ros_error:
                detail = f"ROS桥不可用：{self._ros_error}"
            return status_message(
                "ros_unavailable",
                "ROS_BRIDGE_START_FAILED",
                detail,
            )

        if self._latest_frame is None:
            return status_message(
                "waiting",
                "NONE",
                "点云预览服务已连接",
            )

        return status_message("streaming", "NONE", "点云预览正常")
