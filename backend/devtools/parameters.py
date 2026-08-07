from __future__ import annotations

import json
import math
import subprocess
from dataclasses import dataclass


class DevParameterError(RuntimeError):
    pass


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


SPECS: tuple[ParameterSpec, ...] = (
    ParameterSpec("motion.max_interpolation_gap_s", "/enu_cloud_transform_node", "max_interpolation_gap_s", "最大姿态插值间隔", "s", "float", 0.001, 0.2, False, "当前节点未实现运行时更新，仅读取"),
    ParameterSpec("motion.minimum_valid_pose_ratio", "/enu_cloud_transform_node", "minimum_valid_pose_ratio", "最低姿态覆盖率", "", "float", 0.0, 1.0, False, "当前节点未实现运行时更新，仅读取"),
    ParameterSpec("motion.max_translation_per_scan_m", "/enu_cloud_transform_node", "max_translation_per_scan_m", "单帧最大平移", "m", "float", 0.0, 20.0, False, "当前节点未实现运行时更新，仅读取"),
    ParameterSpec("motion.odometry_time_offset_s", "/enu_cloud_transform_node", "odometry_time_offset_s", "里程计时间偏移", "s", "float", -1.0, 1.0, False, "当前节点未实现运行时更新，仅读取"),
    ParameterSpec("motion.cloud_time_offset_s", "/enu_cloud_transform_node", "cloud_time_offset_s", "点云时间偏移", "s", "float", -1.0, 1.0, False, "当前节点未实现运行时更新，仅读取"),
    ParameterSpec("clearance.distance_threshold_m", "/clearance_engine_node", "ransac.distance_threshold_m", "RANSAC距离阈值", "m", "float", 0.001, 0.5, True, "临时生效，重启恢复YAML值"),
    ParameterSpec("clearance.voxel_size_m", "/clearance_engine_node", "ransac.voxel_size_m", "体素尺寸", "m", "float", 0.001, 1.0, True, "临时生效"),
    ParameterSpec("clearance.max_candidate_planes", "/clearance_engine_node", "ransac.max_candidate_planes", "最大候选平面数", "", "int", 1, 100, True, "临时生效"),
    ParameterSpec("clearance.min_inliers_absolute", "/clearance_engine_node", "ransac.min_inliers_absolute", "最少内点", "", "int", 1, 1_000_000, True, "临时生效"),
    ParameterSpec("clearance.region_grid_size_m", "/clearance_engine_node", "region.grid_size_m", "区域网格尺寸", "m", "float", 0.001, 1.0, True, "临时生效"),
    ParameterSpec("clearance.min_region_occupied_cells", "/clearance_engine_node", "region.min_occupied_cells", "最少占用网格", "", "int", 1, 100_000, True, "临时生效"),
    ParameterSpec("clearance.max_residual_p95_m", "/clearance_engine_node", "region.max_residual_p95_m", "残差P95上限", "m", "float", 0.001, 1.0, True, "临时生效"),
)

_SPEC_BY_KEY = {spec.key: spec for spec in SPECS}


class DevParameterService:
    def __init__(self, timeout_seconds: float = 2.0) -> None:
        self.timeout_seconds = timeout_seconds

    def list_parameters(self) -> list[dict[str, object]]:
        return [self._read(spec) for spec in SPECS]

    def set_parameter(self, key: str, value: object) -> dict[str, object]:
        spec = _SPEC_BY_KEY.get(key)
        if spec is None:
            raise DevParameterError("参数不在开发白名单中")
        if not spec.writable:
            raise DevParameterError("该参数不允许运行时修改")
        normalized = self._normalize(spec, value)
        command_value = "true" if normalized is True else "false" if normalized is False else str(normalized)
        result = self._run(["ros2", "param", "set", spec.node, spec.parameter, command_value])
        output = result.stdout.strip()
        if result.returncode != 0 or "Successful" not in output:
            detail = result.stderr.strip() or output or f"退出状态{result.returncode}"
            raise DevParameterError(f"参数设置失败：{detail}")
        return self._read(spec)

    def _read(self, spec: ParameterSpec) -> dict[str, object]:
        result = self._run(["ros2", "param", "get", spec.node, spec.parameter])
        available = result.returncode == 0
        value: object | None = None
        detail = result.stderr.strip()
        if available:
            try:
                value = self._parse_get_value(result.stdout)
                detail = ""
            except DevParameterError as error:
                available = False
                detail = str(error)
        return {
            "key": spec.key,
            "node": spec.node,
            "parameter": spec.parameter,
            "label": spec.label,
            "unit": spec.unit,
            "kind": spec.kind,
            "minimum": spec.minimum,
            "maximum": spec.maximum,
            "writable": spec.writable,
            "note": spec.note,
            "available": available,
            "value": value,
            "detail": detail,
        }

    def _run(self, command: list[str]) -> subprocess.CompletedProcess[str]:
        try:
            return subprocess.run(command, text=True, capture_output=True, timeout=self.timeout_seconds, check=False)
        except FileNotFoundError as error:
            raise DevParameterError("未找到ros2命令") from error
        except subprocess.TimeoutExpired as error:
            raise DevParameterError("ROS参数操作超时") from error

    @staticmethod
    def _parse_get_value(output: str) -> object:
        text = output.strip()
        if ":" not in text:
            raise DevParameterError(f"无法解析ROS参数输出：{text}")
        value_text = text.split(":", 1)[1].strip()
        lowered = value_text.lower()
        if lowered in {"true", "false"}:
            return lowered == "true"
        try:
            if any(token in value_text for token in (".", "e", "E")):
                value = float(value_text)
                return value if math.isfinite(value) else None
            return int(value_text)
        except ValueError:
            try:
                return json.loads(value_text)
            except json.JSONDecodeError:
                return value_text.strip("'\"")

    @staticmethod
    def _normalize(spec: ParameterSpec, value: object) -> object:
        if spec.kind == "int":
            if isinstance(value, bool) or not isinstance(value, (int, float)) or int(value) != value:
                raise DevParameterError("参数必须为整数")
            normalized: object = int(value)
        elif spec.kind == "float":
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise DevParameterError("参数必须为数值")
            normalized = float(value)
            if not math.isfinite(normalized):
                raise DevParameterError("参数必须为有限数值")
        elif spec.kind == "bool":
            if not isinstance(value, bool):
                raise DevParameterError("参数必须为布尔值")
            normalized = value
        else:
            raise DevParameterError("参数类型未支持")
        numeric = float(normalized) if not isinstance(normalized, bool) else None
        if numeric is not None and spec.minimum is not None and numeric < spec.minimum:
            raise DevParameterError(f"参数不能小于{spec.minimum}")
        if numeric is not None and spec.maximum is not None and numeric > spec.maximum:
            raise DevParameterError(f"参数不能大于{spec.maximum}")
        return normalized
