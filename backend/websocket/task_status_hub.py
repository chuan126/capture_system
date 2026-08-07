from __future__ import annotations

import asyncio
from dataclasses import dataclass, field

from backend.protocols.task_status_v1 import TaskStatusSnapshot


class TaskStatusClientLimitReachedError(RuntimeError):
    pass


@dataclass(eq=False, slots=True)
class TaskStatusSession:
    queue: asyncio.Queue[TaskStatusSnapshot] = field(
        default_factory=lambda: asyncio.Queue(maxsize=1)
    )


class TaskStatusHub:
    def __init__(self, max_clients: int = 4, stale_after_seconds: float = 5.0) -> None:
        self._max_clients = max_clients
        self._sessions: set[TaskStatusSession] = set()
        self._latest_by_task: dict[str, TaskStatusSnapshot] = {}
        self._ros_available = False
        self._ros_error: str | None = None

    def set_ros_availability(self, available: bool, error: str | None = None) -> None:
        self._ros_available = available
        self._ros_error = error

    def publish(self, snapshot: TaskStatusSnapshot) -> None:
        self._latest_by_task[snapshot.task_id] = snapshot
        for session in tuple(self._sessions):
            if session.queue.full():
                try:
                    session.queue.get_nowait()
                except asyncio.QueueEmpty:
                    pass
            session.queue.put_nowait(snapshot)

    def register(self) -> TaskStatusSession:
        if len(self._sessions) >= self._max_clients:
            raise TaskStatusClientLimitReachedError()
        session = TaskStatusSession()
        self._sessions.add(session)
        latest = max(
            self._latest_by_task.values(),
            key=lambda item: item.emitted_at_ns,
            default=None,
        )
        if latest is not None:
            session.queue.put_nowait(latest)
        return session

    def unregister(self, session: TaskStatusSession) -> None:
        self._sessions.discard(session)

    def current_status(self) -> dict[str, str]:
        if not self._ros_available:
            detail = "任务控制ROS桥不可用"
            if self._ros_error:
                detail += f"：{self._ros_error}"
            return {
                "type": "status",
                "state": "ros_unavailable",
                "reason": "ROS_BRIDGE_START_FAILED",
                "detail": detail,
            }
        if not self._latest_by_task:
            return {
                "type": "status",
                "state": "waiting",
                "reason": "NONE",
                "detail": "正在等待任务状态",
            }
        return {
            "type": "status",
            "state": "streaming",
            "reason": "NONE",
            "detail": "任务状态订阅正常",
        }
