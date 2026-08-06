from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text(
        "<!doctype html><html lang='zh-CN'><body>Capture System</body></html>",
        encoding="utf-8",
    )


def test_create_list_get_and_restart_persistence(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
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
    assert [task["display_sequence"] for task in created] == ["01", "02"]
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
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-204", "tunnel_name": "运行任务"},
        ).json()
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute("UPDATE tasks SET status = 'running' WHERE task_id = ?", (task["task_id"],))
        response = client.delete(f"/api/v1/tasks/{task['task_id']}")

    assert response.status_code == 409
    assert "不能删除" in response.json()["detail"]
