from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def test_clearance_node_has_runtime_parameter_callback_for_devtools_whitelist() -> None:
    source = (PROJECT_ROOT / "ros2_ws/src/clearance_engine/src/clearance_engine_node.cpp").read_text()
    assert "add_on_set_parameters_callback" in source
    assert "raw_cluster.min_detection_x_m" in source
    assert "raw_cluster.max_detection_x_m" in source
    assert "raw_cluster.detection_radius_m" in source
    assert "raw_cluster.support_height_band_m" in source
    assert "raw_cluster.min_support_points" in source
    assert "raw_cluster.spatial_grid_size_m" in source
    assert "raw_cluster.min_occupied_cells" in source
    assert "raw_cluster.min_spatial_span_m" in source
    assert "estimator_mutex_" in source
