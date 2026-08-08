from pathlib import Path


def test_bringup_default_clearance_profile_is_the_documented_one_centimeter_profile() -> None:
    root = Path(__file__).parents[2]
    launch_source = (
        root / "ros2_ws" / "src" / "bringup" / "launch" / "clearance_preview.launch.py"
    ).read_text(encoding="utf-8")
    config_source = (
        root
        / "ros2_ws"
        / "src"
        / "clearance_engine"
        / "config"
        / "clearance_engine_small_board_1cm.yaml"
    ).read_text(encoding="utf-8")

    assert '"clearance_engine_small_board_1cm.yaml"' in launch_source
    assert "ransac.voxel_size_m: 0.04" in config_source
    assert "ransac.max_candidate_planes: 8" in config_source
    assert "ransac.min_inliers_absolute: 50" in config_source
    assert "region.grid_size_m: 0.010" in config_source
