from __future__ import annotations

import sqlite3
import threading


class MeasurementQueryCancelledError(RuntimeError):
    """同一浏览器回放会话已发起更新的查询。"""


class MeasurementQueryHandle:
    def __init__(self, coordinator: "MeasurementQueryCoordinator", session_id: str) -> None:
        self._coordinator = coordinator
        self.session_id = session_id
        self._lock = threading.Lock()
        self._connection: sqlite3.Connection | None = None
        self._cancelled = False

    @property
    def cancelled(self) -> bool:
        with self._lock:
            return self._cancelled

    def bind(self, connection: sqlite3.Connection) -> None:
        with self._lock:
            self._connection = connection
            cancelled = self._cancelled
        connection.set_progress_handler(self._should_interrupt, 1_000)
        if cancelled:
            connection.interrupt()
            raise MeasurementQueryCancelledError("回放查询已被后续请求取消")

    def cancel(self) -> None:
        with self._lock:
            self._cancelled = True
            connection = self._connection
        if connection is not None:
            try:
                connection.interrupt()
            except sqlite3.ProgrammingError:
                # 查询线程可能刚好已经关闭连接。
                pass

    def unbind(self, connection: sqlite3.Connection) -> None:
        with self._lock:
            if self._connection is connection:
                self._connection = None
        try:
            connection.set_progress_handler(None, 0)
        except sqlite3.ProgrammingError:
            pass

    def close(self) -> None:
        with self._lock:
            connection = self._connection
            self._connection = None
        if connection is not None:
            try:
                connection.set_progress_handler(None, 0)
            except sqlite3.ProgrammingError:
                pass
        self._coordinator.finish(self)

    def raise_if_cancelled(self) -> None:
        if self.cancelled:
            raise MeasurementQueryCancelledError("回放查询已被后续请求取消")

    def _should_interrupt(self) -> int:
        return 1 if self.cancelled else 0


class MeasurementQueryCoordinator:
    """同一浏览器回放会话只保留最新的历史读取，防止旧查询拖慢切换和刷新。"""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._active: dict[str, MeasurementQueryHandle] = {}

    def begin(self, session_id: str | None) -> MeasurementQueryHandle | None:
        if not session_id:
            return None
        handle = MeasurementQueryHandle(self, session_id)
        with self._lock:
            previous = self._active.get(session_id)
            self._active[session_id] = handle
        if previous is not None:
            previous.cancel()
        return handle

    def finish(self, handle: MeasurementQueryHandle) -> None:
        with self._lock:
            if self._active.get(handle.session_id) is handle:
                del self._active[handle.session_id]
