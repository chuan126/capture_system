#!/usr/bin/env python3
"""生成隔离的前端回放测试数据，不用于现场测量或报告。"""

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

TASK_PENDING = "00000000-0000-4000-8000-000000000001"
TASK_COMPLETED = "00000000-0000-4000-8000-000000000002"
TASK_INTERRUPTED = "00000000-0000-4000-8000-000000000003"


def iso_utc(epoch_ns: int) -> str:
    return datetime.fromtimestamp(epoch_ns / 1_000_000_000, tz=timezone.utc).isoformat().replace("+00:00", "Z")


def create_recording_schema(connection: sqlite3.Connection) -> None:
    connection.executescript(
        """
        CREATE TABLE recording_metadata (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            schema_version INTEGER NOT NULL CHECK (schema_version > 0),
            task_id TEXT NOT NULL,
            data_origin TEXT NOT NULL CHECK (data_origin IN ('recorded', 'test_fixture')),
            lane TEXT NOT NULL CHECK (lane IN ('left', 'right', 'unknown')),
            started_at TEXT NOT NULL,
            ended_at TEXT,
            complete INTEGER NOT NULL CHECK (complete IN (0, 1)),
            nominal_sample_rate_hz REAL NOT NULL CHECK (nominal_sample_rate_hz > 0),
            algorithm_version TEXT,
            config_version TEXT,
            software_version TEXT
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
            quality_score REAL
        );
        CREATE INDEX clearance_samples_timestamp_idx ON clearance_samples(source_timestamp_ns);
        CREATE TABLE rtk_endpoints (
            role TEXT PRIMARY KEY CHECK (role IN ('entry', 'exit')),
            timestamp_ns INTEGER NOT NULL,
            latitude_deg REAL NOT NULL,
            longitude_deg REAL NOT NULL,
            altitude_m REAL,
            fix_type TEXT NOT NULL,
            valid INTEGER NOT NULL CHECK (valid IN (0, 1))
        );
        CREATE TABLE pause_intervals (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            started_elapsed_ms REAL NOT NULL,
            ended_elapsed_ms REAL NOT NULL,
            CHECK (ended_elapsed_ms >= started_elapsed_ms)
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
                id, schema_version, task_id, data_origin, lane, started_at,
                ended_at, complete, nominal_sample_rate_hz, algorithm_version,
                config_version, software_version
            ) VALUES (1, 1, ?, 'test_fixture', ?, ?, ?, ?, 50.0, ?, ?, ?)
            """,
            (
                task_id,
                lane,
                iso_utc(start_ns),
                iso_utc(end_ns) if complete else None,
                1 if complete else 0,
                "fixture-clearance-1.0",
                "fixture-small-board-1cm",
                "0.2.0-test-fixture",
            ),
        )

        rows = []
        for index in range(sample_count):
            elapsed_ms = index * 20.0
            timestamp_ns = start_ns + index * 20_000_000
            invalid = any(start <= index < end for start, end in invalid_ranges)
            if invalid:
                rows.append(
                    (
                        index,
                        timestamp_ns,
                        timestamp_ns + 1_500_000,
                        elapsed_ms,
                        None,
                        None,
                        0,
                        "test_invalid_segment",
                        None,
                    )
                )
                continue

            seconds = index / 50.0
            broad_shape = 5.22 + 0.055 * math.sin(seconds * 0.72) + 0.025 * math.sin(seconds * 2.1)
            local_dip = 0.24 * math.exp(-((seconds - 12.0) ** 2) / 5.2)
            height = broad_shape - local_dip
            lidar_to_top = height - 2.30
            quality = 0.91 + 0.05 * (0.5 + 0.5 * math.sin(seconds * 0.33))
            rows.append(
                (
                    index,
                    timestamp_ns,
                    timestamp_ns + 1_500_000,
                    elapsed_ms,
                    round(lidar_to_top, 6),
                    round(height, 6),
                    1,
                    None,
                    round(quality, 4),
                )
            )

        connection.executemany(
            """
            INSERT INTO clearance_samples (
                sample_index, source_timestamp_ns, recorded_timestamp_ns,
                elapsed_ms, lidar_to_top_m, clearance_height_m,
                valid, invalid_reason, quality_score
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            rows,
        )
        connection.execute(
            """
            INSERT INTO rtk_endpoints (
                role, timestamp_ns, latitude_deg, longitude_deg,
                altitude_m, fix_type, valid
            ) VALUES ('entry', ?, 39.9000000, 116.3900000, 48.2, 'RTK_FIXED', 1)
            """,
            (start_ns,),
        )
        if include_exit_rtk:
            connection.execute(
                """
                INSERT INTO rtk_endpoints (
                    role, timestamp_ns, latitude_deg, longitude_deg,
                    altitude_m, fix_type, valid
                ) VALUES ('exit', ?, 39.9001800, 116.3903600, 48.6, 'RTK_FIXED', 1)
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
        connection.executemany(
            """
            INSERT INTO tasks (
                task_id, sequence, tunnel_code, tunnel_name, status,
                created_at, updated_at, started_at, completed_at,
                has_measurements, recording_path, schema_version,
                deleted_at, delete_reason
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 2, NULL, NULL)
            """,
            [
                (
                    TASK_PENDING,
                    1,
                    "TEST-TUNNEL-001",
                    "待执行界面测试隧道",
                    "pending",
                    "2026-08-06T00:50:00Z",
                    "2026-08-06T00:50:00Z",
                    None,
                    None,
                    0,
                    None,
                ),
                (
                    TASK_COMPLETED,
                    2,
                    "TEST-TUNNEL-002",
                    "完整曲线界面测试隧道",
                    "completed",
                    "2026-08-06T00:55:00Z",
                    iso_utc(start_completed_ns + 29_980_000_000),
                    iso_utc(start_completed_ns),
                    iso_utc(start_completed_ns + 29_980_000_000),
                    1,
                    completed_path,
                ),
                (
                    TASK_INTERRUPTED,
                    3,
                    "TEST-TUNNEL-003",
                    "中断曲线界面测试隧道",
                    "interrupted",
                    "2026-08-06T01:55:00Z",
                    iso_utc(start_interrupted_ns + 11_980_000_000),
                    iso_utc(start_interrupted_ns),
                    None,
                    1,
                    interrupted_path,
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

    with sqlite3.connect(output / "capture.db") as connection:
        assert connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    for task_id in (TASK_COMPLETED, TASK_INTERRUPTED):
        with sqlite3.connect(output / "tasks" / task_id / "measurements.db") as connection:
            assert connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

    print(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, help="测试数据根目录")
    parser.add_argument("--force", action="store_true", help="覆盖已有输出目录")
    args = parser.parse_args()
    generate(args.output.resolve(), force=args.force)


if __name__ == "__main__":
    main()
