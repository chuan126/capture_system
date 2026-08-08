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
            "ransac.distance_threshold_m": 0.04,
            "region.grid_size_m": 0.01,
            "region.min_span_cells": 5,
            "region.min_occupied_cells": 15,
            "region.max_residual_p95_m": 0.05,
            "ransac.min_inliers_absolute": 50,
            "ransac.max_candidate_planes": 8,
            "ransac.min_remaining_points": 100,
        }
        return {name: values.get(name, 1.0) for name in names}

    def set_parameter(self, node, name, value, timeout_seconds=1.5):
        return value


def test_core_parameter_bindings_are_centralized_in_bringup_config() -> None:
    payload = json.loads(BINDINGS.read_text(encoding="utf-8"))
    assert payload["schema_version"] == 1
    keys = {item["key"] for item in payload["parameters"]}
    assert "motion.odometry_time_offset_s" in keys
    assert "odometry.sample_rate_hz" in keys
    assert "clearance.distance_threshold_m" in keys
    assert "clearance.min_remaining_points" in keys
    assert "clearance.min_region_span_cells" in keys
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
    assert "ransac.distance_threshold_m" in clearance_call
    assert "ransac.min_remaining_points" in clearance_call


def test_parameter_snapshot_uses_cached_runtime_values_and_source_hashes() -> None:
    bridge = FakeBridge()
    service = DevParameterService(bridge=bridge, bindings_path=BINDINGS)
    service.refresh_now()
    call_count = len(bridge.calls)
    snapshot = service.snapshot()
    assert snapshot["schema_version"] == 1
    assert snapshot["complete"] is True
    assert len(snapshot["parameters"]) >= 10
    assert snapshot["binding_config"]["sha256"]
    assert all(item["exists"] is True and item["sha256"] for item in snapshot["source_configs"])
    assert len(bridge.calls) == call_count, "录制参数快照不得再次同步访问ROS"


def test_parameter_page_exposes_only_requested_clearance_parameters() -> None:
    bridge = FakeBridge()
    service = DevParameterService(bridge=bridge, bindings_path=BINDINGS)
    service.refresh_now()
    call_count = len(bridge.calls)
    parameters = service.list_parameters(ui_only=True)
    assert len(bridge.calls) == call_count, "参数页面不得在HTTP请求路径同步访问ROS"
    assert [item["parameter"] for item in parameters] == [
        "ransac.distance_threshold_m",
        "ransac.max_candidate_planes",
        "ransac.min_remaining_points",
        "ransac.min_inliers_absolute",
        "region.grid_size_m",
        "region.min_span_cells",
        "region.min_occupied_cells",
        "region.max_residual_p95_m",
    ]
    assert next(item for item in parameters if item["parameter"] == "ransac.min_remaining_points")["value"] == 100
    assert next(item for item in parameters if item["parameter"] == "region.min_span_cells")["writable"] is False


def test_parameter_page_keeps_yaml_value_when_ros_bridge_is_unavailable() -> None:
    bridge = FakeBridge()
    bridge.available = False
    bridge.error = "ROS桥不可用"
    service = DevParameterService(bridge=bridge, bindings_path=BINDINGS)
    parameters = service.list_parameters(ui_only=True)
    assert len(parameters) == 8
    distance = next(item for item in parameters if item["parameter"] == "ransac.distance_threshold_m")
    remaining = next(item for item in parameters if item["parameter"] == "ransac.min_remaining_points")
    assert distance["configured_value"] == 0.04
    assert distance["available"] is False
    assert distance["value"] is None
    assert remaining["configured_value"] == 100


def test_one_node_failure_does_not_hide_other_nodes() -> None:
    bridge = FakeBridge(unavailable_node="/clearance_engine_node")
    service = DevParameterService(bridge=bridge, bindings_path=BINDINGS)
    service.refresh_now()
    parameters = service.list_parameters()
    clearance = [item for item in parameters if item["node"] == "/clearance_engine_node"]
    other = [item for item in parameters if item["node"] != "/clearance_engine_node"]
    assert clearance and all(item["available"] is False for item in clearance)
    assert any(item["available"] is True for item in other)
