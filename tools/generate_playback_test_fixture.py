#!/usr/bin/env python3
"""生成隔离的前端回放测试数据，不用于现场测量或正式报告。"""

from __future__ import annotations

import argparse
import math
import shutil
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from backend.tasks.repository import TaskRepository

COMPAT_BATCH_ID = "00000000-0000-4000-8000-00000000ba01"
TASK_PENDING = "00000000-0000-4000-8000-000000000001"
TASK_COMPLETED = "00000000-0000-4000-8000-000000000002"
TASK_INTERRUPTED = "00000000-0000-4000-8000-000000000003"


def iso_utc(epoch_ns: int) -> str:
    return (
        datetime.fromtimestamp(epoch_ns / 1_000_000_000, tz=timezone.utc)
        .isoformat()
        .replace("+00:00", "Z")
    )


def create_recording_schema(connection: sqlite3.Connection) -> None:
    """建立与 data_recorder 当前 schema v9 对齐的测试数据库。"""
    connection.executescript(
        """
        CREATE TABLE recording_metadata (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            schema_version INTEGER NOT NULL CHECK (schema_version > 0),
            task_id TEXT NOT NULL,
            data_origin TEXT NOT NULL CHECK (data_origin IN ('recorded', 'test_fixture')),
            lane TEXT NOT NULL CHECK (lane IN ('left', 'right', 'unknown')),
            travel_direction TEXT NOT NULL CHECK (travel_direction IN ('up', 'down', 'unknown')),
            lane_side TEXT NOT NULL CHECK (lane_side IN ('left', 'right', 'unknown')),
            started_at TEXT NOT NULL,
            ended_at TEXT,
            complete INTEGER NOT NULL CHECK (complete IN (0, 1)),
            nominal_sample_rate_hz REAL NOT NULL CHECK (nominal_sample_rate_hz > 0),
            algorithm_version TEXT,
            config_version TEXT,
            software_version TEXT,
            lidar_mount_height_m REAL,
            clearance_threshold_m REAL,
            entry_rtk_status TEXT NOT NULL DEFAULT 'pending',
            exit_rtk_status TEXT NOT NULL DEFAULT 'not_requested'
        );
        CREATE TABLE clearance_samples (
            sample_index INTEGER PRIMARY KEY CHECK (sample_index >= 0),
            source_timestamp_ns INTEGER NOT NULL,
            recorded_timestamp_ns INTEGER NOT NULL,
            elapsed_ms REAL NOT NULL CHECK (elapsed_ms >= 0),
            lidar_to_top_m REAL,
            clearance_height_m REAL,
            valid INTEGER NOT NULL CHECK (valid IN (0, 1)),
            invalid_reason TEXT,
            quality_score REAL,
            source_sequence INTEGER NOT NULL DEFAULT 0,
            source_age_ms REAL NOT NULL DEFAULT 0,
            is_repeated INTEGER NOT NULL DEFAULT 0 CHECK (is_repeated IN (0, 1)),
            repeat_index INTEGER NOT NULL DEFAULT 0,
            rtk_timestamp_ns INTEGER,
            gyro_x_rad_s REAL,
            gyro_y_rad_s REAL,
            gyro_z_rad_s REAL,
            accel_x_m_s2 REAL,
            accel_y_m_s2 REAL,
            accel_z_m_s2 REAL,
            imu_sample_count INTEGER NOT NULL DEFAULT 0,
            radar_temperature_c REAL,
            minimum_point_x_m REAL,
            minimum_point_y_m REAL,
            minimum_point_z_m REAL,
            vehicle_pitch_deg REAL,
            vehicle_roll_deg REAL,
            vehicle_heading_deg REAL,
            odin_position_x_m REAL,
            odin_position_y_m REAL,
            odin_position_z_m REAL
        );
        CREATE INDEX clearance_samples_timestamp_idx ON clearance_samples(source_timestamp_ns);
        CREATE TABLE clearance_source_frames (
            source_sequence INTEGER PRIMARY KEY,
            source_timestamp_ns INTEGER NOT NULL,
            received_timestamp_ns INTEGER NOT NULL,
            valid INTEGER NOT NULL CHECK (valid IN (0, 1)),
            lidar_to_top_m REAL,
            invalid_reason TEXT,
            quality_score REAL,
            candidate_region_count INTEGER,
            selected_inlier_count INTEGER,
            selected_grid_area_m2 REAL,
            selected_tilt_deg REAL,
            selected_residual_median_m REAL,
            selected_residual_p95_m REAL,
            processing_time_ms REAL
        );
        CREATE TABLE rtk_samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_ns INTEGER NOT NULL,
            latitude_deg REAL,
            longitude_deg REAL,
            altitude_m REAL,
            fix_type TEXT NOT NULL,
            valid INTEGER NOT NULL CHECK (valid IN (0, 1))
        );
        CREATE TABLE localization_fix_samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_ns INTEGER NOT NULL,
            latitude_deg REAL,
            longitude_deg REAL,
            altitude_m REAL,
            fix_status INTEGER NOT NULL,
            valid INTEGER NOT NULL CHECK (valid IN (0, 1))
        );
        CREATE TABLE localization_status_samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_ns INTEGER NOT NULL,
            valid INTEGER NOT NULL CHECK (valid IN (0, 1)),
            mode INTEGER NOT NULL,
            heading_source INTEGER NOT NULL,
            latitude_deg REAL NOT NULL,
            longitude_deg REAL NOT NULL,
            altitude_m REAL NOT NULL,
            heading_deg REAL NOT NULL,
            vehicle_attitude_valid INTEGER NOT NULL CHECK (vehicle_attitude_valid IN (0, 1)),
            vehicle_pitch_deg REAL NOT NULL,
            vehicle_roll_deg REAL NOT NULL,
            vehicle_heading_deg REAL NOT NULL,
            heading_alignment_valid INTEGER NOT NULL CHECK (heading_alignment_valid IN (0, 1)),
            delta_yaw_deg REAL NOT NULL,
            scale_calibration_mode INTEGER NOT NULL CHECK (scale_calibration_mode IN (0, 1)),
            scale_status INTEGER NOT NULL,
            scale_valid INTEGER NOT NULL CHECK (scale_valid IN (0, 1)),
            horizontal_scale REAL NOT NULL,
            vertical_scale REAL NOT NULL,
            scale_baseline_m REAL NOT NULL,
            scale_fit_residual_m REAL NOT NULL,
            heading_baseline_m REAL NOT NULL,
            heading_alignment_reason TEXT NOT NULL,
            distance_from_anchor_m REAL NOT NULL,
            dr_duration_s REAL NOT NULL,
            rtk_age_s REAL NOT NULL,
            odometry_age_s REAL NOT NULL,
            imu_age_s REAL NOT NULL,
            position_difference_to_rtk_m REAL NOT NULL,
            invalid_reason TEXT NOT NULL
        );
        CREATE TABLE localization_odometry_samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_ns INTEGER NOT NULL,
            frame_id TEXT NOT NULL,
            child_frame_id TEXT NOT NULL,
            east_m REAL NOT NULL,
            north_m REAL NOT NULL,
            up_m REAL NOT NULL,
            qx REAL NOT NULL,
            qy REAL NOT NULL,
            qz REAL NOT NULL,
            qw REAL NOT NULL
        );
        CREATE TABLE rtk_endpoints (
            role TEXT PRIMARY KEY CHECK (role IN ('entry', 'exit')),
            timestamp_ns INTEGER NOT NULL,
            latitude_deg REAL NOT NULL,
            longitude_deg REAL NOT NULL,
            altitude_m REAL,
            fix_type TEXT NOT NULL,
            valid INTEGER NOT NULL CHECK (valid IN (0, 1))
        );
        CREATE TABLE event_rtk_snapshots (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            event_type TEXT NOT NULL,
            requested_timestamp_ns INTEGER NOT NULL,
            coordinate_timestamp_ns INTEGER,
            latitude_deg REAL,
            longitude_deg REAL,
            altitude_m REAL,
            fix_type TEXT,
            valid INTEGER NOT NULL CHECK (valid IN (0, 1))
        );
        CREATE TABLE pause_intervals (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            started_elapsed_ms REAL NOT NULL,
            ended_elapsed_ms REAL NOT NULL,
            CHECK (ended_elapsed_ms >= started_elapsed_ms)
        );
        CREATE TABLE task_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            event_type TEXT NOT NULL,
            occurred_at_ns INTEGER NOT NULL,
            message TEXT,
            error_code TEXT
        );
        CREATE TABLE recording_counters (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            total_samples INTEGER NOT NULL,
            valid_samples INTEGER NOT NULL,
            invalid_samples INTEGER NOT NULL,
            source_frames INTEGER NOT NULL,
            write_errors INTEGER NOT NULL
        );
        """
    )


def create_recording(
    path: Path,
    *,
    task_id: str,
    start_ns: int,
    sample_count: int,
    complete: bool,
    lane: str,
    invalid_ranges: list[tuple[int, int]],
    include_exit_rtk: bool,
    pause_interval: tuple[float, float] | None,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(path) as connection:
        create_recording_schema(connection)
        end_ns = start_ns + (sample_count - 1) * 20_000_000
        connection.execute(
            """
            INSERT INTO recording_metadata (
                id, schema_version, task_id, data_origin, lane, travel_direction, lane_side, started_at,
                ended_at, complete, nominal_sample_rate_hz, algorithm_version,
                config_version, software_version, lidar_mount_height_m,
                clearance_threshold_m, entry_rtk_status, exit_rtk_status
            ) VALUES (1, 9, ?, 'test_fixture', ?, 'up', ?, ?, ?, ?, 50.0, ?, ?, ?, ?, ?, 'confirmed', ?)
            """,
            (
                task_id,
                lane,
                lane,
                iso_utc(start_ns),
                iso_utc(end_ns) if complete else None,
                1 if complete else 0,
                "fixture-clearance-2.0",
                "clearance_engine_small_board_1cm.yaml-test-fixture",
                "0.2.0-test-fixture",
                2.30,
                4.50,
                "confirmed" if include_exit_rtk else "unconfirmed",
            ),
        )

        sample_rows = []
        source_rows = []
        valid_samples = 0
        for index in range(sample_count):
            elapsed_ms = index * 20.0
            source_timestamp_ns = start_ns + index * 20_000_000
            recorded_timestamp_ns = source_timestamp_ns + 1_500_000
            source_sequence = index + 1
            telemetry = (
                source_timestamp_ns,
                0.001 + index * 0.000001,
                0.002,
                0.003,
                0.10,
                0.20,
                9.81,
                8,
                42.5,
                0.25,
                -0.15,
                2.92,
                1.5,
                -0.2,
                45.0,
                index * 0.02,
                index * 0.01,
                0.0,
            )
            invalid = any(start <= index < end for start, end in invalid_ranges)
            if invalid:
                sample_rows.append(
                    (
                        index,
                        source_timestamp_ns,
                        recorded_timestamp_ns,
                        elapsed_ms,
                        None,
                        None,
                        0,
                        "test_invalid_segment",
                        None,
                        source_sequence,
                        1.5,
                        0,
                        0,
                        *telemetry,
                    )
                )
                source_rows.append(
                    (
                        source_sequence,
                        source_timestamp_ns,
                        recorded_timestamp_ns,
                        0,
                        None,
                        "test_invalid_segment",
                        None,
                        0,
                        0,
                        None,
                        None,
                        None,
                        None,
                        2.0,
                    )
                )
                continue

            seconds = index / 50.0
            lidar_to_top = (
                5.22
                + 0.055 * math.sin(seconds * 0.72)
                + 0.025 * math.sin(seconds * 2.1)
                - 0.24 * math.exp(-((seconds - 12.0) ** 2) / 5.2)
            )
            quality = 0.91 + 0.05 * (0.5 + 0.5 * math.sin(seconds * 0.33))
            lidar_to_top = round(lidar_to_top, 6)
            quality = round(quality, 4)
            valid_samples += 1
            sample_rows.append(
                (
                    index,
                    source_timestamp_ns,
                    recorded_timestamp_ns,
                    elapsed_ms,
                    lidar_to_top,
                    round(lidar_to_top + 2.30, 6),
                    1,
                    None,
                    quality,
                    source_sequence,
                    1.5,
                    0,
                    0,
                    *telemetry,
                )
            )
            source_rows.append(
                (
                    source_sequence,
                    source_timestamp_ns,
                    recorded_timestamp_ns,
                    1,
                    lidar_to_top,
                    None,
                    quality,
                    2,
                    240,
                    1.2,
                    1.0,
                    0.012,
                    0.028,
                    2.0,
                )
            )

        connection.executemany(
            """
            INSERT INTO clearance_samples (
                sample_index, source_timestamp_ns, recorded_timestamp_ns,
                elapsed_ms, lidar_to_top_m, clearance_height_m,
                valid, invalid_reason, quality_score, source_sequence,
                source_age_ms, is_repeated, repeat_index, rtk_timestamp_ns,
                gyro_x_rad_s, gyro_y_rad_s, gyro_z_rad_s,
                accel_x_m_s2, accel_y_m_s2, accel_z_m_s2, imu_sample_count,
                radar_temperature_c, minimum_point_x_m, minimum_point_y_m,
                minimum_point_z_m, vehicle_pitch_deg, vehicle_roll_deg, vehicle_heading_deg,
                odin_position_x_m, odin_position_y_m, odin_position_z_m
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                      ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            sample_rows,
        )
        connection.executemany(
            """
            INSERT INTO clearance_source_frames (
                source_sequence, source_timestamp_ns, received_timestamp_ns,
                valid, lidar_to_top_m, invalid_reason, quality_score,
                candidate_region_count, selected_inlier_count, selected_grid_area_m2,
                selected_tilt_deg, selected_residual_median_m, selected_residual_p95_m,
                processing_time_ms
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            source_rows,
        )

        entry = (start_ns, 39.9000000, 116.3900000, 48.2, "RTK_FIXED", 1)
        connection.execute(
            """
            INSERT INTO rtk_endpoints (
                role, timestamp_ns, latitude_deg, longitude_deg,
                altitude_m, fix_type, valid
            ) VALUES ('entry', ?, ?, ?, ?, ?, ?)
            """,
            entry,
        )
        connection.execute(
            """
            INSERT INTO rtk_samples (
                timestamp_ns, latitude_deg, longitude_deg, altitude_m, fix_type, valid
            ) VALUES (?, ?, ?, ?, ?, ?)
            """,
            entry,
        )
        connection.execute(
            """
            INSERT INTO event_rtk_snapshots (
                event_type, requested_timestamp_ns, coordinate_timestamp_ns,
                latitude_deg, longitude_deg, altitude_m, fix_type, valid
            ) VALUES ('entry', ?, ?, ?, ?, ?, ?, 1)
            """,
            (start_ns, *entry[:-1]),
        )

        if include_exit_rtk:
            exit_fix = (end_ns, 39.9001800, 116.3903600, 48.6, "RTK_FIXED", 1)
            connection.execute(
                """
                INSERT INTO rtk_endpoints (
                    role, timestamp_ns, latitude_deg, longitude_deg,
                    altitude_m, fix_type, valid
                ) VALUES ('exit', ?, ?, ?, ?, ?, ?)
                """,
                exit_fix,
            )
            connection.execute(
                """
                INSERT INTO rtk_samples (
                    timestamp_ns, latitude_deg, longitude_deg, altitude_m, fix_type, valid
                ) VALUES (?, ?, ?, ?, ?, ?)
                """,
                exit_fix,
            )
            connection.execute(
                """
                INSERT INTO event_rtk_snapshots (
                    event_type, requested_timestamp_ns, coordinate_timestamp_ns,
                    latitude_deg, longitude_deg, altitude_m, fix_type, valid
                ) VALUES ('exit', ?, ?, ?, ?, ?, ?, 1)
                """,
                (end_ns, *exit_fix[:-1]),
            )
        else:
            connection.execute(
                """
                INSERT INTO event_rtk_snapshots (
                    event_type, requested_timestamp_ns, coordinate_timestamp_ns,
                    latitude_deg, longitude_deg, altitude_m, fix_type, valid
                ) VALUES ('exit', ?, NULL, NULL, NULL, NULL, NULL, 0)
                """,
                (end_ns,),
            )

        if pause_interval is not None:
            connection.execute(
                """
                INSERT INTO pause_intervals(started_elapsed_ms, ended_elapsed_ms)
                VALUES (?, ?)
                """,
                pause_interval,
            )
        connection.execute(
            """
            INSERT INTO recording_counters (
                id, total_samples, valid_samples, invalid_samples, source_frames, write_errors
            ) VALUES (1, ?, ?, ?, ?, 0)
            """,
            (sample_count, valid_samples, sample_count - valid_samples, sample_count),
        )


def generate(output: Path, *, force: bool) -> None:
    if output.exists():
        if not force:
            raise SystemExit(f"输出目录已存在：{output}，使用 --force 覆盖")
        shutil.rmtree(output)
    output.mkdir(parents=True)

    repository = TaskRepository(output / "capture.db", output / "tasks")
    repository.initialize()

    completed_path = f"{TASK_COMPLETED}/measurements.db"
    interrupted_path = f"{TASK_INTERRUPTED}/measurements.db"
    start_completed_ns = 1_785_978_000_000_000_000
    start_interrupted_ns = 1_785_981_600_000_000_000

    with sqlite3.connect(output / "capture.db") as connection:
        schema_version = int(
            connection.execute("SELECT MAX(version) FROM schema_migrations").fetchone()[0]
        )
        connection.execute(
            """
            INSERT INTO operation_batches (
                batch_id, batch_code, operation_date, daily_sequence, status, active_slot,
                created_at, updated_at, started_at, completed_at, task_count
            ) VALUES (?, 'FIXTURE-COMPAT-01', '2026-08-06', 1, 'completed', NULL,
                      '2026-08-06T00:50:00Z', '2026-08-06T02:07:00Z',
                      '2026-08-06T00:50:00Z', '2026-08-06T02:07:00Z', 3)
            """,
            (COMPAT_BATCH_ID,),
        )
        connection.executemany(
            """
            INSERT INTO tasks (
                task_id, sequence, batch_id, batch_sequence, display_id,
                tunnel_code, tunnel_name, status, operation_phase,
                created_at, updated_at, started_at, completed_at,
                entry_rtk_status, exit_rtk_status, has_measurements,
                recording_path, schema_version,
                planned_travel_direction, planned_lane_side, planned_clearance_threshold_m,
                deleted_at, delete_reason
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, NULL)
            """,
            [
                (
                    TASK_PENDING,
                    1,
                    COMPAT_BATCH_ID,
                    1,
                    "20260806_085000",
                    "TEST-TUNNEL-001",
                    "待执行界面测试隧道",
                    "pending",
                    "idle",
                    "2026-08-06T00:50:00Z",
                    "2026-08-06T00:50:00Z",
                    None,
                    None,
                    "not_requested",
                    "not_requested",
                    0,
                    None,
                    schema_version,
                    "up",
                    "left",
                    4.5,
                ),
                (
                    TASK_COMPLETED,
                    2,
                    COMPAT_BATCH_ID,
                    2,
                    "20260806_085500",
                    "TEST-TUNNEL-002",
                    "完整曲线界面测试隧道",
                    "completed",
                    "completed",
                    "2026-08-06T00:55:00Z",
                    iso_utc(start_completed_ns + 29_980_000_000),
                    iso_utc(start_completed_ns),
                    iso_utc(start_completed_ns + 29_980_000_000),
                    "confirmed",
                    "confirmed",
                    1,
                    completed_path,
                    schema_version,
                    "up",
                    "left",
                    4.5,
                ),
                (
                    TASK_INTERRUPTED,
                    3,
                    COMPAT_BATCH_ID,
                    3,
                    "20260806_095500",
                    "TEST-TUNNEL-003",
                    "中断曲线界面测试隧道",
                    "interrupted",
                    "interrupted",
                    "2026-08-06T01:55:00Z",
                    iso_utc(start_interrupted_ns + 11_980_000_000),
                    iso_utc(start_interrupted_ns),
                    None,
                    "confirmed",
                    "unconfirmed",
                    1,
                    interrupted_path,
                    schema_version,
                    "down",
                    "right",
                    4.2,
                ),
            ],
        )

    create_recording(
        output / "tasks" / completed_path,
        task_id=TASK_COMPLETED,
        start_ns=start_completed_ns,
        sample_count=1_500,
        complete=True,
        lane="left",
        invalid_ranges=[(210, 245), (680, 710), (1_160, 1_175)],
        include_exit_rtk=True,
        pause_interval=(18_000.0, 19_200.0),
    )
    create_recording(
        output / "tasks" / interrupted_path,
        task_id=TASK_INTERRUPTED,
        start_ns=start_interrupted_ns,
        sample_count=600,
        complete=False,
        lane="right",
        invalid_ranges=[(120, 155), (430, 470)],
        include_exit_rtk=False,
        pause_interval=None,
    )

    tasks = repository.list_tasks(order="asc")
    assert [task.task_id for task in tasks] == [TASK_PENDING, TASK_COMPLETED, TASK_INTERRUPTED]
    assert [task.display_id for task in tasks] == [
        "20260806_085000",
        "20260806_085500",
        "20260806_095500",
    ]

    with sqlite3.connect(output / "capture.db") as connection:
        assert connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    for task_id in (TASK_COMPLETED, TASK_INTERRUPTED):
        with sqlite3.connect(output / "tasks" / task_id / "measurements.db") as connection:
            assert connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
            assert connection.execute(
                "SELECT schema_version FROM recording_metadata WHERE id=1"
            ).fetchone()[0] == 9

    print(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, help="测试数据根目录")
    parser.add_argument("--force", action="store_true", help="覆盖已有输出目录")
    args = parser.parse_args()
    generate(args.output.resolve(), force=args.force)


if __name__ == "__main__":
    main()
