from __future__ import annotations

import hashlib
import json
import sqlite3
import threading
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Literal, Sequence

from backend.tasks.models import TaskCreateRequest, TaskStatus

TaskOrder = Literal["asc", "desc"]


class TaskStorageError(RuntimeError):
    """任务数据库不可用或操作失败。"""


class TaskNotFoundError(LookupError):
    """指定任务不存在。"""


class TaskIdempotencyConflictError(RuntimeError):
    """幂等键已用于不同的创建请求。"""


class TaskDeleteConflictError(RuntimeError):
    """任务当前状态不允许删除。"""


@dataclass(frozen=True)
class TaskRecord:
    task_id: str
    sequence: int
    tunnel_code: str
    tunnel_name: str
    status: TaskStatus
    created_at: str
    updated_at: str
    started_at: str | None
    completed_at: str | None
    has_measurements: bool
    recording_path: str | None
    schema_version: int
    deleted_at: str | None
    delete_reason: str | None

    @property
    def display_sequence(self) -> str:
        return str(self.sequence).zfill(2)


_SCHEMA_VERSION = 2
_ALLOWED_STATUS = {
    "pending",
    "running",
    "paused",
    "completed",
    "interrupted",
    "failed",
}


class TaskRepository:
    def __init__(self, database_path: Path, tasks_directory: Path) -> None:
        self.database_path = database_path
        self.tasks_directory = tasks_directory
        self._initialization_lock = threading.Lock()
        self._initialized = False
        self._initialization_error: str | None = None

    @property
    def initialization_error(self) -> str | None:
        return self._initialization_error

    def initialize(self) -> None:
        with self._initialization_lock:
            if self._initialized:
                return
            try:
                self.database_path.parent.mkdir(parents=True, exist_ok=True)
                self.tasks_directory.mkdir(parents=True, exist_ok=True)
                with self._connect() as connection:
                    connection.execute(
                        """
                        CREATE TABLE IF NOT EXISTS schema_migrations (
                            version INTEGER PRIMARY KEY,
                            applied_at TEXT NOT NULL
                        )
                        """
                    )
                    current_version = connection.execute(
                        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"
                    ).fetchone()[0]
                    if current_version < 1:
                        self._apply_migration_1(connection)
                    current_version = connection.execute(
                        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"
                    ).fetchone()[0]
                    if current_version < 2:
                        self._apply_migration_2(connection)
                    current_version = connection.execute(
                        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"
                    ).fetchone()[0]
                    if current_version != _SCHEMA_VERSION:
                        raise TaskStorageError(
                            f"不支持的任务数据库版本：{current_version}，程序支持版本：{_SCHEMA_VERSION}"
                        )
                self._initialized = True
                self._initialization_error = None
            except (OSError, sqlite3.Error, TaskStorageError) as error:
                self._initialized = False
                self._initialization_error = str(error)
                raise TaskStorageError(f"任务数据库初始化失败：{error}") from error

    def create_tasks(
        self,
        drafts: Sequence[TaskCreateRequest],
        *,
        idempotency_key: str | None = None,
    ) -> list[TaskRecord]:
        self._ensure_initialized()
        normalized_key = self._normalize_idempotency_key(idempotency_key)
        request_hash = self._request_hash(drafts)
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            if normalized_key is not None:
                repeated = self._load_idempotent_response(
                    connection,
                    normalized_key,
                    request_hash,
                )
                if repeated is not None:
                    connection.commit()
                    return repeated

            first_sequence = connection.execute(
                "SELECT COALESCE(MAX(sequence), 0) + 1 FROM tasks"
            ).fetchone()[0]
            now = _utc_now_text()
            created: list[TaskRecord] = []
            for index, draft in enumerate(drafts):
                task_id = str(uuid.uuid4())
                sequence = first_sequence + index
                connection.execute(
                    """
                    INSERT INTO tasks (
                        task_id,
                        sequence,
                        tunnel_code,
                        tunnel_name,
                        status,
                        created_at,
                        updated_at,
                        started_at,
                        completed_at,
                        has_measurements,
                        recording_path,
                        schema_version,
                        deleted_at,
                        delete_reason
                    ) VALUES (?, ?, ?, ?, 'pending', ?, ?, NULL, NULL, 0, NULL, ?, NULL, NULL)
                    """,
                    (
                        task_id,
                        sequence,
                        draft.tunnel_code,
                        draft.tunnel_name,
                        now,
                        now,
                        _SCHEMA_VERSION,
                    ),
                )
                row = connection.execute(
                    "SELECT * FROM tasks WHERE task_id = ?",
                    (task_id,),
                ).fetchone()
                created.append(self._row_to_record(row))

            if normalized_key is not None:
                connection.execute(
                    """
                    INSERT INTO task_idempotency (
                        idempotency_key,
                        request_hash,
                        response_task_ids,
                        created_at
                    ) VALUES (?, ?, ?, ?)
                    """,
                    (
                        normalized_key,
                        request_hash,
                        json.dumps([task.task_id for task in created]),
                        now,
                    ),
                )
            connection.commit()
            return created
        except TaskIdempotencyConflictError:
            connection.rollback()
            raise
        except (OSError, sqlite3.Error) as error:
            connection.rollback()
            raise TaskStorageError(f"创建任务失败：{error}") from error
        finally:
            connection.close()

    def list_tasks(
        self,
        *,
        status: TaskStatus | None = None,
        has_measurements: bool | None = None,
        limit: int = 100,
        offset: int = 0,
        order: TaskOrder = "asc",
        include_deleted: bool = False,
    ) -> list[TaskRecord]:
        self._ensure_initialized()
        if status is not None and status not in _ALLOWED_STATUS:
            raise ValueError(f"不支持的任务状态：{status}")
        direction = "ASC" if order == "asc" else "DESC"
        conditions: list[str] = []
        parameters: list[object] = []
        if not include_deleted:
            conditions.append("deleted_at IS NULL")
        if status is not None:
            conditions.append("status = ?")
            parameters.append(status)
        if has_measurements is not None:
            conditions.append("has_measurements = ?")
            parameters.append(1 if has_measurements else 0)
        where_clause = f"WHERE {' AND '.join(conditions)}" if conditions else ""
        parameters.extend([limit, offset])
        try:
            with self._connect() as connection:
                rows = connection.execute(
                    f"""
                    SELECT * FROM tasks
                    {where_clause}
                    ORDER BY sequence {direction}
                    LIMIT ? OFFSET ?
                    """,
                    parameters,
                ).fetchall()
            return [self._row_to_record(row) for row in rows]
        except (OSError, sqlite3.Error) as error:
            raise TaskStorageError(f"读取任务列表失败：{error}") from error

    def get_task(self, task_id: str, *, include_deleted: bool = False) -> TaskRecord:
        self._ensure_initialized()
        deleted_clause = "" if include_deleted else "AND deleted_at IS NULL"
        try:
            with self._connect() as connection:
                row = connection.execute(
                    f"SELECT * FROM tasks WHERE task_id = ? {deleted_clause}",
                    (task_id,),
                ).fetchone()
        except (OSError, sqlite3.Error) as error:
            raise TaskStorageError(f"读取任务失败：{error}") from error
        if row is None:
            raise TaskNotFoundError(task_id)
        return self._row_to_record(row)

    def soft_delete_task(self, task_id: str, *, reason: str = "user_request") -> TaskRecord:
        self._ensure_initialized()
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT * FROM tasks WHERE task_id = ? AND deleted_at IS NULL",
                (task_id,),
            ).fetchone()
            if row is None:
                raise TaskNotFoundError(task_id)
            record = self._row_to_record(row)
            if record.status in {"running", "paused"}:
                raise TaskDeleteConflictError("采集中或已暂停的任务不能删除，请先正常结束任务")
            now = _utc_now_text()
            connection.execute(
                """
                UPDATE tasks
                SET deleted_at = ?, delete_reason = ?, updated_at = ?
                WHERE task_id = ? AND deleted_at IS NULL
                """,
                (now, reason, now, task_id),
            )
            deleted_row = connection.execute(
                "SELECT * FROM tasks WHERE task_id = ?",
                (task_id,),
            ).fetchone()
            connection.commit()
            return self._row_to_record(deleted_row)
        except (TaskNotFoundError, TaskDeleteConflictError):
            connection.rollback()
            raise
        except (OSError, sqlite3.Error) as error:
            connection.rollback()
            raise TaskStorageError(f"删除任务失败：{error}") from error
        finally:
            connection.close()

    def _ensure_initialized(self) -> None:
        if self._initialized:
            return
        try:
            self.initialize()
        except TaskStorageError:
            detail = self._initialization_error or "未知错误"
            raise TaskStorageError(f"任务数据库不可用：{detail}") from None

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(
            self.database_path,
            timeout=5.0,
            isolation_level=None,
        )
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("PRAGMA busy_timeout = 5000")
        connection.execute("PRAGMA journal_mode = WAL")
        return connection

    def _apply_migration_1(self, connection: sqlite3.Connection) -> None:
        connection.execute("BEGIN IMMEDIATE")
        try:
            connection.execute(
                """
                CREATE TABLE tasks (
                    task_id TEXT PRIMARY KEY,
                    sequence INTEGER NOT NULL UNIQUE CHECK (sequence > 0),
                    tunnel_code TEXT NOT NULL CHECK (length(tunnel_code) BETWEEN 1 AND 128),
                    tunnel_name TEXT NOT NULL CHECK (length(tunnel_name) BETWEEN 1 AND 256),
                    status TEXT NOT NULL DEFAULT 'pending'
                        CHECK (status IN ('pending', 'running', 'paused', 'completed', 'interrupted', 'failed')),
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    started_at TEXT,
                    completed_at TEXT,
                    has_measurements INTEGER NOT NULL DEFAULT 0 CHECK (has_measurements IN (0, 1)),
                    recording_path TEXT,
                    schema_version INTEGER NOT NULL DEFAULT 1 CHECK (schema_version > 0)
                )
                """
            )
            connection.execute(
                """
                CREATE TABLE task_idempotency (
                    idempotency_key TEXT PRIMARY KEY,
                    request_hash TEXT NOT NULL,
                    response_task_ids TEXT NOT NULL,
                    created_at TEXT NOT NULL
                )
                """
            )
            connection.execute(
                "CREATE INDEX tasks_status_sequence_idx ON tasks(status, sequence)"
            )
            connection.execute(
                "CREATE INDEX tasks_measurements_sequence_idx ON tasks(has_measurements, sequence)"
            )
            connection.execute(
                "INSERT INTO schema_migrations(version, applied_at) VALUES (?, ?)",
                (1, _utc_now_text()),
            )
            connection.commit()
        except Exception:
            connection.rollback()
            raise

    def _apply_migration_2(self, connection: sqlite3.Connection) -> None:
        connection.execute("BEGIN IMMEDIATE")
        try:
            connection.execute("ALTER TABLE tasks ADD COLUMN deleted_at TEXT")
            connection.execute("ALTER TABLE tasks ADD COLUMN delete_reason TEXT")
            connection.execute(
                "CREATE INDEX tasks_deleted_sequence_idx ON tasks(deleted_at, sequence)"
            )
            connection.execute(
                "INSERT INTO schema_migrations(version, applied_at) VALUES (?, ?)",
                (2, _utc_now_text()),
            )
            connection.commit()
        except Exception:
            connection.rollback()
            raise

    def _load_idempotent_response(
        self,
        connection: sqlite3.Connection,
        idempotency_key: str,
        request_hash: str,
    ) -> list[TaskRecord] | None:
        row = connection.execute(
            """
            SELECT request_hash, response_task_ids
            FROM task_idempotency
            WHERE idempotency_key = ?
            """,
            (idempotency_key,),
        ).fetchone()
        if row is None:
            return None
        if row["request_hash"] != request_hash:
            raise TaskIdempotencyConflictError(
                "该幂等键已经用于不同的任务创建请求"
            )
        task_ids = json.loads(row["response_task_ids"])
        tasks_by_id = {
            task.task_id: task
            for task in self._load_tasks_by_ids(connection, task_ids)
        }
        try:
            return [tasks_by_id[task_id] for task_id in task_ids]
        except KeyError as error:
            raise TaskStorageError("幂等记录引用的任务不存在") from error

    def _load_tasks_by_ids(
        self,
        connection: sqlite3.Connection,
        task_ids: Iterable[str],
    ) -> list[TaskRecord]:
        identifiers = list(task_ids)
        if not identifiers:
            return []
        placeholders = ",".join("?" for _ in identifiers)
        rows = connection.execute(
            f"SELECT * FROM tasks WHERE task_id IN ({placeholders})",
            identifiers,
        ).fetchall()
        return [self._row_to_record(row) for row in rows]

    @staticmethod
    def _row_to_record(row: sqlite3.Row) -> TaskRecord:
        return TaskRecord(
            task_id=row["task_id"],
            sequence=row["sequence"],
            tunnel_code=row["tunnel_code"],
            tunnel_name=row["tunnel_name"],
            status=row["status"],
            created_at=row["created_at"],
            updated_at=row["updated_at"],
            started_at=row["started_at"],
            completed_at=row["completed_at"],
            has_measurements=bool(row["has_measurements"]),
            recording_path=row["recording_path"],
            schema_version=row["schema_version"],
            deleted_at=row["deleted_at"],
            delete_reason=row["delete_reason"],
        )

    @staticmethod
    def _request_hash(drafts: Sequence[TaskCreateRequest]) -> str:
        payload = [
            {
                "tunnel_code": draft.tunnel_code,
                "tunnel_name": draft.tunnel_name,
            }
            for draft in drafts
        ]
        canonical = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        return hashlib.sha256(canonical.encode("utf-8")).hexdigest()

    @staticmethod
    def _normalize_idempotency_key(value: str | None) -> str | None:
        if value is None:
            return None
        normalized = value.strip()
        if not normalized:
            return None
        if len(normalized) > 128:
            raise TaskIdempotencyConflictError("幂等键不能超过128个字符")
        if any(ord(character) < 33 or ord(character) > 126 for character in normalized):
            raise TaskIdempotencyConflictError("幂等键只能包含可见ASCII字符")
        return normalized


def _utc_now_text() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
