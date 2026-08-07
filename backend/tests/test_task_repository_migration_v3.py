from __future__ import annotations

import sqlite3
from pathlib import Path

from backend.tasks.repository import TaskRepository


def test_version_2_database_migrates_to_task_control_schema(tmp_path: Path) -> None:
    database_path = tmp_path / "capture.db"
    with sqlite3.connect(database_path) as connection:
        connection.executescript(
            """
            CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL);
            INSERT INTO schema_migrations VALUES (1, '2026-08-01T00:00:00Z');
            INSERT INTO schema_migrations VALUES (2, '2026-08-02T00:00:00Z');
            CREATE TABLE tasks (
                task_id TEXT PRIMARY KEY,
                sequence INTEGER NOT NULL UNIQUE,
                tunnel_code TEXT NOT NULL,
                tunnel_name TEXT NOT NULL,
                status TEXT NOT NULL,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                started_at TEXT,
                completed_at TEXT,
                has_measurements INTEGER NOT NULL DEFAULT 0,
                recording_path TEXT,
                schema_version INTEGER NOT NULL DEFAULT 1,
                deleted_at TEXT,
                delete_reason TEXT
            );
            CREATE TABLE task_idempotency (
                idempotency_key TEXT PRIMARY KEY,
                request_hash TEXT NOT NULL,
                response_task_ids TEXT NOT NULL,
                created_at TEXT NOT NULL
            );
            INSERT INTO tasks (
                task_id, sequence, tunnel_code, tunnel_name, status, created_at, updated_at,
                has_measurements, schema_version
            ) VALUES (
                '00000000-0000-0000-0000-000000000001', 1, 'T-001', '迁移测试隧道',
                'pending', '2026-08-01T00:00:00Z', '2026-08-01T00:00:00Z', 0, 2
            );
            """
        )

    repository = TaskRepository(database_path, tmp_path / "tasks")
    repository.initialize()
    task = repository.get_task("00000000-0000-0000-0000-000000000001")

    assert task.operation_phase == "idle"
    assert task.status_revision == 0
    assert task.entry_rtk_status == "not_requested"
    assert task.exit_rtk_status == "not_requested"
    assert task.batch_id
    assert task.batch_code
    assert task.sequence == 1
    assert task.global_sequence == 1
    with sqlite3.connect(database_path) as connection:
        assert connection.execute("SELECT MAX(version) FROM schema_migrations").fetchone()[0] == 6
        tables = {row[0] for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        )}
    assert {"task_parameters", "task_events", "control_requests", "operation_batches"}.issubset(tables)
    with sqlite3.connect(database_path) as connection:
        columns = {row[1] for row in connection.execute("PRAGMA table_info(tasks)")}
    assert {"transition_started_at", "transition_deadline_at", "display_id", "local_data_purged_at", "purged_bytes"}.issubset(columns)
    assert task.display_id == "20260801_080000"
    assert task.schema_version == 6
