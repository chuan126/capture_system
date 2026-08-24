import json
import stat
from pathlib import Path

from backend.device_settings import DEFAULT_DEEPSEEK_SKILL, DeviceSettingsStore


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


def test_deepseek_api_key_endpoint_and_skill_persist_inside_runtime(tmp_path: Path) -> None:
    store = DeviceSettingsStore(tmp_path / "runtime")
    store.initialize()
    store.set_deepseek(
        "https://api.deepseek.com/chat/completions",
        "sk-device-runtime",
        "deepseek-v4-pro",
        "数据库={DB_PATH}；范围={TASK_OR_TIME_RANGE}。",
    )
    assert store.get_deepseek() == {
        "api_url": "https://api.deepseek.com/chat/completions",
        "api_key": "sk-device-runtime",
        "model": "deepseek-v4-pro",
        "skill_prompt": "数据库={DB_PATH}；范围={TASK_OR_TIME_RANGE}。",
    }
    payload = json.loads(store.path.read_text(encoding="utf-8"))
    assert payload["deepseek"]["api_key"] == "sk-device-runtime"


def test_legacy_full_database_skill_is_migrated_to_bounded_audit_skill(
    tmp_path: Path,
) -> None:
    store = DeviceSettingsStore(tmp_path / "runtime")
    store.initialize()
    legacy = (
        "你是隧道净空数据库审计器。直接读取SQLite数据库，不加载或输出全表，"
        "不使用SELECT *。"
    )
    store.set_deepseek(
        "https://api.deepseek.com/chat/completions",
        "sk-device-runtime",
        "deepseek-v4-flash",
        legacy,
    )

    loaded = store.get_deepseek()

    assert loaded["skill_prompt"] == DEFAULT_DEEPSEEK_SKILL
    assert "capture-clearance-audit-v2" in loaded["skill_prompt"]
    assert "不是数据库全表" in loaded["skill_prompt"]


def test_outdated_overly_conservative_confidence_skill_is_migrated(
    tmp_path: Path,
) -> None:
    store = DeviceSettingsStore(tmp_path / "runtime")
    store.initialize()
    payload = json.loads(store.path.read_text(encoding="utf-8"))
    payload["deepseek"]["skill_prompt"] = (
        "分析规则。可信度c表示当前证据充分程度，不是统计概率。"
    )
    store.path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")

    loaded = store.get_deepseek()

    assert loaded["skill_prompt"] == DEFAULT_DEEPSEEK_SKILL
    assert "对V/R/O判定结论的可信程度" in loaded["skill_prompt"]
    assert "合计影响不得超过0.05" in loaded["skill_prompt"]
    persisted = json.loads(store.path.read_text(encoding="utf-8"))
    assert persisted["deepseek"]["skill_prompt"] == DEFAULT_DEEPSEEK_SKILL
