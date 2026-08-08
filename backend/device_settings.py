from __future__ import annotations

import json
import os
import tempfile
import threading
from pathlib import Path


class DeviceSettingsError(RuntimeError):
    pass


class DeviceSettingsStore:
    def __init__(self, data_root: Path) -> None:
        self.data_root = data_root.resolve()
        self.settings_dir = self.data_root / "settings"
        self.path = self.settings_dir / "device_settings.json"
        self._lock = threading.RLock()

    def initialize(self) -> None:
        with self._lock:
            self.settings_dir.mkdir(parents=True, exist_ok=True)
            os.chmod(self.settings_dir, 0o700)
            if not self.path.exists():
                initial = {
                    "schema_version": 1,
                    "amap": {
                        "js_api_key": os.getenv("CAPTURE_AMAP_JS_KEY", "").strip(),
                        "security_js_code": os.getenv("CAPTURE_AMAP_SECURITY_CODE", "").strip(),
                    },
                }
                self._write_locked(initial)
            else:
                self._read_locked()

    def get_amap(self) -> tuple[str, str]:
        with self._lock:
            payload = self._read_locked()
            amap = payload.get("amap") if isinstance(payload, dict) else None
            if not isinstance(amap, dict):
                return "", ""
            return str(amap.get("js_api_key") or "").strip(), str(amap.get("security_js_code") or "").strip()

    def set_amap(self, js_api_key: str, security_js_code: str) -> None:
        key = js_api_key.strip()
        code = security_js_code.strip()
        if not key or not code:
            raise DeviceSettingsError("地图 Key 和安全密钥不能为空")
        if len(key) > 256 or len(code) > 512:
            raise DeviceSettingsError("地图配置长度无效")
        if any(ord(char) < 32 for char in key + code):
            raise DeviceSettingsError("地图配置包含非法控制字符")
        with self._lock:
            try:
                payload = self._read_locked()
            except DeviceSettingsError:
                # 前端重新保存地图配置应能修复损坏的设备配置文件。当前 schema 仅包含 amap。
                payload = {"schema_version": 1}
            payload["schema_version"] = 1
            payload["amap"] = {"js_api_key": key, "security_js_code": code}
            self._write_locked(payload)

    def _read_locked(self) -> dict[str, object]:
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return {"schema_version": 1, "amap": {}}
        except (OSError, json.JSONDecodeError) as error:
            raise DeviceSettingsError(f"设备配置读取失败：{error.__class__.__name__}") from error
        if not isinstance(raw, dict) or raw.get("schema_version") != 1:
            raise DeviceSettingsError("设备配置文件格式无效")
        return raw

    def _write_locked(self, payload: dict[str, object]) -> None:
        self.settings_dir.mkdir(parents=True, exist_ok=True)
        os.chmod(self.settings_dir, 0o700)
        fd, temporary = tempfile.mkstemp(prefix="device_settings.", suffix=".tmp", dir=self.settings_dir)
        temporary_path = Path(temporary)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                json.dump(payload, stream, ensure_ascii=False, indent=2)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.chmod(temporary_path, 0o600)
            os.replace(temporary_path, self.path)
            os.chmod(self.path, 0o600)
        except Exception:
            temporary_path.unlink(missing_ok=True)
            raise
