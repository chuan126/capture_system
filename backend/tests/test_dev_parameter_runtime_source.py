from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def test_clearance_node_has_runtime_parameter_callback_for_devtools_whitelist() -> None:
    source = (PROJECT_ROOT / "ros2_ws/src/clearance_engine/src/clearance_engine_node.cpp").read_text()
    assert "add_on_set_parameters_callback" in source
    assert "ransac.distance_threshold_m" in source
    assert "ransac.voxel_size_m" in source
    assert "ransac.max_candidate_planes" in source
    assert "ransac.min_inliers_absolute" in source
    assert "region.grid_size_m" in source
    assert "region.min_occupied_cells" in source
    assert "region.max_residual_p95_m" in source
    assert "estimator_mutex_" in source
