from __future__ import annotations

import re
import sqlite3
from pathlib import Path


def test_data_recorder_cpp_schema_executes_without_duplicate_columns() -> None:
    source = (
        Path(__file__).resolve().parents[2]
        / "ros2_ws"
        / "src"
        / "data_recorder"
        / "src"
        / "data_recorder_node.cpp"
    ).read_text(encoding="utf-8")
    match = re.search(
        r"void create_schema\(\).*?execute\(database_, R\"SQL\((.*?)\)SQL\"\);",
        source,
        re.DOTALL,
    )
    assert match is not None

    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(match.group(1))
        metadata_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(recording_metadata)")
        }
        sample_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(clearance_samples)")
        }
    finally:
        connection.close()

    assert "lidar_mount_height_m" in metadata_columns
    assert "clearance_threshold_m" in metadata_columns
    assert {"travel_direction", "lane_side"}.issubset(metadata_columns)
    assert {
        "source_sequence",
        "source_age_ms",
        "is_repeated",
        "repeat_index",
    }.issubset(sample_columns)
    assert {
        "rtk_timestamp_ns",
        "gyro_x_rad_s",
        "gyro_y_rad_s",
        "gyro_z_rad_s",
        "accel_x_m_s2",
        "accel_y_m_s2",
        "accel_z_m_s2",
        "imu_sample_count",
        "radar_temperature_c",
        "minimum_point_x_m",
        "minimum_point_y_m",
        "minimum_point_z_m",
        "vehicle_pitch_deg",
        "vehicle_roll_deg",
        "vehicle_heading_deg",
        "odin_position_x_m",
        "odin_position_y_m",
        "odin_position_z_m",
    }.issubset(sample_columns)
    assert "imu_accumulator_ = ImuAccumulator{};" in source
    assert "message->vehicle_pitch_deg" in source
    assert "q2att(" not in source
    assert "insert_source_frame(source);" in source
    assert "DELETE FROM clearance_samples WHERE recorded_timestamp_ns > ?" in source



def test_data_recorder_stores_mount_adjusted_clearance_and_keeps_raw_algorithm_value() -> None:
    source = (
        Path(__file__).resolve().parents[2]
        / "ros2_ws"
        / "src"
        / "data_recorder"
        / "src"
        / "data_recorder_node.cpp"
    ).read_text(encoding="utf-8")

    assert "VALUES (1, 9, ?, 'recorded'" in source
    assert "clearance_height = *value + lidar_mount_height_m_" in source
    assert "bind_nullable_double(statement, 5, value);" in source
    assert "bind_nullable_double(statement, 6, clearance_height);" in source
    assert "request->lidar_mount_height_m < 0.0" in source
    assert "request->clearance_threshold_m < 0.0" in source


def test_vehicle_attitude_and_direction_use_expected_sources_without_position_side_effects() -> None:
    project_root = Path(__file__).resolve().parents[2]
    recorder = (project_root / "ros2_ws/src/data_recorder/src/data_recorder_node.cpp").read_text(
        encoding="utf-8"
    )
    localization_node = (
        project_root / "ros2_ws/src/localization/src/dead_reckoning_node.cpp"
    ).read_text(encoding="utf-8")
    page = (project_root / "frontend/app/page.tsx").read_text(encoding="utf-8")
    exporter = (project_root / "backend/exports/service.py").read_text(encoding="utf-8")

    assert localization_node.count("vehicleAttitudeFromOdinQuaternion(") == 1
    assert "message.vehicle_pitch_deg" in localization_node
    assert "message->vehicle_pitch_deg" in recorder
    assert "latest_localization_heading_.heading_deg = message->heading_deg" in recorder
    assert "localization_vehicle_pitch_deg" in page
    assert "localization_heading_deg" in page
    assert "formatMetric(rtkSnapshot?.localization_vehicle_heading_deg" not in page
    assert "sample.vehicle_pitch_deg" in exporter
    assert "q2att(" not in recorder

    rtk_heading = re.search(
        r"std::uint8_t currentHeadingSourceForRtk\(.*?\n  std::optional<Output>",
        localization_node,
        re.DOTALL,
    )
    assert rtk_heading is not None
    assert "latest_rtk_status_.track_degrees" in rtk_heading.group(0)
    assert "HEADING_RTK_TRACK" in rtk_heading.group(0)

    odometry_handler = re.search(
        r"void on_odometry\(.*?\n  void on_radar_temperature", recorder, re.DOTALL
    )
    assert odometry_handler is not None
    assert "orientation" not in odometry_handler.group(0)
    assert "latest_odin_.position_x_m = position.x" in odometry_handler.group(0)

    for relative_directory in ("ros2_ws/src/motion_compensation", "ros2_ws/src/clearance_engine"):
        directory = project_root / relative_directory
        source_text = "".join(
            path.read_text(encoding="utf-8", errors="ignore")
            for path in directory.rglob("*")
            if path.is_file()
        )
        assert "vehicle_attitude_mount_rotation_bm" not in source_text
