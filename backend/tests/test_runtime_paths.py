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


def test_autostart_scripts_use_project_relative_paths_and_do_not_create_runtime_during_apply() -> None:
    root = Path(__file__).resolve().parents[2]
    apply = (root / "scripts/deploy/apply_autostart.sh").read_text(encoding="utf-8")
    ready = (root / "scripts/operation/check_autostart_ready.sh").read_text(encoding="utf-8")
    stop = (root / "scripts/operation/stop_capture_system.sh").read_text(encoding="utf-8")
    assert 'capture_project_root' in apply
    assert 'project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"' in ready
    assert '/home/firefly/project/capture_system' not in apply + ready + stop
    assert 'mkdir -p "${project_root}/runtime' not in apply


def test_autostart_readiness_does_not_block_on_lidar_or_network() -> None:
    root = Path(__file__).resolve().parents[2]
    ready = (root / "scripts/operation/check_autostart_ready.sh").read_text(encoding="utf-8")
    service = (root / "system/systemd/capture-system.service").read_text(encoding="utf-8")
    assert "雷达可稍后连接" in ready
    assert "仍将启动并等待网络恢复" in ready
    assert "等待开机网络就绪超时" not in ready
    assert "CAPTURE_AUTOSTART_READY_TIMEOUT_S" not in ready + service
