from pathlib import Path

import yaml


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def test_formal_motion_compensation_uses_fusion_position_only():
    motion_config = yaml.safe_load(
        (PROJECT_ROOT / "ros2_ws/src/motion_compensation/config/motion_compensation.yaml")
        .read_text(encoding="utf-8")
    )["enu_cloud_transform_node"]["ros__parameters"]
    fusion_source = (
        PROJECT_ROOT / "ros2_ws/src/localization/src/fusion_navigation_node.cpp"
    ).read_text(encoding="utf-8")

    assert motion_config["odometry_topic"] == "/capture/localization/fusion_odometry"
    assert "message->pose.pose.position" not in fusion_source
    assert "message->twist.twist.linear" not in fusion_source
    assert "speed_knots" not in fusion_source
    assert "track_degrees" not in fusion_source
    assert '"odin_position_used", "false"' in fusion_source
    assert '"odin_twist_linear_used", "false"' in fusion_source


def test_imu_packet_timestamps_are_expanded_without_driver_modification():
    sensor_launch = (
        PROJECT_ROOT / "ros2_ws/src/sensor_adapter/launch/odin_driver.launch.py"
    ).read_text(encoding="utf-8")
    adapter_config = yaml.safe_load(
        (PROJECT_ROOT / "ros2_ws/src/motion_compensation/config/imu_timestamp_adapter.yaml")
        .read_text(encoding="utf-8")
    )["imu_timestamp_adapter_node"]["ros__parameters"]

    assert '("imu", "/capture/imu/data_raw")' in sensor_launch
    assert adapter_config["input_topic"] == "/capture/imu/data_raw"
    assert adapter_config["output_topic"] == "/capture/imu/data"
    assert adapter_config["sample_rate_hz"] == 400.0


def test_bringup_launches_continuous_fusion_node():
    launch_source = (
        PROJECT_ROOT / "ros2_ws/src/bringup/launch/task_control.launch.py"
    ).read_text(encoding="utf-8")
    config = yaml.safe_load(
        (PROJECT_ROOT / "ros2_ws/src/localization/config/fusion_navigation.yaml")
        .read_text(encoding="utf-8")
    )["fusion_navigation_node"]["ros__parameters"]

    assert 'executable="fusion_navigation_node"' in launch_source
    assert 'name="fusion_navigation_node"' in launch_source
    assert config["fusion_odometry_topic"] == "/capture/localization/fusion_odometry"
    assert config["odin_orientation_topic"] == "/capture/odometry/high_rate"
    assert config["imu_topic"] == "/capture/imu/data"
    assert config["rtk_fix_topic"] == "/capture/rtk/fix"
    assert config["compensated_cloud_topic"] == "/capture/lidar/points_compensated_enu"


def test_status_contract_separates_local_global_and_heading_validity():
    message = (
        PROJECT_ROOT / "ros2_ws/src/interfaces/msg/LocalizationStatus.msg"
    ).read_text(encoding="utf-8")
    for field in (
        "bool local_navigation_valid",
        "bool global_position_valid",
        "bool heading_alignment_valid",
        "bool rtk_update_valid",
        "bool lidar_update_valid",
        "bool translation_compensation_valid",
        "float64 accelerometer_bias_x_mps2",
        "float64 position_std_m",
        "float64 velocity_std_mps",
    ):
        assert field in message


def test_fusion_configuration_has_quality_gates_for_all_updates():
    config = yaml.safe_load(
        (PROJECT_ROOT / "ros2_ws/src/localization/config/fusion_navigation.yaml")
        .read_text(encoding="utf-8")
    )["fusion_navigation_node"]["ros__parameters"]

    assert config["maximum_inertial_only_duration_s"] > 0
    assert config["maximum_translation_position_std_m"] > 0
    assert config["maximum_rtk_innovation_m"] > 0
    assert config["heading_fit_valid_baseline_m"] > config["heading_fit_min_baseline_m"]
    assert config["lidar.minimum_scan_points"] > 0
    assert config["lidar.maximum_fitness_score_m2"] > 0
    assert config["lidar.maximum_position_correction_m"] > 0
    assert "lidar.maximum_rotation_correction_deg" not in config
    assert config["lidar.minimum_inlier_ratio"] > 0
    assert config["lidar.minimum_observable_dof"] >= 4
    assert config["lidar.large_rotation_confirmation_frames"] >= 2


def test_fusion_attitude_uses_odin_increment_and_finite_angle_injection():
    navigator = (
        PROJECT_ROOT / "ros2_ws/src/localization/src/fusion_navigator.cpp"
    ).read_text(encoding="utf-8")
    finite_rotation = (
        PROJECT_ROOT
        / "ros2_ws/src/localization/src/finite_attitude_correction.cpp"
    ).read_text(encoding="utf-8")
    lidar = (
        PROJECT_ROOT / "ros2_ws/src/localization/src/lidar_localizer.cpp"
    ).read_text(encoding="utf-8")

    assert "previous_odin_orientation_.conjugate() * normalized" in navigator
    assert "state_.orientation_local_from_body * odin_increment" in navigator
    assert "applyFiniteAttitudeCorrection" in navigator
    assert "a2mat(input, output)" in finite_rotation
    assert "finiteAttitudeResetJacobian" in navigator
    assert "correctPose" in navigator
    assert "ICP_ROTATION_REJECTED" not in lidar
    assert "normalized_information" in lidar


def test_motion_compensation_consumes_complete_fusion_pose():
    transformer = (
        PROJECT_ROOT
        / "ros2_ws/src/motion_compensation/src/enu_cloud_transformer.cpp"
    ).read_text(encoding="utf-8")
    assert "p_fusion(t_i) - p_fusion(t_0)" in transformer
    assert "point_pose.quaternion_xyzw" in transformer
