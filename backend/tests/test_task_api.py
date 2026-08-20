from pathlib import Path
import re

from fastapi.testclient import TestClient

from backend.main import create_app
from backend.tasks.repository import TaskRepository


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text(
        "<!doctype html><html lang='zh-CN'><body>Capture System</body></html>",
        encoding="utf-8",
    )


def create_active_batch(client: TestClient) -> dict[str, object]:
    response = client.post("/api/v1/batches")
    assert response.status_code == 201
    return response.json()


def test_create_list_get_and_restart_persistence(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
        create_active_batch(client)
        create_response = client.post(
            "/api/v1/tasks/batch",
            headers={"Idempotency-Key": "browser-request-001"},
            json={
                "tasks": [
                    {"tunnel_code": " T-001 ", "tunnel_name": " 东山隧道 "},
                    {"tunnel_code": "T-001", "tunnel_name": "东山隧道复测"},
                ]
            },
        )
        repeated_response = client.post(
            "/api/v1/tasks/batch",
            headers={"Idempotency-Key": "browser-request-001"},
            json={
                "tasks": [
                    {"tunnel_code": "T-001", "tunnel_name": "东山隧道"},
                    {"tunnel_code": "T-001", "tunnel_name": "东山隧道复测"},
                ]
            },
        )
        list_response = client.get("/api/v1/tasks")

    assert create_response.status_code == 201
    created = create_response.json()
    assert re.fullmatch(r"\d{8}_\d{6}", created[0]["display_id"])
    assert created[1]["display_id"] == f"{created[0]['display_id']}_02"
    assert [task["display_sequence"] for task in created] == [task["display_id"] for task in created]
    assert created[0]["task_id"] != created[1]["task_id"]
    assert created[0]["tunnel_code"] == "T-001"
    assert repeated_response.status_code == 201
    assert repeated_response.json() == created
    assert list_response.status_code == 200
    assert list_response.json() == created

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as restarted_client:
        persisted_response = restarted_client.get("/api/v1/tasks")
        single_response = restarted_client.get(
            f"/api/v1/tasks/{created[0]['task_id']}"
        )

    assert persisted_response.status_code == 200
    assert persisted_response.json() == created
    assert single_response.status_code == 200
    assert single_response.json() == created[0]


def test_single_create_and_input_validation(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(
        create_app(static_dir, data_root=tmp_path / "runtime", start_ros_bridge=False)
    ) as client:
        create_active_batch(client)
        response = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-009", "tunnel_name": "南湾隧道"},
        )
        invalid_response = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "   ", "tunnel_name": "南湾隧道"},
        )
        extra_field_response = client.post(
            "/api/v1/tasks",
            json={
                "tunnel_code": "T-010",
                "tunnel_name": "西岭隧道",
                "task_name": "不得接收",
            },
        )

    assert response.status_code == 201
    assert response.json()["sequence"] == 1
    assert response.json()["status"] == "pending"
    assert invalid_response.status_code == 422
    assert extra_field_response.status_code == 422


def test_idempotency_key_conflict_returns_409(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(
        create_app(static_dir, data_root=tmp_path / "runtime", start_ros_bridge=False)
    ) as client:
        create_active_batch(client)
        first = client.post(
            "/api/v1/tasks",
            headers={"Idempotency-Key": "same-key"},
            json={"tunnel_code": "T-011", "tunnel_name": "甲隧道"},
        )
        conflict = client.post(
            "/api/v1/tasks",
            headers={"Idempotency-Key": "same-key"},
            json={"tunnel_code": "T-012", "tunnel_name": "乙隧道"},
        )

    assert first.status_code == 201
    assert conflict.status_code == 409
    assert "幂等键" in conflict.json()["detail"]


def test_task_api_returns_503_when_database_cannot_open(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    database_directory = tmp_path / "database-as-directory"
    database_directory.mkdir()

    with TestClient(
        create_app(
            static_dir,
            data_root=tmp_path / "runtime",
            task_database_path=database_directory,
            start_ros_bridge=False,
        )
    ) as client:
        response = client.get("/api/v1/tasks")

    assert response.status_code == 503
    assert "任务数据库" in response.json()["detail"]


def test_missing_task_returns_404(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(
        create_app(static_dir, data_root=tmp_path / "runtime", start_ros_bridge=False)
    ) as client:
        response = client.get("/api/v1/tasks/00000000-0000-0000-0000-000000000000")

    assert response.status_code == 404
    assert response.json()["detail"] == "任务不存在"


def test_delete_task_hides_it_from_all_normal_queries(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
        create_active_batch(client)
        first = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-201", "tunnel_name": "待删除任务"},
        ).json()
        second = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-202", "tunnel_name": "保留任务"},
        ).json()
        deleted = client.delete(f"/api/v1/tasks/{first['task_id']}")
        listed = client.get("/api/v1/tasks")
        missing = client.get(f"/api/v1/tasks/{first['task_id']}")
        third = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-203", "tunnel_name": "后续任务"},
        ).json()

    assert deleted.status_code == 204
    assert [task["task_id"] for task in listed.json()] == [second["task_id"]]
    assert missing.status_code == 404
    assert third["sequence"] == 3


def test_delete_running_task_returns_409(tmp_path: Path) -> None:
    import sqlite3

    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
        create_active_batch(client)
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-204", "tunnel_name": "运行任务"},
        ).json()
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute("UPDATE tasks SET status = 'running' WHERE task_id = ?", (task["task_id"],))
        response = client.delete(f"/api/v1/tasks/{task['task_id']}")

    assert response.status_code == 409
    assert "不能删除" in response.json()["detail"]


def test_delete_selected_tasks_is_one_transaction_and_hides_all_selected(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        first = client.post("/api/v1/tasks", json={"tunnel_code": "T-301", "tunnel_name": "批量删除一"}).json()
        second = client.post("/api/v1/tasks", json={"tunnel_code": "T-302", "tunnel_name": "批量删除二"}).json()
        third = client.post("/api/v1/tasks", json={"tunnel_code": "T-303", "tunnel_name": "保留任务"}).json()
        response = client.post("/api/v1/tasks/delete-selected", json={"task_ids": [first["task_id"], second["task_id"]]})
        listed = client.get("/api/v1/tasks").json()

    assert response.status_code == 200
    assert response.json()["deleted_task_count"] == 2
    assert response.json()["task_ids"] == [first["task_id"], second["task_id"]]
    assert [task["task_id"] for task in listed] == [third["task_id"]]


def test_delete_selected_tasks_rolls_back_when_one_task_is_active(tmp_path: Path) -> None:
    import sqlite3

    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        first = client.post("/api/v1/tasks", json={"tunnel_code": "T-311", "tunnel_name": "应保留"}).json()
        second = client.post("/api/v1/tasks", json={"tunnel_code": "T-312", "tunnel_name": "活动任务"}).json()
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute("UPDATE tasks SET status='running' WHERE task_id=?", (second["task_id"],))
        response = client.post("/api/v1/tasks/delete-selected", json={"task_ids": [first["task_id"], second["task_id"]]})
        listed = client.get("/api/v1/tasks").json()

    assert response.status_code == 409
    assert {task["task_id"] for task in listed} == {first["task_id"], second["task_id"]}


def test_task_creation_without_batch_input_generates_time_identifier(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
        response = client.post(
            "/api/v1/tasks/batch",
            headers={"Idempotency-Key": "time-id-001"},
            json={
                "tasks": [
                    {"tunnel_code": "T-401", "tunnel_name": "时间编号测试"},
                    {"tunnel_code": "T-402", "tunnel_name": "同秒编号测试"},
                ]
            },
        )

    assert response.status_code == 201
    first, second = response.json()
    assert re.fullmatch(r"\d{8}_\d{6}", first["display_id"])
    assert second["display_id"] == f"{first['display_id']}_02"
    assert first["display_sequence"] == first["display_id"]
    assert first["task_id"] != second["task_id"]


def test_legacy_batch_mode_field_is_rejected_from_task_creation(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(
        create_app(static_dir, data_root=tmp_path / "runtime", start_ros_bridge=False)
    ) as client:
        response = client.post(
            "/api/v1/tasks/batch",
            headers={"Idempotency-Key": "legacy-batch-mode"},
            json={
                "tasks": [{"tunnel_code": "T-411", "tunnel_name": "不再选择作业批次"}],
                "batch_mode": "new",
            },
        )
        tasks = client.get("/api/v1/tasks").json()

    assert response.status_code == 422
    assert tasks == []


def test_purge_task_data_accepts_logically_deleted_task(tmp_path: Path) -> None:
    import sqlite3

    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-420", "tunnel_name": "删除后清理测试"},
        ).json()
        task_dir = data_root / "tasks" / task["task_id"]
        task_dir.mkdir(parents=True, exist_ok=True)
        measurement = task_dir / "measurements.db"
        measurement.write_bytes(b"x" * 2048)
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                "UPDATE tasks SET status='completed', has_measurements=1, recording_path=? WHERE task_id=?",
                (f"{task['task_id']}/measurements.db", task["task_id"]),
            )

        deleted = client.delete(f"/api/v1/tasks/{task['task_id']}")
        purge = client.post("/api/v1/tasks/purge-data", json={"task_ids": [task["task_id"]]})

    assert deleted.status_code == 204
    assert purge.status_code == 200
    assert purge.json()["removed_task_count"] == 1
    assert purge.json()["released_bytes"] == 2048
    assert not task_dir.exists()

    repository = TaskRepository(data_root / "capture.db", data_root / "tasks")
    persisted = repository.get_task(task["task_id"], include_deleted=True)
    assert persisted.deleted_at is not None
    assert persisted.has_measurements is False
    assert persisted.recording_path is None
    assert persisted.local_data_purged_at is not None


def test_purge_task_data_removes_files_but_keeps_task_index(tmp_path: Path) -> None:
    import sqlite3

    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-421", "tunnel_name": "本地清理测试"},
        ).json()
        task_dir = data_root / "tasks" / task["task_id"]
        task_dir.mkdir(parents=True, exist_ok=True)
        measurement = task_dir / "measurements.db"
        measurement.write_bytes(b"x" * 4096)
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                "UPDATE tasks SET status='completed', has_measurements=1, recording_path=? WHERE task_id=?",
                (f"{task['task_id']}/measurements.db", task["task_id"]),
            )
        purge = client.post("/api/v1/tasks/purge-data", json={"task_ids": [task["task_id"]]})
        persisted = client.get(f"/api/v1/tasks/{task['task_id']}")

    assert purge.status_code == 200
    assert purge.json()["removed_task_count"] == 1
    assert purge.json()["released_bytes"] == 4096
    assert not task_dir.exists()
    assert persisted.status_code == 200
    payload = persisted.json()
    assert payload["display_id"] == task["display_id"]
    assert payload["has_measurements"] is False
    assert payload["recording_path"] is None
    assert payload["local_data_purged_at"] is not None



def test_task_api_returns_frozen_start_parameters_including_zero(tmp_path: Path) -> None:
    import sqlite3

    static_dir = tmp_path / "site-params"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime-params"
    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-PARAM", "tunnel_name": "冻结参数测试"},
        ).json()
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                """
                INSERT INTO task_parameters (
                    task_id, lane, lidar_mount_height_m, clearance_threshold_m,
                    captured_at, parameter_schema_version
                ) VALUES (?, 'left', 0.0, 0.0, '2026-08-09T00:00:00Z', 1)
                """,
                (task["task_id"],),
            )
        response = client.get(f"/api/v1/tasks/{task['task_id']}")

    assert response.status_code == 200
    payload = response.json()
    assert payload["lane"] == "left"
    assert payload["lidar_mount_height_m"] == 0.0
    assert payload["clearance_threshold_m"] == 0.0
    assert payload["clearance_upper_limit_m"] == 20.0


def test_task_creation_persists_independent_planned_lane_and_height_range(tmp_path: Path) -> None:
    import sqlite3

    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        task = client.post(
            "/api/v1/tasks",
            json={
                "tunnel_code": "T-501",
                "tunnel_name": "计划参数测试",
                "travel_direction": "down",
                "lane_side": "left",
                "clearance_threshold_m": 4.6,
                "clearance_upper_limit_m": 5.8,
            },
        )

    assert task.status_code == 201
    payload = task.json()
    assert payload["planned_travel_direction"] == "down"
    assert payload["planned_lane_side"] == "left"
    assert payload["planned_clearance_threshold_m"] == 4.6
    assert payload["planned_clearance_upper_limit_m"] == 5.8
    assert payload["travel_direction"] is None
    assert payload["lane_side"] is None
    assert payload["lane"] is None
    assert payload["clearance_threshold_m"] is None
    assert payload["clearance_upper_limit_m"] is None

    with sqlite3.connect(data_root / "capture.db") as connection:
        row = connection.execute(
            "SELECT planned_travel_direction, planned_lane_side, planned_clearance_threshold_m, "
            "planned_clearance_upper_limit_m "
            "FROM tasks WHERE task_id=?",
            (payload["task_id"],),
        ).fetchone()
    assert row == ("down", "left", 4.6, 5.8)


def test_task_creation_rejects_height_threshold_above_upper_limit(tmp_path: Path) -> None:
    static_dir = tmp_path / "site-range"
    make_static_site(static_dir)
    with TestClient(
        create_app(static_dir, data_root=tmp_path / "runtime-range", start_ros_bridge=False)
    ) as client:
        response = client.post(
            "/api/v1/tasks",
            json={
                "tunnel_code": "T-RANGE",
                "tunnel_name": "高度区间校验测试",
                "clearance_threshold_m": 5.0,
                "clearance_upper_limit_m": 4.9,
            },
        )

    assert response.status_code == 422
    assert "高度下限阈值不能大于高度上限阈值" in response.text
