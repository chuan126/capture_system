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
        sample_indexes = {
            row[1] for row in connection.execute("PRAGMA index_list(clearance_samples)")
        }
        imu_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(imu_samples)")
        }
    finally:
        connection.close()

    assert "lidar_mount_height_m" in metadata_columns
    assert "clearance_threshold_m" in metadata_columns
    assert "clearance_upper_limit_m" in metadata_columns
    assert {"detection_radius_m", "min_support_points"}.issubset(metadata_columns)
    assert {"travel_direction", "lane_side", "lane_number_from_right"}.issubset(metadata_columns)
    assert {
        "source_sequence",
        "source_age_ms",
        "is_repeated",
        "repeat_index",
    }.issubset(sample_columns)
    assert "clearance_samples_recorded_timestamp_idx" in sample_indexes
    assert {
        "rtk_timestamp_ns",
        "rtk_latitude_deg",
        "rtk_longitude_deg",
        "rtk_altitude_m",
        "rtk_satellite_count",
        "rtk_hdop",
        "rtk_pdop",
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
        "odin_qx",
        "odin_qy",
        "odin_qz",
        "odin_qw",
    }.issubset(sample_columns)
    assert "imu_accumulator_ = ImuAccumulator{};" in source
    assert {
        "recorded_timestamp_ns",
        "clearance_height_m",
        "minimum_clearance_height_m",
        "rtk_timestamp_ns",
        "gyro_x_rad_s",
        "accel_z_m_s2",
        "minimum_point_x_m",
        "odin_qw",
    }.issubset(imu_columns)
    assert "insert_imu_sample(*message);" in source
    assert "const bool imu_values_finite" in source
    assert "非有限分量只在该行落0，不丢整行" in source
    assert "finite_or_zero(message.angular_velocity.x)" in source
    assert "finite_or_zero(message.linear_acceleration.z)" in source
    assert "bind_nullable_double(statement, 36, std::nullopt);" in source
    assert "bind_nullable_double(statement, 37, std::nullopt);" in source
    assert "bind_nullable_double(statement, 38, std::nullopt);" in source
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

    assert "VALUES (1, 15, ?, 'recorded'" in source
    assert "clearance_height = *value + lidar_mount_height_m_" in source
    assert "bind_nullable_double(statement, 5, value);" in source
    assert "bind_nullable_double(statement, 6, clearance_height);" in source
    assert "request->lidar_mount_height_m < 0.0" in source
    assert "request->clearance_threshold_m < 0.0" in source
    assert "request->clearance_threshold_m > request->clearance_upper_limit_m" in source
    assert "request->detection_radius_m < 0.1" in source
    assert "request->min_support_points < 1U" in source
    assert "write_periodic_sample();" not in source
    assert "write_periodic_sample" in source
    assert "sample_timer_" in source
    assert "latest.source_timestamp_ns == last_received_clearance_timestamp_ns_" in source
    assert "DELETE FROM imu_samples WHERE recorded_timestamp_ns > ?" in source


def test_data_recorder_uses_manual_rtk_endpoints_and_stops_immediately() -> None:
    source = (
        Path(__file__).resolve().parents[2]
        / "ros2_ws/src/data_recorder/src/data_recorder_node.cpp"
    ).read_text(encoding="utf-8")

    assert 'request->command == "capture_entry_rtk"' in source
    assert 'request->command == "capture_exit_rtk"' in source
    assert 'capture_manual_endpoint("entry"' in source
    assert 'capture_manual_endpoint("exit"' in source
    assert 'finalize_recording(requested_ns, true, *response)' in source
    assert 'capture_endpoint("entry", start_requested_ns_)' not in source
    finalize_body = source[source.index("void finalize_recording("):source.index("void on_clearance(")]
    assert 'capture_endpoint("exit", requested_ns)' not in finalize_body
    assert 'entry_rtk_snapshot.db' in source
    assert 'capture_prestart_entry(request->task_id' in source
    assert 'load_staged_entry_snapshot(task_id_)' in source
    assert 'import_staged_entry_snapshot(*staged_entry)' in source
    assert 'entry_rtk_status_ = staged_entry->valid ? "confirmed" : "unconfirmed"' in source
    assert 'for (const auto * suffix : {"", "-wal", "-shm"})' in source
    assert 'fs::remove(fs::path(staging_path.string() + suffix), remove_error)' in source
    assert 'capture_poststop_exit(request->task_id, requested_ns' in source
    assert 'SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX' in source
    assert '"exit_rtk_poststop_capture"' in source
    assert '"出口RTK快照已在停止后记录（定位有效）"' in source
    assert '"?, \'not_requested\')"' in source
    assert "waiting_exit_rtk_" not in source
    assert "exit_rtk_wait_timeout_ms" not in source
    assert "previous_status == \"confirmed\"" in source
    assert 'endpoint_name + "RTK快照已记录（当前无有效定位）"' in source
    assert 'endpoint_name + "RTK未记录"' not in source


def test_recorder_keeps_raw_sensor_snapshots_without_fusion_localization_dependencies() -> None:
    project_root = Path(__file__).resolve().parents[2]
    recorder = (project_root / "ros2_ws/src/data_recorder/src/data_recorder_node.cpp").read_text(
        encoding="utf-8"
    )
    page = (project_root / "frontend/app/page.tsx").read_text(encoding="utf-8")
    exporter = (project_root / "backend/exports/service.py").read_text(encoding="utf-8")

    assert "bind_nullable_double(statement, 36, std::nullopt);" in recorder
    assert "bind_nullable_double(statement, 37, std::nullopt);" in recorder
    assert "bind_nullable_double(statement, 38, std::nullopt);" in recorder
    assert "latest_localization_heading_" not in recorder
    assert "localization_status_subscription_" not in recorder
    assert "localization_odometry_subscription_" not in recorder
    assert "deriveLocalizationStatus" not in page
    assert "sample.vehicle_pitch_deg" in exporter
    assert "q2att(" not in recorder
    assert not (project_root / "ros2_ws/src/localization/src/dead_reckoning_node.cpp").exists()

    odometry_handler = re.search(
        r"void on_odometry\(.*?\n  void on_radar_temperature", recorder, re.DOTALL
    )
    assert odometry_handler is not None
    assert "latest_odin_.position_x_m = position.x" in odometry_handler.group(0)
    assert "latest_odin_.qx = orientation.x" in odometry_handler.group(0)
    assert "vehicleAttitudeFromOdinQuaternion" not in odometry_handler.group(0)

    for relative_directory in ("ros2_ws/src/clearance_engine",):
        directory = project_root / relative_directory
        source_text = "".join(
            path.read_text(encoding="utf-8", errors="ignore")
            for path in directory.rglob("*")
            if path.is_file()
        )
        assert "vehicle_attitude_mount_rotation_bm" not in source_text
