import json
import stat
from pathlib import Path

from backend.device_settings import DeviceSettingsStore


def test_device_settings_persist_amap_secret_inside_runtime(tmp_path: Path) -> None:
    store = DeviceSettingsStore(tmp_path / "runtime")
    store.initialize()
    store.set_amap("web-key", "secret-code")
    assert store.get_amap() == ("web-key", "secret-code")
    assert store.path == tmp_path / "runtime/settings/device_settings.json"
    payload = json.loads(store.path.read_text(encoding="utf-8"))
    assert payload["amap"]["security_js_code"] == "secret-code"
    assert stat.S_IMODE(store.path.stat().st_mode) == 0o600
    assert stat.S_IMODE(store.settings_dir.stat().st_mode) == 0o700


def test_frontend_map_save_can_repair_a_corrupted_settings_file(tmp_path: Path) -> None:
    store = DeviceSettingsStore(tmp_path / "runtime")
    store.initialize()
    store.path.write_text("{broken", encoding="utf-8")
    store.set_amap("replacement-key", "replacement-secret")
    assert store.get_amap() == ("replacement-key", "replacement-secret")
    assert stat.S_IMODE(store.path.stat().st_mode) == 0o600
