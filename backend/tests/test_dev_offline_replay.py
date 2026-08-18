from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest

from backend.devtools.offline_replay import (
    OFFLINE_CLEARANCE_TOPIC,
    OFFLINE_COMPENSATED_CLOUD_TOPIC,
    OFFLINE_ODOMETRY_TOPIC,
    OFFLINE_RAW_CLOUD_TOPIC,
    OFFLINE_RAW_ODOMETRY_TOPIC,
    OfflineReplayError,
    OfflineReplayManager,
)


PROJECT_ROOT = Path(__file__).resolve().parents[2]


class FakeRecordingManager:
    def __init__(self, root: Path, record: dict[str, object]) -> None:
        self.root = root.resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.record = record

    def status(self) -> dict[str, object]:
        return {"active": False}

    def get_recording(self, recording_id: str) -> dict[str, object]:
        assert recording_id == self.record["recording_id"]
        return dict(self.record)


def make_record(tmp_path: Path, *, replay_ready: bool = True) -> tuple[FakeRecordingManager, Path]:
    root = tmp_path / "dev-tests"
    recording_id = "raw-cloud_20260817_120000_abcdef"
    path = root / "raw-cloud" / recording_id
    path.mkdir(parents=True)
    record = {
        "recording_id": recording_id,
        "profile": "raw_cloud",
        "path": str(path),
        "active": False,
        "replay_ready": replay_ready,
        "duration_seconds": 12.5,
    }
    return FakeRecordingManager(root, record), path


def snapshot() -> dict[str, object]:
    return {
        "complete": True,
        "parameters": [
            {
                "key": "odometry.sample_rate_hz",
                "node": "/odometry_timestamp_adapter_node",
                "parameter": "sample_rate_hz",
                "available": True,
                "value": 400.0,
            },
            {
                "key": "motion.processing_poll_interval_ms",
                "node": "/enu_cloud_transform_node",
                "parameter": "processing_poll_interval_ms",
                "available": True,
                "value": 10,
            },
            {
                "key": "clearance.max_candidate_planes",
                "node": "/clearance_engine_node",
                "parameter": "ransac.max_candidate_planes",
                "available": True,
                "value": 2500,
            },
            {
                "key": "clearance.distance_threshold_m",
                "node": "/clearance_engine_node",
                "parameter": "ransac.distance_threshold_m",
                "available": True,
                "value": 0.04,
            },
        ],
    }


def test_offline_commands_reuse_formal_nodes_with_isolated_topics(tmp_path: Path) -> None:
    recording_manager, recording_path = make_record(tmp_path)
    manager = OfflineReplayManager(
        recording_manager, snapshot, project_root=PROJECT_ROOT,
        startup_delay_seconds=0.0, drain_delay_seconds=0.0,
    )
    overrides, fallback = manager._runtime_parameter_overrides(snapshot())
    assert fallback == []
    temp_dir = manager.temp_root / "command-test"
    temp_dir.mkdir()
    commands = manager._build_commands(recording_path, temp_dir, overrides)

    assert commands["odometry"][:4] == ["ros2", "run", "motion_compensation", "odometry_timestamp_adapter_node"]
    assert commands["motion"][:4] == ["ros2", "run", "motion_compensation", "enu_cloud_transform_node"]
    assert commands["clearance"][:4] == ["ros2", "run", "clearance_engine", "clearance_engine_node"]
    assert f"input_topic:={OFFLINE_RAW_ODOMETRY_TOPIC}" in commands["odometry"]
    assert f"output_topic:={OFFLINE_ODOMETRY_TOPIC}" in commands["odometry"]
    assert "sample_rate_hz:=400.0" in commands["odometry"]
    assert "sample_rate_hz:=400" not in commands["odometry"]
    assert f"input_cloud_topic:={OFFLINE_RAW_CLOUD_TOPIC}" in commands["motion"]
    assert f"odometry_topic:={OFFLINE_ODOMETRY_TOPIC}" in commands["motion"]
    assert f"output_cloud_topic:={OFFLINE_COMPENSATED_CLOUD_TOPIC}" in commands["motion"]
    assert f"input_topic:={OFFLINE_COMPENSATED_CLOUD_TOPIC}" in commands["clearance"]
    assert f"output_topic:={OFFLINE_CLEARANCE_TOPIC}" in commands["clearance"]
    assert "ransac.max_candidate_planes:=2500" in commands["clearance"]
    assert "processing_poll_interval_ms:=10" in commands["motion"]

    player = commands["player"]
    assert str(recording_path) in player
    assert "--remap" in player
    assert f"/capture/lidar/points_raw:={OFFLINE_RAW_CLOUD_TOPIC}" in player
    assert f"/capture/odometry/high_rate_raw:={OFFLINE_RAW_ODOMETRY_TOPIC}" in player

    assert (temp_dir / "odometry.yaml").read_text(encoding="utf-8").startswith("offline_odometry_timestamp_adapter_node:\n")
    assert (temp_dir / "motion.yaml").read_text(encoding="utf-8").startswith("offline_enu_cloud_transform_node:\n")
    assert (temp_dir / "clearance.yaml").read_text(encoding="utf-8").startswith("offline_clearance_engine_node:\n")



def test_offline_replay_rejects_while_any_dev_recording_is_active(monkeypatch, tmp_path: Path) -> None:
    recording_manager, _ = make_record(tmp_path)
    recording_manager.status = lambda: {"active": True, "profile": "algorithm_debug"}  # type: ignore[method-assign]
    monkeypatch.setattr("backend.devtools.offline_replay.shutil.which", lambda name: "/usr/bin/ros2")
    manager = OfflineReplayManager(recording_manager, snapshot, project_root=PROJECT_ROOT)
    with pytest.raises(OfflineReplayError, match="正在保存"):
        manager.start(str(recording_manager.record["recording_id"]))

def test_offline_replay_rejects_old_raw_cloud_sample_without_auxiliary_odometry(monkeypatch, tmp_path: Path) -> None:
    recording_manager, _ = make_record(tmp_path, replay_ready=False)
    monkeypatch.setattr("backend.devtools.offline_replay.shutil.which", lambda name: "/usr/bin/ros2")
    manager = OfflineReplayManager(recording_manager, snapshot, project_root=PROJECT_ROOT)
    with pytest.raises(OfflineReplayError, match="缺少原始高频里程计"):
        manager.start(str(recording_manager.record["recording_id"]))


def test_offline_result_statistics_use_ransac_plane_count(tmp_path: Path) -> None:
    recording_manager, _ = make_record(tmp_path)
    manager = OfflineReplayManager(recording_manager, snapshot, project_root=PROJECT_ROOT)
    with manager._lock:
        manager._state = "running"

    def message(*, ransac: int, valid: bool, clearance: float, processing: float, reason: str = ""):
        return SimpleNamespace(
            header=SimpleNamespace(stamp=SimpleNamespace(sec=7, nanosec=25)),
            ransac_plane_count=ransac,
            valid=valid,
            lidar_to_top_m=clearance,
            processing_time_ms=processing,
            invalid_reason=reason,
        )

    manager._on_result(message(ransac=4, valid=True, clearance=4.7, processing=8.1))
    manager._on_result(message(ransac=9, valid=True, clearance=4.5, processing=8.4))
    manager._on_result(message(ransac=2, valid=False, clearance=float("nan"), processing=7.9, reason="NO_PLANE"))
    status = manager.status()

    assert status["processed_frames"] == 3
    assert status["valid_frames"] == 2
    assert status["invalid_frames"] == 1
    assert status["ransac_plane_last"] == 2
    assert status["ransac_plane_mean"] == pytest.approx(5.0)
    assert status["ransac_plane_max"] == 9
    assert status["lidar_to_top_min_m"] == pytest.approx(4.5)
    assert status["lidar_to_top_mean_m"] == pytest.approx(4.6)
    assert status["lidar_to_top_max_m"] == pytest.approx(4.7)
    assert status["invalid_reason"] == "NO_PLANE"
    assert status["latest_stamp_ns"] == 7_000_000_025


def test_offline_completion_without_clearance_result_is_failure(tmp_path: Path) -> None:
    recording_manager, _ = make_record(tmp_path)
    manager = OfflineReplayManager(recording_manager, snapshot, project_root=PROJECT_ROOT)
    with manager._lock:
        manager._generation = 7
        manager._state = "running"
    manager._complete_generation(7, failed=None)
    status = manager.status()
    assert status["state"] == "failed"
    assert "未收到净空结果" in str(status["last_error"])

class _FakeProcess:
    def __init__(self, codes: list[int | None]) -> None:
        self.codes = list(codes)
        self.pid = 999999
        self.terminated = False

    def poll(self):
        if len(self.codes) > 1:
            return self.codes.pop(0)
        return self.codes[0] if self.codes else None


def test_offline_stopped_progress_is_frozen(monkeypatch, tmp_path: Path) -> None:
    recording_manager, _ = make_record(tmp_path)
    manager = OfflineReplayManager(recording_manager, snapshot, project_root=PROJECT_ROOT)
    clock = {"value": 100.0}
    monkeypatch.setattr("backend.devtools.offline_replay.time.monotonic", lambda: clock["value"])
    with manager._lock:
        manager._state = "running"
        manager._duration_seconds = 100.0
        manager._started_monotonic = 100.0
        manager._started_at_ns = 1
    observed_during_teardown: list[dict[str, object]] = []
    fake_process = object()
    with manager._lock:
        manager._processes["player"] = fake_process  # type: ignore[assignment]

    def slow_terminate(_process: object) -> None:
        clock["value"] = 180.0
        observed_during_teardown.append(manager.status())

    monkeypatch.setattr(manager, "_terminate_process", slow_terminate)
    clock["value"] = 137.0
    stopped = manager.stop()
    assert observed_during_teardown[0]["state"] == "stopping"
    assert observed_during_teardown[0]["progress"] == pytest.approx(0.37)
    assert observed_during_teardown[0]["elapsed_seconds"] == pytest.approx(37.0)
    assert stopped["state"] == "stopped"
    assert stopped["progress"] == pytest.approx(0.37)
    assert stopped["elapsed_seconds"] == pytest.approx(37.0)
    clock["value"] = 199.0
    later = manager.status()
    assert later["progress"] == pytest.approx(0.37)
    assert later["elapsed_seconds"] == pytest.approx(37.0)


def test_offline_invalid_frame_keeps_last_valid_clearance(tmp_path: Path) -> None:
    recording_manager, _ = make_record(tmp_path)
    manager = OfflineReplayManager(recording_manager, snapshot, project_root=PROJECT_ROOT)
    with manager._lock:
        manager._state = "running"

    valid = SimpleNamespace(
        header=SimpleNamespace(stamp=SimpleNamespace(sec=1, nanosec=0)),
        ransac_plane_count=3,
        valid=True,
        lidar_to_top_m=4.75,
        processing_time_ms=8.0,
        invalid_reason="",
    )
    invalid = SimpleNamespace(
        header=SimpleNamespace(stamp=SimpleNamespace(sec=2, nanosec=0)),
        ransac_plane_count=0,
        valid=False,
        lidar_to_top_m=float("nan"),
        processing_time_ms=7.5,
        invalid_reason="NO_PLANE",
    )
    manager._on_result(valid)
    manager._on_result(invalid)
    status = manager.status()
    assert status["latest_result_valid"] is False
    assert status["lidar_to_top_last_m"] == pytest.approx(4.75)
    assert status["invalid_reason"] == "NO_PLANE"


def test_offline_monitor_reports_algorithm_exit_without_waiting_for_player(monkeypatch, tmp_path: Path) -> None:
    recording_manager, _ = make_record(tmp_path)
    manager = OfflineReplayManager(recording_manager, snapshot, project_root=PROJECT_ROOT, drain_delay_seconds=0.0)
    player = _FakeProcess([None, None, None])
    clearance = _FakeProcess([17])
    with manager._lock:
        manager._generation = 4
        manager._state = "running"
        manager._processes = {"clearance": clearance, "player": player}  # type: ignore[assignment]
    captured: list[str | None] = []
    monkeypatch.setattr(manager, "_complete_generation", lambda generation, failed: captured.append(failed))
    manager._monitor_player(4, player)  # type: ignore[arg-type]
    assert captured
    assert "clearance" in str(captured[0])
    assert "17" in str(captured[0])


def test_offline_diagnostics_are_exposed_in_status(tmp_path: Path) -> None:
    recording_manager, _ = make_record(tmp_path)
    manager = OfflineReplayManager(recording_manager, snapshot, project_root=PROJECT_ROOT)
    with manager._lock:
        manager._state = "running"
    diagnostic = SimpleNamespace(
        status=[SimpleNamespace(values=[
            SimpleNamespace(key="clouds_received_total", value="12"),
            SimpleNamespace(key="clouds_processed_total", value="9"),
            SimpleNamespace(key="interpolation_failure_count", value="2"),
            SimpleNamespace(key="pending_cloud_count", value="1"),
            SimpleNamespace(key="queue_wait_ms_mean", value="3.25"),
        ])]
    )
    manager._on_diagnostics(diagnostic)
    values = manager.status()["diagnostics"]
    assert values["clouds_received_total"] == 12
    assert values["clouds_processed_total"] == 9
    assert values["interpolation_failure_count"] == 2
    assert values["pending_cloud_count"] == 1
    assert values["queue_wait_ms_mean"] == pytest.approx(3.25)
