from __future__ import annotations

import sqlite3
from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text("<!doctype html><html><body>Capture</body></html>", encoding="utf-8")


def test_batch_api_controls_job_boundaries_and_task_numbering(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        empty = client.get("/api/v1/batches")
        first_batch = client.post("/api/v1/batches")
        conflict = client.post("/api/v1/batches")
        task_1 = client.post("/api/v1/tasks", json={"tunnel_code": "T-001", "tunnel_name": "甲隧道"})
        task_2 = client.post("/api/v1/tasks", json={"tunnel_code": "T-002", "tunnel_name": "乙隧道"})
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                "UPDATE tasks SET status='completed', operation_phase='completed' WHERE batch_id=?",
                (first_batch.json()["batch_id"],),
            )
        completed = client.post(f"/api/v1/batches/{first_batch.json()['batch_id']}/complete")
        second_batch = client.post("/api/v1/batches")
        task_3 = client.post("/api/v1/tasks", json={"tunnel_code": "T-101", "tunnel_name": "第二次作业"})
        first_tasks = client.get("/api/v1/tasks", params={"batch_id": first_batch.json()["batch_id"]})
        second_tasks = client.get("/api/v1/tasks", params={"batch_id": second_batch.json()["batch_id"]})

    assert empty.status_code == 200 and empty.json() == []
    assert first_batch.status_code == 201
    assert conflict.status_code == 409
    assert [task_1.json()["sequence"], task_2.json()["sequence"]] == [1, 2]
    assert completed.status_code == 200
    assert second_batch.status_code == 201
    assert task_3.json()["sequence"] == 1
    assert task_3.json()["global_sequence"] == 3
    assert len(first_tasks.json()) == 2
    assert len(second_tasks.json()) == 1
    assert second_tasks.json()[0]["batch_code"] == second_batch.json()["batch_code"]
