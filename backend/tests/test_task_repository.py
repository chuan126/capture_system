from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re

from backend.tasks.models import TaskCreateRequest
from backend.tasks.repository import TaskRepository


def make_repository(root: Path) -> TaskRepository:
    repository = TaskRepository(root / "capture.db", root / "tasks")
    repository.initialize()
    if repository.get_active_batch() is None and not repository.list_batches():
        repository.create_batch()
    return repository


def test_repository_persists_tasks_and_assigns_stable_ids(tmp_path: Path) -> None:
    repository = make_repository(tmp_path)
    created = repository.create_tasks(
        [
            TaskCreateRequest(tunnel_code="T-001", tunnel_name="东山隧道"),
            TaskCreateRequest(tunnel_code="T-001", tunnel_name="东山隧道复测"),
        ]
    )

    restarted = make_repository(tmp_path)
    persisted = restarted.list_tasks(limit=100, offset=0, order="asc")

    assert [task.sequence for task in created] == [1, 2]
    assert created[0].task_id != created[1].task_id
    assert [task.task_id for task in persisted] == [task.task_id for task in created]
    display_ids = [task.display_id for task in persisted]
    assert re.fullmatch(r"\d{8}_\d{6}", display_ids[0])
    assert display_ids[1] == f"{display_ids[0]}_02"
    assert [task.display_sequence for task in persisted] == display_ids
    assert all(task.status == "pending" for task in persisted)
    assert all(not task.has_measurements for task in persisted)


def test_repository_reuses_idempotent_batch_response(tmp_path: Path) -> None:
    repository = make_repository(tmp_path)
    drafts = [TaskCreateRequest(tunnel_code="T-002", tunnel_name="北岭隧道")]

    first = repository.create_tasks(drafts, idempotency_key="request-001")
    second = repository.create_tasks(drafts, idempotency_key="request-001")

    assert second == first
    assert len(repository.list_tasks(limit=100, offset=0, order="asc")) == 1


def test_concurrent_creates_do_not_duplicate_sequence_numbers(tmp_path: Path) -> None:
    repository = make_repository(tmp_path)

    def create(index: int) -> tuple[str, int, str]:
        record = repository.create_tasks(
            [TaskCreateRequest(tunnel_code=f"T-{index:03d}", tunnel_name=f"隧道{index}")]
        )[0]
        return record.task_id, record.sequence, record.display_id

    with ThreadPoolExecutor(max_workers=8) as executor:
        results = list(executor.map(create, range(1, 25)))

    task_ids = [task_id for task_id, _, _ in results]
    sequences = [sequence for _, sequence, _ in results]
    display_ids = [display_id for _, _, display_id in results]
    assert len(set(task_ids)) == 24
    assert sorted(sequences) == list(range(1, 25))
    assert len(set(display_ids)) == 24
    assert all(re.fullmatch(r"\d{8}_\d{6}(?:_\d{2})?", value) for value in display_ids)


def test_soft_delete_hides_task_without_affecting_time_display_ids(tmp_path: Path) -> None:
    repository = make_repository(tmp_path)
    first, second = repository.create_tasks(
        [
            TaskCreateRequest(tunnel_code="T-101", tunnel_name="甲隧道"),
            TaskCreateRequest(tunnel_code="T-102", tunnel_name="乙隧道"),
        ]
    )

    deleted = repository.soft_delete_task(first.task_id)
    third = repository.create_tasks(
        [TaskCreateRequest(tunnel_code="T-103", tunnel_name="丙隧道")]
    )[0]

    assert deleted.deleted_at is not None
    assert repository.get_task(first.task_id, include_deleted=True).delete_reason == "user_request"
    assert [task.task_id for task in repository.list_tasks()] == [second.task_id, third.task_id]
    assert third.sequence == 3
    assert third.display_id not in {first.display_id, second.display_id}


def test_soft_delete_rejects_running_or_paused_task(tmp_path: Path) -> None:
    import sqlite3

    from backend.tasks.repository import TaskDeleteConflictError

    repository = make_repository(tmp_path)
    task = repository.create_tasks(
        [TaskCreateRequest(tunnel_code="T-104", tunnel_name="运行任务")]
    )[0]
    with sqlite3.connect(repository.database_path) as connection:
        connection.execute("UPDATE tasks SET status = 'running' WHERE task_id = ?", (task.task_id,))

    try:
        repository.soft_delete_task(task.task_id)
    except TaskDeleteConflictError as error:
        assert "不能删除" in str(error)
    else:
        raise AssertionError("running task deletion must fail")
