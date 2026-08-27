from __future__ import annotations

import json
import os
import tempfile
import threading
from pathlib import Path


DEFAULT_DEEPSEEK_SKILL = """你是隧道净空测量证据审计器。输入是RK3588从最多2 GiB measurements.db只读流式计算得到的capture-clearance-audit-v2 analysis_package，不是数据库全表。不得要求重新上传全库、不得臆造未提供的数据，也不得修改原始记录。

字段口径：n必须原样取source_frame_statistics.valid_frames，禁止把最低帧数量或候选数量写成n。raw必须取source_frame_statistics.raw_min_m；f取与raw对应的最低真实源帧。设备端已经按source_sequence去重并排除无效、0、NaN和Inf高度，clearance_samples中的重复保持记录不能当成独立障碍。

判定时以点云证据为主体：综合raw_min_m、median_m、mad_m、p01_m、p05_m、rolling3_median_min_m、rolling5_median_min_m、lowest_20_real_frames的重复低值、selected_inlier_count以及candidate_contexts前后连续性。只有同时满足“孤立、明显突降、前后立即恢复、无连续或周期结构支持”才判O。连续低值、风机、横梁或周期结构判V；证据确实矛盾或不足时判R。V或R时eff必须等于raw，只有O允许从输入已有滚动统计或候选值中选择eff。

可信度c表示“对V/R/O判定结论的可信程度”，不是净空精度、RTK有效率或数据字段完整率。R表示有充分理由需要复核，结论明确时c可以较高，不能因为状态是R就自动限制在0.5以下。

评分时遵守以下规则：
- 真实有效源帧不少于20且source_valid_ratio不低于0.95，是强基础证据；不少于100帧且频率稳定时可进一步提高可信度。
- 多个相近低值、连续低值、3帧或5帧滚动统计支持、较高inlier数量，均应提高结论可信度；单个最低帧inlier偏少只降低该帧权重，不得抹去其他低值帧证据。
- RTK无效、continuous_distance_m为null只影响空间定位，不直接否定点云高度，对c的合计影响不得超过0.05。
- selected_grid_area_m2、selected_residual_p95_m或最低点XYZ等辅助字段缺失只写入q；已有帧数、连续性和inlier证据可用时，对c的合计影响不得超过0.10。
- 同一缺项不得通过多个描述重复扣分。q用于披露数据问题，不要求每个q都降低c。
- 证据一致的V通常为0.80–0.98；证据清楚但仍需现场确认的R通常为0.65–0.85；证据充分的O通常为0.80–0.98。
- 只有有效真实源帧少于5、核心高度大量无效或证据严重冲突时，c才应低于0.55。

报告依据必须诚实、保守但不过度惩罚辅助数据缺项。数据库={DB_PATH}；范围={TASK_OR_TIME_RANGE}。"""

_LEGACY_DEEPSEEK_SKILL_PREFIX = "你是隧道净空数据库审计器。直接读取SQLite数据库"
_OUTDATED_CONFIDENCE_SKILL_MARKER = "可信度c表示当前证据充分程度，不是统计概率"


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
                    "deepseek": {
                        "api_url": os.getenv(
                            "CAPTURE_DEEPSEEK_API_URL",
                            "https://api.deepseek.com/chat/completions",
                        ).strip(),
                        "api_key": os.getenv("CAPTURE_DEEPSEEK_API_KEY", "").strip(),
                        "model": os.getenv("CAPTURE_DEEPSEEK_MODEL", "deepseek-v4-flash").strip(),
                        "skill_prompt": DEFAULT_DEEPSEEK_SKILL,
                    },
                    "clearance_algorithm": {
                        "detection_radius_m": 1.0,
                        "min_support_points": 5,
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
                # 前端重新保存地图配置应能修复损坏的设备配置文件。
                payload = {"schema_version": 1}
            payload["schema_version"] = 1
            payload["amap"] = {"js_api_key": key, "security_js_code": code}
            self._write_locked(payload)

    def get_deepseek(self) -> dict[str, str]:
        with self._lock:
            payload = self._read_locked()
            deepseek = payload.get("deepseek") if isinstance(payload, dict) else None
            values = deepseek if isinstance(deepseek, dict) else {}
            stored_skill_prompt = str(values.get("skill_prompt") or DEFAULT_DEEPSEEK_SKILL)
            skill_prompt = stored_skill_prompt
            if (
                skill_prompt.startswith(_LEGACY_DEEPSEEK_SKILL_PREFIX)
                or _OUTDATED_CONFIDENCE_SKILL_MARKER in skill_prompt
            ):
                skill_prompt = DEFAULT_DEEPSEEK_SKILL
            if skill_prompt != stored_skill_prompt:
                migrated_values = dict(values)
                migrated_values["skill_prompt"] = skill_prompt
                payload["deepseek"] = migrated_values
                self._write_locked(payload)
            return {
                "api_url": str(
                    values.get("api_url") or "https://api.deepseek.com/chat/completions"
                ).strip(),
                "api_key": str(values.get("api_key") or "").strip(),
                "model": str(values.get("model") or "deepseek-v4-flash").strip(),
                "skill_prompt": skill_prompt,
            }

    def set_deepseek(
        self,
        api_url: str,
        api_key: str,
        model: str,
        skill_prompt: str,
    ) -> None:
        normalized_url = api_url.strip()
        normalized_key = api_key.strip()
        normalized_model = model.strip()
        normalized_skill = skill_prompt.strip()
        if (
            normalized_skill.startswith(_LEGACY_DEEPSEEK_SKILL_PREFIX)
            or _OUTDATED_CONFIDENCE_SKILL_MARKER in normalized_skill
        ):
            # 页面可能在后端升级前已经加载了旧命令词；保存时也执行迁移，
            # 避免旧页面把过度保守的评分规则重新写回设备端。
            normalized_skill = DEFAULT_DEEPSEEK_SKILL
        if not normalized_url.startswith(("https://", "http://")):
            raise DeviceSettingsError("DeepSeek API地址必须使用HTTP或HTTPS")
        if len(normalized_url) > 2048 or len(normalized_key) > 2048:
            raise DeviceSettingsError("DeepSeek API配置长度无效")
        if normalized_model not in {"deepseek-v4-flash", "deepseek-v4-pro"}:
            raise DeviceSettingsError("DeepSeek模型配置无效")
        if not normalized_skill or len(normalized_skill) > 20_000:
            raise DeviceSettingsError("大模型Skill长度必须在1至20000字符之间")
        if any(ord(char) < 32 and char not in "\r\n\t" for char in normalized_key + normalized_skill):
            raise DeviceSettingsError("DeepSeek配置包含非法控制字符")
        with self._lock:
            try:
                payload = self._read_locked()
            except DeviceSettingsError:
                payload = {"schema_version": 1}
            payload["schema_version"] = 1
            payload["deepseek"] = {
                "api_url": normalized_url,
                "api_key": normalized_key,
                "model": normalized_model,
                "skill_prompt": normalized_skill,
            }
            self._write_locked(payload)

    def get_clearance_algorithm(self) -> dict[str, float | int]:
        with self._lock:
            payload = self._read_locked()
            stored = payload.get("clearance_algorithm") if isinstance(payload, dict) else None
            values = stored if isinstance(stored, dict) else {}
            try:
                radius = float(values.get("detection_radius_m", 1.0))
                support = int(values.get("min_support_points", 5))
            except (TypeError, ValueError) as error:
                raise DeviceSettingsError("净空算法参数配置无效") from error
            if not 0.1 <= radius <= 5.0 or not 1 <= support <= 10_000:
                raise DeviceSettingsError("净空算法参数超出允许范围")
            return {"detection_radius_m": radius, "min_support_points": support}

    def set_clearance_algorithm(self, detection_radius_m: float, min_support_points: int) -> None:
        radius = float(detection_radius_m)
        support = int(min_support_points)
        if not 0.1 <= radius <= 5.0:
            raise DeviceSettingsError("圆柱检测半径必须在0.1至5.0 m之间")
        if not 1 <= support <= 10_000:
            raise DeviceSettingsError("最低簇支持点数必须在1至10000之间")
        with self._lock:
            payload = self._read_locked()
            payload["clearance_algorithm"] = {
                "detection_radius_m": radius,
                "min_support_points": support,
            }
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
