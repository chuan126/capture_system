import json
from pathlib import Path

from backend.devtools.parameters import DevParameterService


PROJECT_ROOT = Path(__file__).resolve().parents[2]
BINDINGS = PROJECT_ROOT / "ros2_ws/src/bringup/config/dev_parameter_bindings.yaml"


class FakeBridge:
    def __init__(self, *, unavailable_node: str | None = None) -> None:
        self.available = True
        self.error = None
        self.unavailable_node = unavailable_node
        self.calls: list[tuple[str, tuple[str, ...]]] = []

    def get_parameters(self, node, names, timeout_seconds=1.5):
        self.calls.append((node, tuple(names)))
        if node == self.unavailable_node:
            raise RuntimeError(f"ROS图中未发现参数Service：{node}")
        values = {
            "raw_cluster.min_detection_x_m": 0.2,
            "raw_cluster.max_detection_x_m": 10.0,
            "raw_cluster.detection_radius_m": 1.0,
            "raw_cluster.support_height_band_m": 0.05,
            "raw_cluster.min_support_points": 10,
            "raw_cluster.spatial_grid_size_m": 0.1,
            "raw_cluster.min_occupied_cells": 3,
            "raw_cluster.min_spatial_span_m": 0.1,
        }
        return {name: values.get(name, 1.0) for name in names}

    def set_parameter(self, node, name, value, timeout_seconds=1.5):
        return value


def test_core_parameter_bindings_are_centralized_in_bringup_config() -> None:
    payload = json.loads(BINDINGS.read_text(encoding="utf-8"))
    assert payload["schema_version"] == 1
    keys = {item["key"] for item in payload["parameters"]}
    assert all(not key.startswith(("motion.", "odometry.")) for key in keys)
    assert "clearance.detection_radius_m" in keys
    assert "clearance.min_support_points" in keys
    assert "clearance.min_spatial_span_m" in keys
    for item in payload["parameters"]:
        source = PROJECT_ROOT / item["source_config"]
        assert source.exists(), item["source_config"]
        assert item["parameter"] in source.read_text(encoding="utf-8")
    assert "DIRECTORY launch config" in (PROJECT_ROOT / "ros2_ws/src/bringup/CMakeLists.txt").read_text(encoding="utf-8")


def test_parameter_refresh_batches_each_ros_node_once() -> None:
    bridge = FakeBridge()
    service = DevParameterService(bridge=bridge, bindings_path=BINDINGS)
    service.refresh_now()
    expected_nodes = {spec.node for spec in service.specs}
    assert {node for node, _names in bridge.calls} == expected_nodes
    assert len(bridge.calls) == len(expected_nodes)
    clearance_call = next(names for node, names in bridge.calls if node == "/clearance_engine_node")
    assert "raw_cluster.detection_radius_m" in clearance_call
    assert "raw_cluster.min_support_points" in clearance_call


def test_parameter_snapshot_uses_cached_runtime_values_and_source_hashes() -> None:
    bridge = FakeBridge()
    service = DevParameterService(bridge=bridge, bindings_path=BINDINGS)
    service.refresh_now()
    call_count = len(bridge.calls)
    snapshot = service.snapshot()
    assert snapshot["schema_version"] == 1
    assert snapshot["complete"] is True
    assert len(snapshot["parameters"]) == 8
    assert snapshot["binding_config"]["sha256"]
    assert all(item["exists"] is True and item["sha256"] for item in snapshot["source_configs"])
    assert len(bridge.calls) == call_count, "录制参数快照不得再次同步访问ROS"


def test_dashboard_exposes_only_requested_core_parameters() -> None:
    bridge = FakeBridge()
    service = DevParameterService(bridge=bridge, bindings_path=BINDINGS)
    service.refresh_now()
    call_count = len(bridge.calls)
    parameters = service.list_parameters(ui_only=True)
    assert len(bridge.calls) == call_count, "参数页面不得在HTTP请求路径同步访问ROS"
    assert [item["key"] for item in parameters] == [
        "clearance.min_detection_x_m",
        "clearance.detection_radius_m",
        "clearance.support_height_band_m",
        "clearance.min_support_points",
        "clearance.spatial_grid_size_m",
        "clearance.min_occupied_cells",
        "clearance.min_spatial_span_m",
    ]
    radius = next(item for item in parameters if item["key"] == "clearance.detection_radius_m")
    assert radius["writable"] is True
    assert radius["configured_value"] == 1.0
    assert radius["maximum"] == 5.0


def test_parameter_page_keeps_yaml_value_when_ros_bridge_is_unavailable() -> None:
    bridge = FakeBridge()
    bridge.available = False
    bridge.error = "ROS桥不可用"
    service = DevParameterService(bridge=bridge, bindings_path=BINDINGS)
    parameters = service.list_parameters(ui_only=True)
    assert len(parameters) == 7
    radius = next(item for item in parameters if item["parameter"] == "raw_cluster.detection_radius_m")
    assert radius["configured_value"] == 1.0
    assert radius["available"] is False
    assert radius["value"] is None


def test_clearance_node_failure_marks_all_runtime_parameters_unavailable() -> None:
    bridge = FakeBridge(unavailable_node="/clearance_engine_node")
    service = DevParameterService(bridge=bridge, bindings_path=BINDINGS)
    service.refresh_now()
    parameters = service.list_parameters()
    clearance = [item for item in parameters if item["node"] == "/clearance_engine_node"]
    assert clearance and all(item["available"] is False for item in clearance)
    assert len(clearance) == 8
