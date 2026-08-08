from pathlib import Path

from backend.main import PROJECT_ROOT, resolve_data_root


def test_default_data_root_is_project_relative_runtime(monkeypatch) -> None:
    monkeypatch.delenv("CAPTURE_DATA_ROOT", raising=False)
    assert resolve_data_root() == (PROJECT_ROOT / "runtime").resolve()


def test_operation_scripts_compute_project_root_and_runtime() -> None:
    root = Path(__file__).resolve().parents[2]
    run_web = (root / "scripts/operation/run_web.sh").read_text(encoding="utf-8")
    preview = (root / "scripts/operation/run_lan_preview.sh").read_text(encoding="utf-8")
    installer = (root / "scripts/deploy/install_systemd.sh").read_text(encoding="utf-8")
    assert '${project_root}/runtime' in run_web
    assert '${project_root}/runtime' in preview
    assert 'CAPTURE_DATA_ROOT=${project_root}/runtime' in installer
    assert '/home/cat/.local/share/capture_system' not in run_web + preview + installer


def test_runtime_sources_do_not_hardcode_previous_device_paths() -> None:
    root = Path(__file__).resolve().parents[2]
    runtime_sources = [
        root / "backend/main.py",
        root / "scripts/operation/run_web.sh",
        root / "scripts/operation/run_lan_preview.sh",
        root / "scripts/deploy/install_systemd.sh",
        root / "ros2_ws/src/bringup/launch/task_control.launch.py",
        root / "ros2_ws/src/bringup/launch/system_status.launch.py",
        root / "ros2_ws/src/data_recorder/launch/data_recorder.launch.py",
        root / "ros2_ws/src/task_manager/launch/task_manager.launch.py",
        root / "ros2_ws/src/system_monitor/launch/system_monitor.launch.py",
        root / "ros2_ws/src/data_recorder/src/data_recorder_node.cpp",
        root / "ros2_ws/src/task_manager/src/task_manager_node.cpp",
        root / "ros2_ws/src/system_monitor/src/system_monitor_node.cpp",
    ]
    combined = "\n".join(path.read_text(encoding="utf-8") for path in runtime_sources)
    assert "/home/cat/Project/capture_system" not in combined
    assert "/home/cat/.local/share/capture_system" not in combined
