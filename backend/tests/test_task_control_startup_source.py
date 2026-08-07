from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
RUN_LAN_PREVIEW = PROJECT_ROOT / "scripts" / "operation" / "run_lan_preview.sh"


def test_task_control_startup_checks_task_manager_service_servers() -> None:
    source = RUN_LAN_PREVIEW.read_text(encoding="utf-8")

    assert "ros2 node info /task_manager_node" in source
    assert "/capture/task/start" in source
    assert "/capture/task/pause" in source
    assert "/capture/task/resume" in source
    assert "/capture/task/stop" in source
    assert "Service Servers:" in source


def test_recover_is_optional_during_startup() -> None:
    source = RUN_LAN_PREVIEW.read_text(encoding="utf-8")

    assert "optional_task_services=(/capture/task/recover)" in source
    assert "开始、暂停、继续和停止仍可使用" in source
