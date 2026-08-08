from __future__ import annotations

import hashlib
import json
import math
import os
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

from backend.devtools.parameter_bridge import DevParameterBridge, ParameterBridgeError


class DevParameterError(RuntimeError):
    pass


class ParameterBridgeLike(Protocol):
    @property
    def available(self) -> bool: ...
    error: str | None
    def get_parameters(self, node: str, names: list[str] | tuple[str, ...], timeout_seconds: float = 1.5) -> dict[str, object]: ...
    def set_parameter(self, node: str, name: str, value: object, timeout_seconds: float = 1.5) -> object: ...


@dataclass(frozen=True, slots=True)
class ParameterSpec:
    key: str
    node: str
    parameter: str
    label: str
    unit: str
    kind: str
    minimum: float | None = None
    maximum: float | None = None
    writable: bool = False
    note: str = ""
    source_config: str = ""
    ui_visible: bool = False


def _project_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _default_bindings_path() -> Path:
    configured = os.getenv("CAPTURE_DEV_PARAMETER_BINDINGS")
    if configured:
        path = Path(configured).expanduser()
        return (path if path.is_absolute() else _project_root() / path).resolve()
    return _project_root() / "ros2_ws" / "src" / "bringup" / "config" / "dev_parameter_bindings.yaml"


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _load_specs(path: Path) -> tuple[ParameterSpec, ...]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise DevParameterError(f"核心参数装订配置不存在：{path}") from error
    except (OSError, json.JSONDecodeError) as error:
        raise DevParameterError(f"核心参数装订配置读取失败：{error}") from error
    if not isinstance(raw, dict) or raw.get("schema_version") != 1 or not isinstance(raw.get("parameters"), list):
        raise DevParameterError("核心参数装订配置格式无效")
    specs: list[ParameterSpec] = []
    seen: set[str] = set()
    for index, item in enumerate(raw["parameters"]):
        if not isinstance(item, dict):
            raise DevParameterError(f"核心参数装订配置第{index + 1}项无效")
        try:
            spec = ParameterSpec(
                key=str(item["key"]), node=str(item["node"]), parameter=str(item["parameter"]),
                label=str(item["label"]), unit=str(item.get("unit", "")), kind=str(item["kind"]),
                minimum=None if item.get("minimum") is None else float(item["minimum"]),
                maximum=None if item.get("maximum") is None else float(item["maximum"]),
                writable=bool(item.get("writable", False)), note=str(item.get("note", "")),
                source_config=str(item.get("source_config", "")), ui_visible=bool(item.get("ui_visible", False)),
            )
        except (KeyError, TypeError, ValueError) as error:
            raise DevParameterError(f"核心参数装订配置第{index + 1}项字段无效") from error
        if not spec.key or spec.key in seen or spec.kind not in {"int", "float", "bool"}:
            raise DevParameterError(f"核心参数装订配置参数 {spec.key or index + 1} 无效")
        seen.add(spec.key)
        specs.append(spec)
    return tuple(specs)


class DevParameterService:
    def __init__(self, bridge: ParameterBridgeLike | None = None, bindings_path: Path | None = None, refresh_seconds: float = 1.0) -> None:
        self.bridge = bridge or DevParameterBridge()
        self.bindings_path = (bindings_path or _default_bindings_path()).resolve()
        self.specs = _load_specs(self.bindings_path)
        self._spec_by_key = {spec.key: spec for spec in self.specs}
        self.project_root = _project_root()
        self.refresh_seconds = max(0.2, refresh_seconds)
        self._cache_lock = threading.Lock()
        self._runtime_cache: dict[str, tuple[bool, object | None, str, int | None]] = {
            spec.key: (False, None, "尚未读取ROS运行参数", None) for spec in self.specs
        }
        self._stop_requested = threading.Event()
        self._refresh_thread: threading.Thread | None = None

    def start(self) -> None:
        if self._refresh_thread is not None:
            return
        self._stop_requested.clear()
        self._refresh_thread = threading.Thread(target=self._refresh_loop, name="dev-parameter-cache", daemon=True)
        self._refresh_thread.start()

    def stop(self, timeout_seconds: float = 2.0) -> None:
        self._stop_requested.set()
        if self._refresh_thread is not None:
            self._refresh_thread.join(timeout_seconds)
            self._refresh_thread = None

    def refresh_now(self) -> None:
        grouped: dict[str, list[ParameterSpec]] = {}
        for spec in self.specs:
            grouped.setdefault(spec.node, []).append(spec)
        captured_at = time.time_ns()
        updates: dict[str, tuple[bool, object | None, str, int | None]] = {}
        for node, specs in grouped.items():
            try:
                values = self.bridge.get_parameters(node, [spec.parameter for spec in specs], timeout_seconds=1.2)
                for spec in specs:
                    if spec.parameter in values:
                        updates[spec.key] = (True, values[spec.parameter], "", captured_at)
                    else:
                        updates[spec.key] = (False, None, f"节点未返回参数：{spec.parameter}", captured_at)
            except Exception as error:
                detail = str(error) or error.__class__.__name__
                for spec in specs:
                    updates[spec.key] = (False, None, detail, captured_at)
        with self._cache_lock:
            self._runtime_cache.update(updates)

    def _refresh_loop(self) -> None:
        while not self._stop_requested.is_set():
            if self.bridge.available:
                self.refresh_now()
            else:
                detail = self.bridge.error or "开发参数ROS桥不可用"
                with self._cache_lock:
                    for spec in self.specs:
                        self._runtime_cache[spec.key] = (False, None, detail, time.time_ns())
            self._stop_requested.wait(self.refresh_seconds)

    def list_parameters(self, *, ui_only: bool = False) -> list[dict[str, object]]:
        specs = (spec for spec in self.specs if spec.ui_visible) if ui_only else self.specs
        return [self._read_cached(spec) for spec in specs]

    def set_parameter(self, key: str, value: object) -> dict[str, object]:
        spec = self._spec_by_key.get(key)
        if spec is None:
            raise DevParameterError("参数不在开发白名单中")
        if not spec.writable:
            raise DevParameterError("该参数不允许运行时修改")
        normalized = self._normalize(spec, value)
        try:
            self.bridge.set_parameter(spec.node, spec.parameter, normalized, timeout_seconds=1.2)
        except (ParameterBridgeError, RuntimeError) as error:
            raise DevParameterError(str(error)) from error
        with self._cache_lock:
            self._runtime_cache[spec.key] = (True, normalized, "", time.time_ns())
        return self._read_cached(spec)

    def snapshot(self) -> dict[str, object]:
        parameters = self.list_parameters()
        source_paths = sorted({spec.source_config for spec in self.specs if spec.source_config})
        source_configs: list[dict[str, object]] = []
        for relative in source_paths:
            path = (self.project_root / relative).resolve()
            exists = path.is_file() and self.project_root in path.parents
            source_configs.append({"path": relative, "exists": exists, "sha256": _sha256_file(path) if exists else None})
        return {
            "schema_version": 1,
            "captured_at_ns": time.time_ns(),
            "complete": all(bool(item["available"]) for item in parameters),
            "binding_config": {
                "path": str(self.bindings_path.relative_to(self.project_root)) if self.project_root in self.bindings_path.parents else str(self.bindings_path),
                "sha256": _sha256_file(self.bindings_path),
            },
            "source_configs": source_configs,
            "parameters": parameters,
        }

    def _read_cached(self, spec: ParameterSpec) -> dict[str, object]:
        configured_value, config_available, config_detail = self._read_configured_value(spec)
        with self._cache_lock:
            available, value, detail, captured_at_ns = self._runtime_cache[spec.key]
        return {
            "key": spec.key, "node": spec.node, "parameter": spec.parameter, "label": spec.label,
            "unit": spec.unit, "kind": spec.kind, "minimum": spec.minimum, "maximum": spec.maximum,
            "writable": spec.writable, "note": spec.note, "source_config": spec.source_config,
            "ui_visible": spec.ui_visible, "config_available": config_available,
            "configured_value": configured_value, "config_detail": config_detail,
            "available": available, "value": value, "detail": detail,
            "runtime_captured_at_ns": captured_at_ns,
        }

    def _read_configured_value(self, spec: ParameterSpec) -> tuple[object | None, bool, str]:
        if not spec.source_config:
            return None, False, "未声明正式配置来源"
        path = (self.project_root / spec.source_config).resolve()
        if self.project_root not in path.parents or not path.is_file():
            return None, False, f"正式配置文件不存在：{spec.source_config}"
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as error:
            return None, False, f"正式配置文件读取失败：{error.__class__.__name__}"
        prefix = f"{spec.parameter}:"
        for raw_line in lines:
            line = raw_line.strip()
            if not line or line.startswith("#") or not line.startswith(prefix):
                continue
            raw_value = line[len(prefix):].split("#", 1)[0].strip()
            try:
                return self._parse_config_scalar(spec, raw_value), True, ""
            except DevParameterError as error:
                return None, False, str(error)
        return None, False, f"正式配置中未找到参数：{spec.parameter}"

    @staticmethod
    def _parse_config_scalar(spec: ParameterSpec, text: str) -> object:
        lowered = text.lower()
        if spec.kind == "bool":
            if lowered in {"true", "yes", "on"}: return True
            if lowered in {"false", "no", "off"}: return False
            raise DevParameterError(f"无法解析正式配置布尔值：{text}")
        try:
            return int(text, 10) if spec.kind == "int" else float(text)
        except ValueError as error:
            raise DevParameterError(f"无法解析正式配置数值：{text}") from error

    @staticmethod
    def _normalize(spec: ParameterSpec, value: object) -> object:
        if spec.kind == "bool":
            if not isinstance(value, bool): raise DevParameterError("参数必须为布尔值")
            return value
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise DevParameterError("参数必须为数值")
        number = float(value)
        if not math.isfinite(number): raise DevParameterError("参数必须为有限数值")
        if spec.minimum is not None and number < spec.minimum: raise DevParameterError(f"参数不能小于{spec.minimum}")
        if spec.maximum is not None and number > spec.maximum: raise DevParameterError(f"参数不能大于{spec.maximum}")
        if spec.kind == "int":
            if not number.is_integer(): raise DevParameterError("参数必须为整数")
            return int(number)
        return number
