from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_system_monitor_uses_raw_pointcloud_freshness_for_lidar_online_state() -> None:
    source = (ROOT / "ros2_ws/src/system_monitor/src/system_monitor_node.cpp").read_text(encoding="utf-8")
    config = (ROOT / "ros2_ws/src/system_monitor/config/system_monitor.yaml").read_text(encoding="utf-8")
    cmake = (ROOT / "ros2_ws/src/system_monitor/CMakeLists.txt").read_text(encoding="utf-8")

    assert '"lidar_raw_topic", "/capture/lidar/points_raw"' in source
    assert "create_subscription<sensor_msgs::msg::PointCloud2>" in source
    assert "lidar_monitor_.observe" in source
    assert "lidar_monitor_.evaluate" in source
    assert "status.level = diagnostic_level(health.level)" in source
    assert "raw_age_ms" in source
    assert "lidar_raw_topic: /capture/lidar/points_raw" in config
    assert "find_package(sensor_msgs REQUIRED)" in cmake


def test_lidar_online_state_is_not_based_only_on_publisher_existence() -> None:
    source = (ROOT / "ros2_ws/src/system_monitor/src/system_monitor_node.cpp").read_text(encoding="utf-8")
    assert 'status.level = online_publishers > 0U ? DiagnosticStatus::OK' not in source
