import json
import threading
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

from backend.devtools.recording import (
    ALGORITHM_DEBUG_PROFILE,
    FULL_DEBUG_PROFILE,
    RAW_CLOUD_PROFILE,
    RAW_SENSOR_PROFILE,
    DevRecordingError,
    RosbagRecordingManager,
)


class FakeProcess:
    def __init__(self, command: list[str]) -> None:
        self.command = command
        self.pid = 4321
        self.returncode = None

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        self.returncode = 0
        return 0


def prepare_runtime(monkeypatch, captured: dict[str, object]) -> None:
    monkeypatch.setattr("backend.devtools.recording.shutil.which", lambda name: "/usr/bin/ros2")
    monkeypatch.setattr(
        "backend.devtools.recording.shutil.disk_usage",
        lambda path: SimpleNamespace(total=20 * 1024**3, used=1, free=19 * 1024**3),
    )
    monkeypatch.setattr(
        "backend.devtools.recording.subprocess.run",
        lambda *args, **kwargs: SimpleNamespace(returncode=0),
    )
    monkeypatch.setattr("backend.devtools.recording.time.sleep", lambda seconds: None)
    monkeypatch.setattr("backend.devtools.recording.os.killpg", lambda pid, sig: None)

    def fake_popen(command, **kwargs):
        captured["command"] = command
        captured["kwargs"] = kwargs
        output_index = command.index("--output") + 1
        Path(command[output_index]).mkdir(parents=True)
        return FakeProcess(command)

    monkeypatch.setattr("backend.devtools.recording.subprocess.Popen", fake_popen)


def test_raw_cloud_recording_uses_fixed_mcap_profile(monkeypatch, tmp_path: Path) -> None:
    captured: dict[str, object] = {}
    prepare_runtime(monkeypatch, captured)
    manager = RosbagRecordingManager(tmp_path, min_free_bytes=1)
    status = manager.start(RAW_CLOUD_PROFILE, None)

    command = captured["command"]
    assert command[:6] == ["ros2", "bag", "record", "--storage", "mcap", "--output"]
    assert command[-2:] == [
        "/capture/lidar/points_raw",
        "/capture/odometry/high_rate_raw",
    ]
    assert len(command) == 9
    assert status["active"] is True
    assert status["profile"] == "raw_cloud"
    assert str(tmp_path / "dev-tests" / "raw-cloud") in str(status["path"])
    assert captured["kwargs"]["start_new_session"] is True
    assert captured["kwargs"]["stdin"] is not None
    manager.stop()


def test_raw_sensor_profile_records_existing_high_rate_sources_without_visual_or_temperature() -> None:
    assert RAW_SENSOR_PROFILE.topics == (
        "/capture/lidar/points_raw",
        "/capture/imu/data",
        "/capture/odometry/high_rate_raw",
        "/capture/odometry/slam",
        "/capture/lidar/device_online",
        "/capture/lidar/device_offline",
    )
    joined = "\n".join(RAW_SENSOR_PROFILE.topics).lower()
    assert "image" not in joined
    assert "camera" not in joined
    assert "temperature" not in joined
    assert "temp" not in joined


def test_algorithm_and_full_debug_profiles_keep_raw_and_processed_topics_separate() -> None:
    assert "/capture/lidar/points_raw" not in ALGORITHM_DEBUG_PROFILE.topics
    assert "/capture/lidar/points_compensated_enu" in ALGORITHM_DEBUG_PROFILE.topics
    assert "/capture/debug/frame_context" in ALGORITHM_DEBUG_PROFILE.topics
    assert "/capture/odometry/high_rate" in ALGORITHM_DEBUG_PROFILE.topics
    assert "/capture/recording/status" in ALGORITHM_DEBUG_PROFILE.topics
    assert "/diagnostics" in ALGORITHM_DEBUG_PROFILE.topics
    assert set(RAW_SENSOR_PROFILE.topics).issubset(FULL_DEBUG_PROFILE.topics)
    assert set(ALGORITHM_DEBUG_PROFILE.topics).issubset(FULL_DEBUG_PROFILE.topics)


def test_recording_writes_parameter_snapshot_and_source_hashes(monkeypatch, tmp_path: Path) -> None:
    captured: dict[str, object] = {}
    prepare_runtime(monkeypatch, captured)
    snapshot = {
        "schema_version": 1,
        "complete": True,
        "binding_config": {"path": "bindings.yaml", "sha256": "abc"},
        "source_configs": [{"path": "motion.yaml", "exists": True, "sha256": "def"}],
        "parameters": [{"key": "motion.test", "available": True, "value": 1.0}],
    }
    manager = RosbagRecordingManager(tmp_path, min_free_bytes=1, parameter_snapshot_provider=lambda: snapshot)
    status = manager.start(RAW_SENSOR_PROFILE, 5)
    path = Path(str(status["path"]))
    manifest = json.loads((path / "capture_manifest.json").read_text(encoding="utf-8"))
    for _ in range(100):
        if manager.status()["parameter_snapshot_complete"] is True:
            break
        threading.Event().wait(0.01)
    assert manager.status()["parameter_snapshot_complete"] is True
    saved_snapshot = json.loads((path / "parameter_snapshot.yaml").read_text(encoding="utf-8"))
    hashes = (path / "source_config_sha256.txt").read_text(encoding="utf-8")
    assert manifest["storage"] == "mcap"
    assert manifest["topic_downsampling"] is False
    assert manifest["topics"] == list(RAW_SENSOR_PROFILE.topics)
    assert saved_snapshot == snapshot
    assert "abc  bindings.yaml" in hashes
    assert "def  motion.yaml" in hashes
    assert manager.status()["parameter_snapshot_complete"] is True
    manager.stop()


def test_dev_recording_reports_rosbag_startup_failure_without_blocking_plugin_probe(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setattr("backend.devtools.recording.shutil.which", lambda name: "/usr/bin/ros2")
    monkeypatch.setattr(
        "backend.devtools.recording.shutil.disk_usage",
        lambda path: SimpleNamespace(total=20 * 1024**3, used=1, free=19 * 1024**3),
    )
    monkeypatch.setattr("backend.devtools.recording.time.sleep", lambda seconds: None)

    class FailedProcess(FakeProcess):
        def __init__(self, command):
            super().__init__(command)
            self.returncode = 1

    monkeypatch.setattr("backend.devtools.recording.subprocess.Popen", lambda command, **kwargs: FailedProcess(command))
    manager = RosbagRecordingManager(tmp_path, min_free_bytes=1)

    with pytest.raises(DevRecordingError, match="ros2 bag record启动失败"):
        manager.start(RAW_CLOUD_PROFILE, 5)


def test_dev_recording_rejects_unapproved_duration(tmp_path: Path) -> None:
    manager = RosbagRecordingManager(tmp_path, min_free_bytes=1)
    with pytest.raises(DevRecordingError, match="5、10或30秒"):
        manager.start(RAW_CLOUD_PROFILE, 7)


def test_recording_start_does_not_wait_for_parameter_snapshot(monkeypatch, tmp_path: Path) -> None:
    captured: dict[str, object] = {}
    prepare_runtime(monkeypatch, captured)
    snapshot_started = threading.Event()
    release_snapshot = threading.Event()
    snapshot_finished = threading.Event()

    def slow_snapshot():
        snapshot_started.set()
        release_snapshot.wait(timeout=2.0)
        snapshot_finished.set()
        return {
            "schema_version": 1,
            "complete": True,
            "binding_config": {"path": "bindings.yaml", "sha256": "abc"},
            "source_configs": [],
            "parameters": [],
        }

    manager = RosbagRecordingManager(
        tmp_path,
        min_free_bytes=1,
        parameter_snapshot_provider=slow_snapshot,
    )
    started_at = time.perf_counter()
    status = manager.start(RAW_CLOUD_PROFILE, None)
    elapsed = time.perf_counter() - started_at
    assert status["active"] is True
    assert snapshot_started.wait(timeout=0.2)
    assert elapsed < 0.2, "录制启动不得等待参数快照完成"
    assert manager.status()["parameter_snapshot_complete"] is None
    release_snapshot.set()
    assert snapshot_finished.wait(timeout=0.2)
    for _ in range(100):
        if manager.status()["parameter_snapshot_complete"] is True:
            break
        threading.Event().wait(0.01)
    assert manager.status()["parameter_snapshot_complete"] is True
    manager.stop()


def test_raw_cloud_recording_can_be_deleted_after_stop(monkeypatch, tmp_path: Path) -> None:
    captured: dict[str, object] = {}
    prepare_runtime(monkeypatch, captured)
    manager = RosbagRecordingManager(tmp_path, min_free_bytes=1)
    status = manager.start(RAW_CLOUD_PROFILE, 5)
    recording_id = str(status["recording_id"])
    path = Path(str(status["path"]))
    (path / "data.mcap").write_bytes(b"mcap")

    with pytest.raises(DevRecordingError, match="正在录制的文件不能删除"):
        manager.delete(recording_id)

    manager.stop()
    assert path.is_dir()
    record = manager.get_recording(recording_id)
    assert record["replay_ready"] is True
    assert isinstance(record["duration_seconds"], float)
    assert record["duration_seconds"] >= 0.0
    manager.delete(recording_id)
    assert not path.exists()


def test_old_raw_cloud_sample_without_odometry_is_marked_not_replay_ready(tmp_path: Path) -> None:
    manager = RosbagRecordingManager(tmp_path, min_free_bytes=1)
    recording_id = "raw-cloud_20260817_120000_abcdef"
    path = tmp_path / "dev-tests" / "raw-cloud" / recording_id
    path.mkdir(parents=True)
    (path / "capture_manifest.json").write_text(
        json.dumps({
            "schema_version": 1,
            "recording_id": recording_id,
            "profile": "raw_cloud",
            "topics": ["/capture/lidar/points_raw"],
        }),
        encoding="utf-8",
    )
    record = manager.get_recording(recording_id)
    assert record["replay_ready"] is False
    assert record["duration_seconds"] is None
