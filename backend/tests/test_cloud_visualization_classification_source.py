from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
CLOUD_NODE = (
    PROJECT_ROOT
    / "ros2_ws"
    / "src"
    / "cloud_visualization"
    / "src"
    / "cloud_visualization_node.cpp"
)
TASK_CONFIG = PROJECT_ROOT / "ros2_ws/src/interfaces/msg/TaskClearanceConfig.msg"
RAW_DIAGNOSTICS = PROJECT_ROOT / "ros2_ws/src/interfaces/msg/RawClearanceDiagnostics.msg"
CLOUD_DIAGNOSTICS = PROJECT_ROOT / "ros2_ws/src/interfaces/msg/CloudPreviewDiagnostics.msg"
TASK_STATUS = PROJECT_ROOT / "ros2_ws/src/interfaces/msg/TaskStatus.msg"


def test_preview_classification_requires_exact_source_frame_match() -> None:
    source = CLOUD_NODE.read_text(encoding="utf-8")

    assert "diagnostics_by_stamp_.find(stampNs(cloud->header))" in source
    assert "diagnostics_by_stamp_[stamp] = message" in source
    assert "diagnostic_stamp_order_.size() > 16U" in source
    assert "diagnostics->input_point_count == point_count" in source
    assert "坐标近似" not in source


def test_diagnostics_qos_matches_non_blocking_bounded_publisher() -> None:
    preview_source = CLOUD_NODE.read_text(encoding="utf-8")
    clearance_source = (
        PROJECT_ROOT
        / "ros2_ws/src/clearance_engine/src/clearance_engine_node.cpp"
    ).read_text(encoding="utf-8")

    assert "rclcpp::KeepLast(16)).best_effort().durability_volatile()" in preview_source
    assert "rclcpp::KeepLast(16)).best_effort().durability_volatile()" in clearance_source


def test_preview_applies_red_green_blue_priority_on_device() -> None:
    source = CLOUD_NODE.read_text(encoding="utf-8")

    green_assignment = "classifications[index] = 1U"
    red_assignment = "classifications[index] = 2U"
    assert "std::vector<std::uint8_t> classifications(point_count, 0U)" in source
    assert green_assignment in source
    assert red_assignment in source
    assert source.index(green_assignment) < source.index(red_assignment)
    assert "diagnostics->roi_point_indices" in source
    assert "diagnostics->lowest_cluster_point_indices" in source


def test_red_requires_only_a_valid_detected_lowest_cluster() -> None:
    source = CLOUD_NODE.read_text(encoding="utf-8")

    assert "diagnostics->measurement_valid" in source
    assert "diagnostics->lowest_cluster_valid" in source
    assert "task_config->" not in source
    assert "clearance_threshold_m" not in source
    assert "clearance_upper_limit_m" not in source


def test_frozen_threshold_message_is_retained_but_does_not_control_preview() -> None:
    node_source = CLOUD_NODE.read_text(encoding="utf-8")
    config_fields = TASK_CONFIG.read_text(encoding="utf-8")
    task_status_fields = TASK_STATUS.read_text(encoding="utf-8")

    assert '"/capture/task/clearance_config"' not in node_source
    assert "TaskClearanceConfig" not in node_source
    for field in (
        "bool active",
        "bool parameters_valid",
        "float64 lidar_mount_height_m",
        "float64 clearance_threshold_m",
        "float64 clearance_upper_limit_m",
    ):
        assert field in config_fields
    assert "clearance_threshold_m" not in task_status_fields
    assert "clearance_upper_limit_m" not in task_status_fields


def test_raw_diagnostics_carries_original_index_classification_contract() -> None:
    fields = RAW_DIAGNOSTICS.read_text(encoding="utf-8")

    assert "uint32 input_point_count" in fields
    assert "uint32[] roi_point_indices" in fields
    assert "uint32[] lowest_cluster_point_indices" in fields
    assert "bool measurement_valid" in fields
    for stage in (
        "filtering_time_ms",
        "roi_time_ms",
        "sorting_time_ms",
        "support_band_time_ms",
        "connectivity_time_ms",
    ):
        assert f"float64 {stage}" in fields


def test_preview_diagnostics_exposes_classification_conversion_and_total_cost() -> None:
    fields = CLOUD_DIAGNOSTICS.read_text(encoding="utf-8")
    node_source = CLOUD_NODE.read_text(encoding="utf-8")

    for field in (
        "uint32 input_point_count",
        "uint32 output_point_count",
        "bool classification_matched",
        "float64 classification_time_ms",
        "float64 conversion_time_ms",
        "float64 total_processing_time_ms",
    ):
        assert field in fields
    assert '"/capture/visualization/diagnostics"' in node_source
