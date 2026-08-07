from __future__ import annotations

import hashlib
import json
import os
import shutil
import sqlite3
import threading
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Literal, Sequence
from zoneinfo import ZoneInfo

from backend.tasks.models import TaskCreateRequest, TaskOperationPhase, TaskStatus

TaskOrder = Literal["asc", "desc"]


class TaskStorageError(RuntimeError):
    """任务数据库不可用或操作失败。"""


class TaskNotFoundError(LookupError):
    """指定任务不存在。"""


class TaskIdempotencyConflictError(RuntimeError):
    """幂等键已用于不同的创建请求。"""


class TaskDeleteConflictError(RuntimeError):
    """任务当前状态不允许删除。"""


class BatchNotFoundError(LookupError):
    """指定作业批次不存在。"""


class BatchConflictError(RuntimeError):
    """作业批次当前状态不允许操作。"""


@dataclass(frozen=True)
class BatchRecord:
    batch_id: str
    batch_code: str
    operation_date: str
    daily_sequence: int
    status: str
    created_at: str
    started_at: str
    completed_at: str | None
    archived_at: str | None
    purged_at: str | None
    task_count: int
    visible_task_count: int
    measurement_bytes: int
    report_id: str | None
    report_path: str | None
    report_sha256: str | None
    report_generated_at: str | None
    purged_bytes: int


@dataclass(frozen=True)
class BatchPurgeResult:
    batch: BatchRecord
    released_bytes: int
    removed_task_count: int


@dataclass(frozen=True)
class TaskRecord:
    task_id: str
    display_id: str
    sequence: int
    global_sequence: int
    batch_id: str
    batch_code: str
    tunnel_code: str
    tunnel_name: str
    status: TaskStatus
    operation_phase: TaskOperationPhase
    status_revision: int
    created_at: str
    updated_at: str
    start_requested_at: str | None
    started_at: str | None
    stop_requested_at: str | None
    completed_at: str | None
    entry_rtk_status: str
    exit_rtk_status: str
    has_measurements: bool
    recording_path: str | None
    local_data_purged_at: str | None
    purged_bytes: int
    last_error_code: str | None
    last_error_message: str | None
    warning_code: str | None
    active_session_id: str | None
    active_slot: int | None
    schema_version: int
    deleted_at: str | None
    delete_reason: str | None

    @property
    def display_sequence(self) -> str:
        return self.display_id


_SCHEMA_VERSION = 6
_LOCAL_TIMEZONE = ZoneInfo("Asia/Singapore")
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
                    if current_version < 3:
                        self._apply_migration_3(connection)
                    current_version = connection.execute(
                        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"
                    ).fetchone()[0]
                    if current_version < 4:
                        self._apply_migration_4(connection)
                    current_version = connection.execute(
                        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"
                    ).fetchone()[0]
                    if current_version < 5:
                        self._apply_migration_5(connection)
                    current_version = connection.execute(
                        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"
                    ).fetchone()[0]
                    if current_version < 6:
                        self._apply_migration_6(connection)
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
        batch_mode: Literal["current", "new"] = "current",
        auto_create_batch: bool = False,
    ) -> list[TaskRecord]:
        """创建任务。

        batch_mode/auto_create_batch 仅保留旧调用兼容。新任务不再向用户暴露作业批次，
        数据库内部仍挂接一个兼容批次，以兼容旧数据库和既有 ROS 2 SQL。
        """
        del batch_mode, auto_create_batch
        self._ensure_initialized()
        normalized_key = self._normalize_idempotency_key(idempotency_key)
        request_hash = self._request_hash(drafts)
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            if normalized_key is not None:
                repeated = self._load_idempotent_response(connection, normalized_key, request_hash)
                if repeated is not None:
                    connection.commit()
                    return repeated

            batch_row = connection.execute(
                "SELECT * FROM operation_batches WHERE status='active' AND active_slot=1 ORDER BY created_at LIMIT 1"
            ).fetchone()
            if batch_row is None:
                batch_row = self._create_batch_in_transaction(connection)
            batch_id = batch_row["batch_id"]

            first_global_sequence = int(connection.execute(
                "SELECT COALESCE(MAX(sequence), 0) + 1 FROM tasks"
            ).fetchone()[0])
            first_batch_sequence = int(connection.execute(
                "SELECT COALESCE(MAX(batch_sequence), 0) + 1 FROM tasks WHERE batch_id=?",
                (batch_id,),
            ).fetchone()[0])
            now = _utc_now_text()
            created: list[TaskRecord] = []
            used_display_ids = {
                str(row[0]) for row in connection.execute(
                    "SELECT display_id FROM tasks WHERE display_id IS NOT NULL"
                ).fetchall()
            }
            for index, draft in enumerate(drafts):
                task_id = str(uuid.uuid4())
                global_sequence = first_global_sequence + index
                batch_sequence = first_batch_sequence + index
                display_id = _allocate_display_id(now, used_display_ids)
                used_display_ids.add(display_id)
                connection.execute(
                    """
                    INSERT INTO tasks (
                        task_id, sequence, batch_id, batch_sequence, display_id, tunnel_code, tunnel_name,
                        status, created_at, updated_at, started_at, completed_at,
                        has_measurements, recording_path, schema_version, deleted_at, delete_reason,
                        local_data_purged_at, purged_bytes
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, 'pending', ?, ?, NULL, NULL, 0, NULL, ?, NULL, NULL, NULL, 0)
                    """,
                    (task_id, global_sequence, batch_id, batch_sequence, display_id, draft.tunnel_code,
                     draft.tunnel_name, now, now, _SCHEMA_VERSION),
                )
                row = connection.execute(
                    "SELECT tasks.*, operation_batches.batch_code FROM tasks "
                    "JOIN operation_batches ON operation_batches.batch_id=tasks.batch_id "
                    "WHERE task_id = ?", (task_id,),
                ).fetchone()
                created.append(self._row_to_record(row))

            connection.execute(
                "UPDATE operation_batches SET task_count=task_count+?, updated_at=? WHERE batch_id=?",
                (len(created), now, batch_id),
            )
            if normalized_key is not None:
                connection.execute(
                    """
                    INSERT INTO task_idempotency (idempotency_key, request_hash, response_task_ids, created_at)
                    VALUES (?, ?, ?, ?)
                    """,
                    (normalized_key, request_hash, json.dumps([task.task_id for task in created]), now),
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
        batch_id: str | None = None,
    ) -> list[TaskRecord]:
        self._ensure_initialized()
        if status is not None and status not in _ALLOWED_STATUS:
            raise ValueError(f"不支持的任务状态：{status}")
        direction = "ASC" if order == "asc" else "DESC"
        conditions: list[str] = []
        parameters: list[object] = []
        if not include_deleted:
            conditions.append("tasks.deleted_at IS NULL")
        if status is not None:
            conditions.append("tasks.status = ?")
            parameters.append(status)
        if has_measurements is not None:
            conditions.append("tasks.has_measurements = ?")
            parameters.append(1 if has_measurements else 0)
        if batch_id is not None:
            conditions.append("tasks.batch_id = ?")
            parameters.append(batch_id)
        where_clause = f"WHERE {' AND '.join(conditions)}" if conditions else ""
        parameters.extend([limit, offset])
        try:
            with self._connect() as connection:
                rows = connection.execute(
                    f"""
                    SELECT tasks.*, operation_batches.batch_code
                    FROM tasks
                    JOIN operation_batches ON operation_batches.batch_id = tasks.batch_id
                    {where_clause}
                    ORDER BY tasks.created_at {direction}, tasks.sequence {direction}
                    LIMIT ? OFFSET ?
                    """,
                    parameters,
                ).fetchall()
            return [self._row_to_record(row) for row in rows]
        except (OSError, sqlite3.Error) as error:
            raise TaskStorageError(f"读取任务列表失败：{error}") from error

    def get_task(self, task_id: str, *, include_deleted: bool = False) -> TaskRecord:
        self._ensure_initialized()
        deleted_clause = "" if include_deleted else "AND tasks.deleted_at IS NULL"
        try:
            with self._connect() as connection:
                row = connection.execute(
                    f"SELECT tasks.*, operation_batches.batch_code FROM tasks "
                    f"JOIN operation_batches ON operation_batches.batch_id=tasks.batch_id "
                    f"WHERE tasks.task_id = ? {deleted_clause}",
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
                "SELECT tasks.*, operation_batches.batch_code FROM tasks "
                "JOIN operation_batches ON operation_batches.batch_id=tasks.batch_id "
                "WHERE task_id = ? AND deleted_at IS NULL",
                (task_id,),
            ).fetchone()
            if row is None:
                raise TaskNotFoundError(task_id)
            record = self._row_to_record(row)
            if record.status in {"running", "paused"} or record.active_slot is not None:
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
                "SELECT tasks.*, operation_batches.batch_code FROM tasks "
                "JOIN operation_batches ON operation_batches.batch_id=tasks.batch_id "
                "WHERE task_id = ?",
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

    def purge_task_data(self, task_ids: Sequence[str]) -> tuple[list[str], int]:
        self._ensure_initialized()
        identifiers = list(dict.fromkeys(task_ids))
        if not identifiers:
            raise ValueError("至少需要一个任务ID")
        connection = self._connect()
        released_bytes = 0
        try:
            connection.execute("BEGIN IMMEDIATE")
            placeholders = ",".join("?" for _ in identifiers)
            rows = connection.execute(
                f"SELECT task_id, status, active_slot FROM tasks WHERE task_id IN ({placeholders}) AND deleted_at IS NULL",
                identifiers,
            ).fetchall()
            found = {row["task_id"] for row in rows}
            missing = [task_id for task_id in identifiers if task_id not in found]
            if missing:
                raise TaskNotFoundError(missing[0])
            blocked = [row["task_id"] for row in rows if row["status"] in {"running", "paused"} or row["active_slot"] is not None]
            if blocked:
                raise TaskDeleteConflictError("活动任务不能清理本地数据，请先停止任务")

            sizes: dict[str, int] = {}
            for task_id in identifiers:
                directory = (self.tasks_directory / task_id).resolve()
                try:
                    directory.relative_to(self.tasks_directory.resolve())
                except ValueError as error:
                    raise TaskStorageError("任务数据目录超出配置的数据根目录") from error
                size = _directory_size(directory)
                if directory.exists():
                    try:
                        shutil.rmtree(directory)
                    except OSError as error:
                        raise TaskStorageError(f"清理任务 {task_id} 数据失败：{error}") from error
                sizes[task_id] = size
                released_bytes += size

            now = _utc_now_text()
            for task_id in identifiers:
                connection.execute(
                    """
                    UPDATE tasks
                    SET has_measurements=0, recording_path=NULL, local_data_purged_at=?,
                        purged_bytes=COALESCE(purged_bytes,0)+?, updated_at=?
                    WHERE task_id=?
                    """,
                    (now, sizes[task_id], now, task_id),
                )
            connection.commit()
            return identifiers, released_bytes
        except (TaskNotFoundError, TaskDeleteConflictError, TaskStorageError):
            connection.rollback()
            raise
        except (OSError, sqlite3.Error) as error:
            connection.rollback()
            raise TaskStorageError(f"清理任务数据失败：{error}") from error
        finally:
            connection.close()

    def list_batches(self) -> list[BatchRecord]:
        self._ensure_initialized()
        try:
            with self._connect() as connection:
                rows = connection.execute(
                    "SELECT * FROM operation_batches ORDER BY created_at DESC, daily_sequence DESC"
                ).fetchall()
                return [self._row_to_batch(connection, row) for row in rows]
        except (OSError, sqlite3.Error) as error:
            raise TaskStorageError(f"读取作业批次失败：{error}") from error

    def get_batch(self, batch_id: str) -> BatchRecord:
        self._ensure_initialized()
        try:
            with self._connect() as connection:
                row = connection.execute(
                    "SELECT * FROM operation_batches WHERE batch_id=?",
                    (batch_id,),
                ).fetchone()
                if row is None:
                    raise BatchNotFoundError(batch_id)
                return self._row_to_batch(connection, row)
        except BatchNotFoundError:
            raise
        except (OSError, sqlite3.Error) as error:
            raise TaskStorageError(f"读取作业批次失败：{error}") from error

    def get_active_batch(self) -> BatchRecord | None:
        self._ensure_initialized()
        try:
            with self._connect() as connection:
                row = connection.execute(
                    "SELECT * FROM operation_batches WHERE status='active' AND active_slot=1"
                ).fetchone()
                return self._row_to_batch(connection, row) if row is not None else None
        except (OSError, sqlite3.Error) as error:
            raise TaskStorageError(f"读取当前作业批次失败：{error}") from error

    def _create_batch_in_transaction(self, connection: sqlite3.Connection) -> sqlite3.Row:
        now = _utc_now_text()
        local_date = datetime.now(_LOCAL_TIMEZONE).date()
        operation_date = local_date.isoformat()
        daily_sequence = int(connection.execute(
            "SELECT COALESCE(MAX(daily_sequence),0)+1 FROM operation_batches WHERE operation_date=?",
            (operation_date,),
        ).fetchone()[0])
        batch_code = f"{local_date:%Y%m%d}-{daily_sequence:02d}"
        batch_id = str(uuid.uuid4())
        connection.execute(
            """
            INSERT INTO operation_batches (
                batch_id, batch_code, operation_date, daily_sequence, status, active_slot,
                created_at, updated_at, started_at, completed_at, archived_at, purged_at,
                task_count, report_id, report_path, report_sha256, report_generated_at, purged_bytes
            ) VALUES (?, ?, ?, ?, 'active', 1, ?, ?, ?, NULL, NULL, NULL, 0, NULL, NULL, NULL, NULL, 0)
            """,
            (batch_id, batch_code, operation_date, daily_sequence, now, now, now),
        )
        return connection.execute(
            "SELECT * FROM operation_batches WHERE batch_id=?", (batch_id,),
        ).fetchone()

    def _complete_batch_in_transaction(
        self, connection: sqlite3.Connection, batch_id: str,
    ) -> sqlite3.Row:
        row = connection.execute(
            "SELECT * FROM operation_batches WHERE batch_id=?", (batch_id,),
        ).fetchone()
        if row is None:
            raise BatchNotFoundError(batch_id)
        if row["status"] != "active":
            raise BatchConflictError("只有进行中的作业可以结束")
        unfinished = int(connection.execute(
            """
            SELECT COUNT(*) FROM tasks
            WHERE batch_id=? AND deleted_at IS NULL
              AND (status IN ('pending','running','paused') OR active_slot IS NOT NULL)
            """, (batch_id,),
        ).fetchone()[0])
        if unfinished > 0:
            raise BatchConflictError(f"当前作业仍有 {unfinished} 个未结束任务")
        now = _utc_now_text()
        connection.execute(
            "UPDATE operation_batches SET status='completed', active_slot=NULL, completed_at=?, updated_at=? WHERE batch_id=?",
            (now, now, batch_id),
        )
        return connection.execute(
            "SELECT * FROM operation_batches WHERE batch_id=?", (batch_id,),
        ).fetchone()

    def create_batch(self) -> BatchRecord:
        self._ensure_initialized()
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            active = connection.execute(
                "SELECT batch_code FROM operation_batches WHERE active_slot=1"
            ).fetchone()
            if active is not None:
                raise BatchConflictError(f"作业 {active['batch_code']} 尚未结束")
            row = self._create_batch_in_transaction(connection)
            connection.commit()
            return self._row_to_batch(connection, row)
        except BatchConflictError:
            connection.rollback()
            raise
        except (OSError, sqlite3.Error) as error:
            connection.rollback()
            raise TaskStorageError(f"创建作业批次失败：{error}") from error
        finally:
            connection.close()

    def complete_batch(self, batch_id: str) -> BatchRecord:
        self._ensure_initialized()
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            updated = self._complete_batch_in_transaction(connection, batch_id)
            connection.commit()
            return self._row_to_batch(connection, updated)
        except (BatchNotFoundError, BatchConflictError):
            connection.rollback()
            raise
        except (OSError, sqlite3.Error) as error:
            connection.rollback()
            raise TaskStorageError(f"结束作业批次失败：{error}") from error
        finally:
            connection.close()

    def archive_batch(self, batch_id: str) -> BatchRecord:
        self._ensure_initialized()
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT * FROM operation_batches WHERE batch_id=?", (batch_id,)
            ).fetchone()
            if row is None:
                raise BatchNotFoundError(batch_id)
            if row["status"] not in {"completed", "archived"}:
                raise BatchConflictError("只有已结束的作业可以暂存")
            if not row["report_path"] or not row["report_sha256"]:
                raise BatchConflictError("请先成功生成本批次 PDF 汇总报告")
            report_path = self.database_path.parent / row["report_path"]
            if not report_path.is_file():
                raise BatchConflictError("本批次 PDF 报告文件不存在，不能暂存")
            now = _utc_now_text()
            connection.execute(
                "UPDATE operation_batches SET status='archived', archived_at=COALESCE(archived_at,?), updated_at=? WHERE batch_id=?",
                (now, now, batch_id),
            )
            updated = connection.execute(
                "SELECT * FROM operation_batches WHERE batch_id=?", (batch_id,)
            ).fetchone()
            connection.commit()
            return self._row_to_batch(connection, updated)
        except (BatchNotFoundError, BatchConflictError):
            connection.rollback()
            raise
        except (OSError, sqlite3.Error) as error:
            connection.rollback()
            raise TaskStorageError(f"暂存作业批次失败：{error}") from error
        finally:
            connection.close()

    def record_batch_report(
        self,
        batch_id: str,
        *,
        report_id: str,
        report_path: Path,
        report_sha256: str,
        generated_at: str,
    ) -> BatchRecord:
        self._ensure_initialized()
        try:
            relative_path = report_path.resolve().relative_to(self.database_path.parent.resolve()).as_posix()
        except ValueError as error:
            raise TaskStorageError("报告文件不在任务数据目录内") from error
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT status FROM operation_batches WHERE batch_id=?", (batch_id,)
            ).fetchone()
            if row is None:
                raise BatchNotFoundError(batch_id)
            if row["status"] == "purged":
                raise BatchConflictError("已清理的作业不能更新报告")
            now = _utc_now_text()
            connection.execute(
                """
                UPDATE operation_batches
                SET report_id=?, report_path=?, report_sha256=?, report_generated_at=?, updated_at=?
                WHERE batch_id=?
                """,
                (report_id, relative_path, report_sha256, generated_at, now, batch_id),
            )
            updated = connection.execute(
                "SELECT * FROM operation_batches WHERE batch_id=?", (batch_id,)
            ).fetchone()
            connection.commit()
            return self._row_to_batch(connection, updated)
        except (BatchNotFoundError, BatchConflictError):
            connection.rollback()
            raise
        except (OSError, sqlite3.Error) as error:
            connection.rollback()
            raise TaskStorageError(f"更新批次报告信息失败：{error}") from error
        finally:
            connection.close()

    def purge_batch(self, batch_id: str) -> BatchPurgeResult:
        self._ensure_initialized()
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            batch_row = connection.execute(
                "SELECT * FROM operation_batches WHERE batch_id=?", (batch_id,)
            ).fetchone()
            if batch_row is None:
                raise BatchNotFoundError(batch_id)
            if batch_row["status"] != "archived":
                raise BatchConflictError("只有已暂存的作业可以清理")
            if not batch_row["report_path"] or not batch_row["report_sha256"]:
                raise BatchConflictError("请先成功生成并暂存本批次 PDF 汇总报告")
            report_path = self.database_path.parent / batch_row["report_path"]
            if not report_path.is_file():
                raise BatchConflictError("本批次 PDF 报告文件不存在，不能清理")

            task_rows = connection.execute(
                "SELECT task_id FROM tasks WHERE batch_id=?", (batch_id,)
            ).fetchall()
            task_ids = [row["task_id"] for row in task_rows]
            released_bytes = sum(
                self._directory_size(self.tasks_directory / task_id) for task_id in task_ids
            )

            errors: list[str] = []
            for task_id in task_ids:
                task_directory = self.tasks_directory / task_id
                if not task_directory.exists():
                    continue
                try:
                    shutil.rmtree(task_directory, ignore_errors=False)
                except OSError as error:
                    errors.append(f"{task_id}: {error}")
            if errors:
                connection.rollback()
                raise TaskStorageError(
                    "部分任务目录清理失败，批次仍保持已暂存，可修复文件权限后重试："
                    + "；".join(errors)
                )

            now = _utc_now_text()
            connection.execute(
                """
                UPDATE tasks
                SET deleted_at=COALESCE(deleted_at,?), delete_reason='batch_purge',
                    has_measurements=0, recording_path=NULL, updated_at=?
                WHERE batch_id=?
                """,
                (now, now, batch_id),
            )
            connection.execute(
                """
                UPDATE operation_batches
                SET status='purged', active_slot=NULL, purged_at=?, purged_bytes=?, updated_at=?
                WHERE batch_id=?
                """,
                (now, released_bytes, now, batch_id),
            )
            connection.commit()
        except (BatchNotFoundError, BatchConflictError, TaskStorageError):
            connection.rollback()
            raise
        except (OSError, sqlite3.Error) as error:
            connection.rollback()
            raise TaskStorageError(
                "任务目录已经清理，但批次索引更新失败，请重试清理操作：" + str(error)
            ) from error
        finally:
            connection.close()

        batch = self.get_batch(batch_id)
        return BatchPurgeResult(
            batch=batch,
            released_bytes=released_bytes,
            removed_task_count=len(task_ids),
        )

    def _row_to_batch(self, connection: sqlite3.Connection, row: sqlite3.Row) -> BatchRecord:
        visible_task_count = int(connection.execute(
            "SELECT COUNT(*) FROM tasks WHERE batch_id=? AND deleted_at IS NULL",
            (row["batch_id"],),
        ).fetchone()[0])
        task_ids = [item[0] for item in connection.execute(
            "SELECT task_id FROM tasks WHERE batch_id=? AND deleted_at IS NULL",
            (row["batch_id"],),
        ).fetchall()]
        measurement_bytes = sum(
            self._directory_size(self.tasks_directory / task_id) for task_id in task_ids
        )
        return BatchRecord(
            batch_id=row["batch_id"],
            batch_code=row["batch_code"],
            operation_date=row["operation_date"],
            daily_sequence=int(row["daily_sequence"]),
            status=row["status"],
            created_at=row["created_at"],
            started_at=row["started_at"],
            completed_at=row["completed_at"],
            archived_at=row["archived_at"],
            purged_at=row["purged_at"],
            task_count=int(row["task_count"]),
            visible_task_count=visible_task_count,
            measurement_bytes=measurement_bytes,
            report_id=row["report_id"],
            report_path=row["report_path"],
            report_sha256=row["report_sha256"],
            report_generated_at=row["report_generated_at"],
            purged_bytes=int(row["purged_bytes"] or 0),
        )

    @staticmethod
    def _directory_size(path: Path) -> int:
        if not path.exists():
            return 0
        if path.is_file():
            try:
                return path.stat().st_size
            except OSError:
                return 0
        total = 0
        for root, _, files in os.walk(path):
            for name in files:
                try:
                    total += (Path(root) / name).stat().st_size
                except OSError:
                    continue
        return total

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

    def _apply_migration_3(self, connection: sqlite3.Connection) -> None:
        connection.execute("BEGIN IMMEDIATE")
        try:
            columns = {
                row[1]
                for row in connection.execute("PRAGMA table_info(tasks)").fetchall()
            }
            additions = [
                ("operation_phase", "TEXT NOT NULL DEFAULT 'idle'"),
                ("status_revision", "INTEGER NOT NULL DEFAULT 0"),
                ("active_session_id", "TEXT"),
                ("active_slot", "INTEGER"),
                ("start_requested_at", "TEXT"),
                ("stop_requested_at", "TEXT"),
                ("entry_rtk_status", "TEXT NOT NULL DEFAULT 'not_requested'"),
                ("exit_rtk_status", "TEXT NOT NULL DEFAULT 'not_requested'"),
                ("last_error_code", "TEXT"),
                ("last_error_message", "TEXT"),
                ("warning_code", "TEXT"),
            ]
            for name, definition in additions:
                if name not in columns:
                    connection.execute(f"ALTER TABLE tasks ADD COLUMN {name} {definition}")

            connection.execute(
                "CREATE UNIQUE INDEX IF NOT EXISTS tasks_single_active_idx "
                "ON tasks(active_slot) WHERE active_slot IS NOT NULL"
            )
            connection.execute(
                "CREATE INDEX IF NOT EXISTS tasks_phase_sequence_idx "
                "ON tasks(operation_phase, sequence)"
            )
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS task_parameters (
                    task_id TEXT PRIMARY KEY REFERENCES tasks(task_id),
                    lane TEXT NOT NULL CHECK (lane IN ('left', 'right')),
                    lidar_mount_height_m REAL NOT NULL CHECK (lidar_mount_height_m > 0),
                    clearance_threshold_m REAL NOT NULL CHECK (clearance_threshold_m > 0),
                    captured_at TEXT NOT NULL,
                    parameter_schema_version INTEGER NOT NULL DEFAULT 1
                )
                """
            )
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS task_events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    task_id TEXT NOT NULL REFERENCES tasks(task_id),
                    event_type TEXT NOT NULL,
                    status_before TEXT,
                    status_after TEXT,
                    phase TEXT NOT NULL,
                    command_id TEXT,
                    occurred_at TEXT NOT NULL,
                    message TEXT,
                    error_code TEXT
                )
                """
            )
            connection.execute(
                "CREATE INDEX IF NOT EXISTS task_events_task_time_idx "
                "ON task_events(task_id, occurred_at)"
            )
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS control_requests (
                    command_id TEXT PRIMARY KEY,
                    task_id TEXT NOT NULL REFERENCES tasks(task_id),
                    command TEXT NOT NULL,
                    request_hash TEXT NOT NULL,
                    accepted INTEGER NOT NULL CHECK (accepted IN (0, 1)),
                    response_json TEXT NOT NULL,
                    created_at TEXT NOT NULL
                )
                """
            )
            connection.execute(
                "INSERT INTO schema_migrations(version, applied_at) VALUES (?, ?)",
                (3, _utc_now_text()),
            )
            connection.commit()
        except Exception:
            connection.rollback()
            raise

    def _apply_migration_4(self, connection: sqlite3.Connection) -> None:
        connection.execute("BEGIN IMMEDIATE")
        try:
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS operation_batches (
                    batch_id TEXT PRIMARY KEY,
                    batch_code TEXT NOT NULL UNIQUE,
                    operation_date TEXT NOT NULL,
                    daily_sequence INTEGER NOT NULL CHECK (daily_sequence > 0),
                    status TEXT NOT NULL CHECK (status IN ('active','completed','archived','purged')),
                    active_slot INTEGER,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    started_at TEXT NOT NULL,
                    completed_at TEXT,
                    archived_at TEXT,
                    purged_at TEXT,
                    task_count INTEGER NOT NULL DEFAULT 0 CHECK (task_count >= 0),
                    report_id TEXT,
                    report_path TEXT,
                    report_sha256 TEXT,
                    report_generated_at TEXT,
                    purged_bytes INTEGER NOT NULL DEFAULT 0 CHECK (purged_bytes >= 0),
                    UNIQUE(operation_date, daily_sequence)
                )
                """
            )
            connection.execute(
                "CREATE UNIQUE INDEX IF NOT EXISTS operation_batches_single_active_idx "
                "ON operation_batches(active_slot) WHERE active_slot IS NOT NULL"
            )
            columns = {row[1] for row in connection.execute("PRAGMA table_info(tasks)").fetchall()}
            if "batch_id" not in columns:
                connection.execute("ALTER TABLE tasks ADD COLUMN batch_id TEXT")
            if "batch_sequence" not in columns:
                connection.execute("ALTER TABLE tasks ADD COLUMN batch_sequence INTEGER")

            task_count = int(connection.execute("SELECT COUNT(*) FROM tasks").fetchone()[0])
            if task_count > 0 and int(connection.execute(
                "SELECT COUNT(*) FROM tasks WHERE batch_id IS NULL OR batch_sequence IS NULL"
            ).fetchone()[0]) > 0:
                first_created = connection.execute(
                    "SELECT created_at FROM tasks ORDER BY sequence LIMIT 1"
                ).fetchone()[0]
                operation_date = _local_operation_date(first_created)
                daily_sequence = int(connection.execute(
                    "SELECT COALESCE(MAX(daily_sequence),0)+1 FROM operation_batches WHERE operation_date=?",
                    (operation_date,),
                ).fetchone()[0])
                compact_date = operation_date.replace("-", "")
                batch_code = f"{compact_date}-{daily_sequence:02d}"
                batch_id = str(uuid.uuid4())
                active_count = int(connection.execute(
                    """
                    SELECT COUNT(*) FROM tasks
                    WHERE deleted_at IS NULL
                      AND (status IN ('pending','running','paused') OR active_slot IS NOT NULL)
                    """
                ).fetchone()[0])
                status_value = "active" if active_count > 0 else "completed"
                completed_at = None if active_count > 0 else connection.execute(
                    "SELECT MAX(COALESCE(completed_at, updated_at)) FROM tasks"
                ).fetchone()[0]
                connection.execute(
                    """
                    INSERT INTO operation_batches (
                        batch_id, batch_code, operation_date, daily_sequence, status, active_slot,
                        created_at, updated_at, started_at, completed_at, task_count
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        batch_id, batch_code, operation_date, daily_sequence, status_value,
                        1 if status_value == "active" else None,
                        first_created, _utc_now_text(), first_created, completed_at, task_count,
                    ),
                )
                connection.execute(
                    "UPDATE tasks SET batch_id=?, batch_sequence=sequence WHERE batch_id IS NULL OR batch_sequence IS NULL",
                    (batch_id,),
                )

            connection.execute(
                "CREATE UNIQUE INDEX IF NOT EXISTS tasks_batch_sequence_unique_idx "
                "ON tasks(batch_id, batch_sequence)"
            )
            connection.execute(
                "CREATE INDEX IF NOT EXISTS tasks_batch_status_idx "
                "ON tasks(batch_id, status, batch_sequence)"
            )
            connection.execute(
                "INSERT INTO schema_migrations(version, applied_at) VALUES (?, ?)",
                (4, _utc_now_text()),
            )
            connection.commit()
        except Exception:
            connection.rollback()
            raise

    def _apply_migration_5(self, connection: sqlite3.Connection) -> None:
        connection.execute("BEGIN IMMEDIATE")
        try:
            columns = {row[1] for row in connection.execute("PRAGMA table_info(tasks)").fetchall()}
            if "transition_started_at" not in columns:
                connection.execute("ALTER TABLE tasks ADD COLUMN transition_started_at TEXT")
            if "transition_deadline_at" not in columns:
                connection.execute("ALTER TABLE tasks ADD COLUMN transition_deadline_at TEXT")
            connection.execute(
                "CREATE INDEX IF NOT EXISTS tasks_transition_deadline_idx "
                "ON tasks(transition_deadline_at) WHERE active_slot IS NOT NULL"
            )
            connection.execute(
                "INSERT INTO schema_migrations(version, applied_at) VALUES (?, ?)",
                (5, _utc_now_text()),
            )
            connection.commit()
        except Exception:
            connection.rollback()
            raise

    def _apply_migration_6(self, connection: sqlite3.Connection) -> None:
        connection.execute("BEGIN IMMEDIATE")
        try:
            columns = {row[1] for row in connection.execute("PRAGMA table_info(tasks)").fetchall()}
            if "display_id" not in columns:
                connection.execute("ALTER TABLE tasks ADD COLUMN display_id TEXT")
            if "local_data_purged_at" not in columns:
                connection.execute("ALTER TABLE tasks ADD COLUMN local_data_purged_at TEXT")
            if "purged_bytes" not in columns:
                connection.execute("ALTER TABLE tasks ADD COLUMN purged_bytes INTEGER NOT NULL DEFAULT 0")

            used: set[str] = set()
            rows = connection.execute(
                "SELECT task_id, created_at, display_id FROM tasks ORDER BY created_at, sequence"
            ).fetchall()
            for row in rows:
                current = row["display_id"]
                if current:
                    used.add(str(current))
                    continue
                display_id = _allocate_display_id(row["created_at"], used)
                used.add(display_id)
                connection.execute("UPDATE tasks SET display_id=? WHERE task_id=?", (display_id, row["task_id"]))
            connection.execute(
                "CREATE UNIQUE INDEX IF NOT EXISTS tasks_display_id_unique_idx ON tasks(display_id)"
            )
            connection.execute(
                "CREATE INDEX IF NOT EXISTS tasks_created_at_idx ON tasks(created_at, sequence)"
            )
            connection.execute(
                "UPDATE tasks SET schema_version=? WHERE schema_version<?",
                (_SCHEMA_VERSION, _SCHEMA_VERSION),
            )
            connection.execute(
                "INSERT INTO schema_migrations(version, applied_at) VALUES (?, ?)",
                (6, _utc_now_text()),
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
            f"SELECT tasks.*, operation_batches.batch_code FROM tasks "
            f"JOIN operation_batches ON operation_batches.batch_id=tasks.batch_id "
            f"WHERE task_id IN ({placeholders})",
            identifiers,
        ).fetchall()
        return [self._row_to_record(row) for row in rows]

    @staticmethod
    def _row_to_record(row: sqlite3.Row) -> TaskRecord:
        keys = set(row.keys())
        return TaskRecord(
            task_id=row["task_id"],
            display_id=(row["display_id"] if "display_id" in keys and row["display_id"] else _task_display_base(row["created_at"])),
            sequence=int(row["batch_sequence"] if "batch_sequence" in keys and row["batch_sequence"] is not None else row["sequence"]),
            global_sequence=int(row["sequence"]),
            batch_id=row["batch_id"] if "batch_id" in keys else "",
            batch_code=row["batch_code"] if "batch_code" in keys else "",
            tunnel_code=row["tunnel_code"],
            tunnel_name=row["tunnel_name"],
            status=row["status"],
            operation_phase=row["operation_phase"] if "operation_phase" in keys else "idle",
            status_revision=int(row["status_revision"]) if "status_revision" in keys else 0,
            created_at=row["created_at"],
            updated_at=row["updated_at"],
            start_requested_at=row["start_requested_at"] if "start_requested_at" in keys else None,
            started_at=row["started_at"],
            stop_requested_at=row["stop_requested_at"] if "stop_requested_at" in keys else None,
            completed_at=row["completed_at"],
            entry_rtk_status=row["entry_rtk_status"] if "entry_rtk_status" in keys else "not_requested",
            exit_rtk_status=row["exit_rtk_status"] if "exit_rtk_status" in keys else "not_requested",
            has_measurements=bool(row["has_measurements"]),
            recording_path=row["recording_path"],
            local_data_purged_at=row["local_data_purged_at"] if "local_data_purged_at" in keys else None,
            purged_bytes=int(row["purged_bytes"]) if "purged_bytes" in keys and row["purged_bytes"] is not None else 0,
            last_error_code=row["last_error_code"] if "last_error_code" in keys else None,
            last_error_message=row["last_error_message"] if "last_error_message" in keys else None,
            warning_code=row["warning_code"] if "warning_code" in keys else None,
            active_session_id=row["active_session_id"] if "active_session_id" in keys else None,
            active_slot=row["active_slot"] if "active_slot" in keys else None,
            schema_version=row["schema_version"],
            deleted_at=row["deleted_at"],
            delete_reason=row["delete_reason"],
        )

    @staticmethod
    def _request_hash(drafts: Sequence[TaskCreateRequest]) -> str:
        payload = {
            "tasks": [
            {
                "tunnel_code": draft.tunnel_code,
                "tunnel_name": draft.tunnel_name,
            }
            for draft in drafts
            ],
        }
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


def _task_display_base(value: str) -> str:
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError:
        parsed = datetime.now(timezone.utc)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    local = parsed.astimezone(_LOCAL_TIMEZONE)
    return local.strftime("%Y%m%d_%H%M%S")


def _allocate_display_id(value: str, used: set[str]) -> str:
    base = _task_display_base(value)
    if base not in used:
        return base
    suffix = 2
    while f"{base}_{suffix:02d}" in used:
        suffix += 1
    return f"{base}_{suffix:02d}"


def _directory_size(path: Path) -> int:
    if not path.exists():
        return 0
    total = 0
    for item in path.rglob("*"):
        try:
            if item.is_file():
                total += item.stat().st_size
        except OSError:
            continue
    return total

def _local_operation_date(value: str) -> str:
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError:
        return datetime.now(_LOCAL_TIMEZONE).date().isoformat()
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(_LOCAL_TIMEZONE).date().isoformat()


def _utc_now_text() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
