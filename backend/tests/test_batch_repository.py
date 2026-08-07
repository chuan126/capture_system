from __future__ import annotations

import hashlib
import sqlite3
from pathlib import Path

import pytest

from backend.tasks.models import TaskCreateRequest
from backend.tasks.repository import BatchConflictError, TaskRepository, TaskStorageError


def make_repository(root: Path) -> TaskRepository:
    repository = TaskRepository(root / "capture.db", root / "tasks")
    repository.initialize()
    return repository


def mark_batch_tasks_completed(repository: TaskRepository, batch_id: str) -> None:
    with sqlite3.connect(repository.database_path) as connection:
        connection.execute(
            """
            UPDATE tasks
            SET status='completed', operation_phase='completed', completed_at=updated_at,
                active_slot=NULL, active_session_id=NULL
            WHERE batch_id=? AND deleted_at IS NULL
            """,
            (batch_id,),
        )


def test_legacy_batches_do_not_reset_time_display_identifier(tmp_path: Path) -> None:
    repository = make_repository(tmp_path)
    first_batch = repository.create_batch()
    first_tasks = repository.create_tasks([
        TaskCreateRequest(tunnel_code="T-001", tunnel_name="甲隧道"),
        TaskCreateRequest(tunnel_code="T-002", tunnel_name="乙隧道"),
    ])
    repository.soft_delete_task(first_tasks[0].task_id)
    third = repository.create_tasks([
        TaskCreateRequest(tunnel_code="T-003", tunnel_name="丙隧道"),
    ])[0]

    assert [first_tasks[0].sequence, first_tasks[1].sequence, third.sequence] == [1, 2, 3]
    assert [first_tasks[0].global_sequence, first_tasks[1].global_sequence, third.global_sequence] == [1, 2, 3]

    mark_batch_tasks_completed(repository, first_batch.batch_id)
    repository.complete_batch(first_batch.batch_id)
    second_batch = repository.create_batch()
    next_task = repository.create_tasks([
        TaskCreateRequest(tunnel_code="T-101", tunnel_name="第二次作业隧道"),
    ])[0]

    assert second_batch.batch_id != first_batch.batch_id
    assert next_task.batch_id == second_batch.batch_id
    assert next_task.sequence == 1
    assert next_task.display_id != first_tasks[0].display_id
    assert next_task.display_id != first_tasks[1].display_id
    assert next_task.display_id != third.display_id
    assert next_task.display_sequence == next_task.display_id
    assert next_task.global_sequence == 4
    assert repository.list_tasks(batch_id=first_batch.batch_id)[0].sequence == 2
    assert repository.list_tasks(batch_id=second_batch.batch_id)[0].sequence == 1


def test_only_one_active_batch_and_incomplete_tasks_block_completion(tmp_path: Path) -> None:
    repository = make_repository(tmp_path)
    batch = repository.create_batch()
    repository.create_tasks([
        TaskCreateRequest(tunnel_code="T-001", tunnel_name="未结束任务"),
    ])

    with pytest.raises(BatchConflictError, match="尚未结束"):
        repository.create_batch()
    with pytest.raises(BatchConflictError, match="未结束任务"):
        repository.complete_batch(batch.batch_id)


def test_archived_batch_can_purge_task_directories_and_keep_report_index(tmp_path: Path) -> None:
    repository = make_repository(tmp_path)
    batch = repository.create_batch()
    tasks = repository.create_tasks([
        TaskCreateRequest(tunnel_code="T-001", tunnel_name="甲隧道"),
        TaskCreateRequest(tunnel_code="T-002", tunnel_name="乙隧道"),
    ])
    expected_bytes = 0
    for index, task in enumerate(tasks, start=1):
        task_dir = repository.tasks_directory / task.task_id
        task_dir.mkdir(parents=True)
        payload = bytes([index]) * (100 * index)
        (task_dir / "measurements.db").write_bytes(payload)
        expected_bytes += len(payload)

    mark_batch_tasks_completed(repository, batch.batch_id)
    repository.complete_batch(batch.batch_id)
    with pytest.raises(BatchConflictError, match="已暂存"):
        repository.purge_batch(batch.batch_id)

    report_path = repository.database_path.parent / "reports" / "report-1" / "batch.pdf"
    report_path.parent.mkdir(parents=True)
    report_path.write_bytes(b"%PDF-test")
    repository.record_batch_report(
        batch.batch_id,
        report_id="report-1",
        report_path=report_path,
        report_sha256=hashlib.sha256(report_path.read_bytes()).hexdigest(),
        generated_at="2026-08-07T01:00:00Z",
    )
    archived = repository.archive_batch(batch.batch_id)
    assert archived.status == "archived"

    result = repository.purge_batch(batch.batch_id)

    assert result.batch.status == "purged"
    assert result.released_bytes == expected_bytes
    assert result.removed_task_count == 2
    assert repository.list_tasks(batch_id=batch.batch_id) == []
    assert all(not (repository.tasks_directory / task.task_id).exists() for task in tasks)
    assert report_path.is_file()
    assert result.batch.report_sha256 is not None


def test_purge_failure_keeps_batch_archived_and_retryable(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    import backend.tasks.repository as repository_module

    repository = make_repository(tmp_path)
    batch = repository.create_batch()
    task = repository.create_tasks([
        TaskCreateRequest(tunnel_code="T-001", tunnel_name="清理失败测试"),
    ])[0]
    task_dir = repository.tasks_directory / task.task_id
    task_dir.mkdir(parents=True)
    (task_dir / "measurements.db").write_bytes(b"test")
    mark_batch_tasks_completed(repository, batch.batch_id)
    repository.complete_batch(batch.batch_id)

    report_path = repository.database_path.parent / "reports" / "report-1" / "batch.pdf"
    report_path.parent.mkdir(parents=True)
    report_path.write_bytes(b"%PDF-test")
    repository.record_batch_report(
        batch.batch_id,
        report_id="report-1",
        report_path=report_path,
        report_sha256=hashlib.sha256(report_path.read_bytes()).hexdigest(),
        generated_at="2026-08-07T01:00:00Z",
    )
    repository.archive_batch(batch.batch_id)

    def fail_remove(path: Path, *, ignore_errors: bool) -> None:
        raise PermissionError(f"permission denied: {path}")

    monkeypatch.setattr(repository_module.shutil, "rmtree", fail_remove)
    with pytest.raises(TaskStorageError, match="批次仍保持已暂存"):
        repository.purge_batch(batch.batch_id)

    assert repository.get_batch(batch.batch_id).status == "archived"
    assert len(repository.list_tasks(batch_id=batch.batch_id)) == 1
    assert task_dir.is_dir()
