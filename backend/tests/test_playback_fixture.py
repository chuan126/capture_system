from pathlib import Path
import sqlite3

from backend.measurements.repository import MeasurementRepository
from backend.tasks.repository import TaskRepository
from tools.generate_playback_test_fixture import (
    TASK_COMPLETED,
    TASK_INTERRUPTED,
    TASK_PENDING,
    generate,
)


def test_playback_fixture_matches_current_task_and_measurement_schemas(tmp_path: Path) -> None:
    data_root = tmp_path / "fixture"
    generate(data_root, force=False)

    repository = TaskRepository(data_root / "capture.db", data_root / "tasks")
    tasks = repository.list_tasks(order="asc")
    assert [task.task_id for task in tasks] == [TASK_PENDING, TASK_COMPLETED, TASK_INTERRUPTED]
    assert [task.display_id for task in tasks] == [
        "20260806_085000",
        "20260806_085500",
        "20260806_095500",
    ]

    completed = next(task for task in tasks if task.task_id == TASK_COMPLETED)
    history = MeasurementRepository(data_root / "tasks").load_history(completed)
    assert history.recording_schema_version == 8
    assert history.data_origin == "test_fixture"
    assert history.travel_direction == "up"
    assert history.lane_side == "left"
    assert history.complete is True
    assert history.statistics.total_samples == 1500

    with sqlite3.connect(data_root / "tasks" / TASK_COMPLETED / "measurements.db") as connection:
        sample_columns = {row[1] for row in connection.execute("PRAGMA table_info(clearance_samples)")}
        localization_status_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(localization_status_samples)")
        }
        assert {"source_sequence", "source_age_ms", "is_repeated", "repeat_index"}.issubset(sample_columns)
        assert {
            "rtk_timestamp_ns", "gyro_x_rad_s", "accel_z_m_s2", "imu_sample_count",
            "radar_temperature_c", "minimum_point_x_m", "vehicle_pitch_deg", "odin_position_x_m",
        }.issubset(sample_columns)
        assert {
            "mode", "heading_source", "vehicle_attitude_valid", "scale_calibration_mode",
            "scale_status", "scale_fit_residual_m", "heading_alignment_reason",
            "distance_from_anchor_m", "dr_duration_s",
        }.issubset(
            localization_status_columns
        )
        assert connection.execute("SELECT COUNT(*) FROM clearance_source_frames").fetchone()[0] == 1500
        source_columns = {row[1] for row in connection.execute("PRAGMA table_info(clearance_source_frames)")}
        assert {
            "candidate_region_count",
            "selected_grid_area_m2",
            "selected_residual_median_m",
            "selected_residual_p95_m",
        }.issubset(source_columns)
        assert {"candidate_plane_count", "selected_rms_m"}.isdisjoint(source_columns)
        source_quality = connection.execute(
            "SELECT candidate_region_count, selected_grid_area_m2, "
            "selected_residual_median_m, selected_residual_p95_m "
            "FROM clearance_source_frames WHERE valid=1 LIMIT 1"
        ).fetchone()
        assert source_quality == (2, 1.2, 0.012, 0.028)
        raw_height, recorded_height = connection.execute(
            "SELECT lidar_to_top_m, clearance_height_m FROM clearance_samples WHERE valid=1 LIMIT 1"
        ).fetchone()
        assert round(recorded_height - raw_height, 6) == 2.30
        telemetry = connection.execute(
            "SELECT imu_sample_count, radar_temperature_c, vehicle_pitch_deg "
            "FROM clearance_samples WHERE valid=1 LIMIT 1"
        ).fetchone()
        assert telemetry == (8, 42.5, 1.5)
