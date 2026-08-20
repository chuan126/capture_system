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
    assert "ransac.voxel_size_m: 0.04" in config_source
    assert "ransac.max_candidate_planes: 2500" in config_source
    assert "ransac.min_inliers_absolute: 50" in config_source
    assert "region.grid_size_m: 0.040" in config_source
    assert "region.min_span_cells: 8" in config_source
    assert "region.min_occupied_cells: 50" in config_source
