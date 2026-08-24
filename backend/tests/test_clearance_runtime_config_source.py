from pathlib import Path


def test_bringup_default_clearance_profile_is_the_documented_tunnel_profile() -> None:
    root = Path(__file__).parents[2]
    config_name = "clearance_engine_tunnel_4cm.yaml"
    launch_source = (
        root / "ros2_ws" / "src" / "bringup" / "launch" / "clearance_preview.launch.py"
    ).read_text(encoding="utf-8")
    offline_source = (root / "backend" / "devtools" / "offline_replay.py").read_text(
        encoding="utf-8"
    )
    binding_source = (
        root / "ros2_ws" / "src" / "bringup" / "config" / "dev_parameter_bindings.yaml"
    ).read_text(encoding="utf-8")
    config_source = (
        root
        / "ros2_ws"
        / "src"
        / "clearance_engine"
        / "config"
        / config_name
    ).read_text(encoding="utf-8")

    assert f'"{config_name}"' in launch_source
    assert config_name in offline_source
    assert config_name in binding_source
    assert "clearance_engine_small_board_1cm.yaml" not in offline_source
    assert "clearance_engine_small_board_1cm.yaml" not in binding_source
    assert "input_topic: /capture/lidar/points_raw" in config_source
    assert "raw_cluster.min_detection_x_m: 1.0" in config_source
    assert "raw_cluster.max_detection_x_m: 10.0" in config_source
    assert "raw_cluster.detection_radius_m: 1.0" in config_source
    assert "raw_cluster.support_height_band_m: 0.05" in config_source
    assert "raw_cluster.min_support_points: 10" in config_source
    assert "raw_cluster.spatial_grid_size_m: 0.10" in config_source
    assert "raw_cluster.min_occupied_cells: 3" in config_source
    assert "raw_cluster.min_spatial_span_m: 0.10" in config_source
    assert "ransac." not in config_source
    assert "region." not in config_source
