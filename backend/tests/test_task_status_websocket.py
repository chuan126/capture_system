from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app
from backend.protocols.task_status_v1 import TaskStatusSnapshot


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text(
        "<!doctype html><html lang='zh-CN'><body>Capture System</body></html>",
        encoding="utf-8",
    )


def test_websocket_sends_latest_device_task_status(tmp_path: Path) -> None:
    site = tmp_path / "site"
    make_static_site(site)
    application = create_app(site, data_root=tmp_path / "runtime", start_ros_bridge=False)
    application.state.task_status_hub.set_ros_availability(True)
    application.state.task_status_hub.publish(
        TaskStatusSnapshot(
            task_id="task-001",
            task_sequence=1,
            status="running",
            operation_phase="recording",
            status_revision=4,
            command_id="start-001",
            message="采集已开始，入口RTK坐标未确认",
            error_code=None,
            entry_rtk_status="unconfirmed",
            exit_rtk_status="not_requested",
            has_measurements=False,
            recording_path="task-001/measurements.db",
            started_at_ns=1,
            completed_at_ns=0,
            emitted_at_ns=2,
        )
    )

    with TestClient(application) as client:
        # lifespan会在start_ros_bridge=False时标记不可用，测试手动恢复。
        application.state.task_status_hub.set_ros_availability(True)
        with client.websocket_connect(
            "/ws/v1/task-status", headers={"origin": "http://testserver"}
        ) as websocket:
            status = websocket.receive_json()
            snapshot = websocket.receive_json()

    assert status["state"] == "streaming"
    assert snapshot["type"] == "task_status_snapshot"
    assert snapshot["operation_phase"] == "recording"
    assert snapshot["entry_rtk_status"] == "unconfirmed"
