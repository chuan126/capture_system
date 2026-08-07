from pathlib import Path
from types import SimpleNamespace

import pytest

from backend.devtools.recording import (
    RAW_CLOUD_PROFILE,
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


def test_raw_cloud_recording_uses_fixed_mcap_profile(monkeypatch, tmp_path: Path) -> None:
    captured: dict[str, object] = {}
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
        return FakeProcess(command)

    monkeypatch.setattr("backend.devtools.recording.subprocess.Popen", fake_popen)

    manager = RosbagRecordingManager(tmp_path, min_free_bytes=1)
    status = manager.start(RAW_CLOUD_PROFILE, None)

    command = captured["command"]
    assert command[:6] == ["ros2", "bag", "record", "--storage", "mcap", "--output"]
    assert command[-1] == "/capture/lidar/points_raw"
    assert len(command) == 8
    assert status["active"] is True
    assert status["profile"] == "raw_cloud"
    assert str(tmp_path / "dev-tests" / "raw-cloud") in str(status["path"])
    assert captured["kwargs"]["start_new_session"] is True
    assert captured["kwargs"]["stdin"] is not None
    manager.stop()


def test_dev_recording_rejects_missing_mcap_plugin(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setattr("backend.devtools.recording.shutil.which", lambda name: "/usr/bin/ros2")
    monkeypatch.setattr(
        "backend.devtools.recording.shutil.disk_usage",
        lambda path: SimpleNamespace(total=20 * 1024**3, used=1, free=19 * 1024**3),
    )
    monkeypatch.setattr(
        "backend.devtools.recording.subprocess.run",
        lambda *args, **kwargs: SimpleNamespace(returncode=1),
    )
    manager = RosbagRecordingManager(tmp_path, min_free_bytes=1)

    with pytest.raises(DevRecordingError, match="rosbag2_storage_mcap"):
        manager.start(RAW_CLOUD_PROFILE, 5)


def test_dev_recording_rejects_unapproved_duration(tmp_path: Path) -> None:
    manager = RosbagRecordingManager(tmp_path, min_free_bytes=1)
    with pytest.raises(DevRecordingError, match="5、10或30秒"):
        manager.start(RAW_CLOUD_PROFILE, 7)
