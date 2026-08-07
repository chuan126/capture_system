import sqlite3
from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text(
        "<!doctype html><html lang='zh-CN'><body>Capture System</body></html>",
        encoding="utf-8",
    )


def create_v2_recording(path: Path, task_id: str) -> None:
    path.parent.mkdir(parents=True)
    base_source_ns = 1_785_978_000_000_000_000
    base_recorded_ns = base_source_ns + 5_000_000
    with sqlite3.connect(path) as connection:
        connection.executescript(
            """
            CREATE TABLE recording_metadata (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                schema_version INTEGER NOT NULL,
                task_id TEXT NOT NULL,
                data_origin TEXT NOT NULL,
                lane TEXT NOT NULL,
                started_at TEXT NOT NULL,
                ended_at TEXT,
                complete INTEGER NOT NULL,
                nominal_sample_rate_hz REAL NOT NULL,
                algorithm_version TEXT,
                config_version TEXT,
                software_version TEXT,
                lidar_mount_height_m REAL,
                clearance_threshold_m REAL,
                entry_rtk_status TEXT NOT NULL,
                exit_rtk_status TEXT NOT NULL
            );
            CREATE TABLE clearance_samples (
                sample_index INTEGER PRIMARY KEY,
                source_timestamp_ns INTEGER NOT NULL,
                recorded_timestamp_ns INTEGER NOT NULL,
                elapsed_ms REAL NOT NULL,
                lidar_to_top_m REAL,
                clearance_height_m REAL,
                valid INTEGER NOT NULL,
                invalid_reason TEXT,
                quality_score REAL,
                source_sequence INTEGER NOT NULL,
                source_age_ms REAL NOT NULL,
                is_repeated INTEGER NOT NULL,
                repeat_index INTEGER NOT NULL
            );
            CREATE TABLE rtk_endpoints (
                role TEXT PRIMARY KEY,
                timestamp_ns INTEGER NOT NULL,
                latitude_deg REAL NOT NULL,
                longitude_deg REAL NOT NULL,
                altitude_m REAL,
                fix_type TEXT NOT NULL,
                valid INTEGER NOT NULL
            );
            CREATE TABLE pause_intervals (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                started_elapsed_ms REAL NOT NULL,
                ended_elapsed_ms REAL NOT NULL
            );
            """
        )
        connection.execute(
            """
            INSERT INTO recording_metadata VALUES (
                1, 2, ?, 'recorded', 'right',
                '2026-08-06T01:00:00Z', '2026-08-06T01:00:00.060Z',
                1, 50.0, 'clearance-current', 'field-config', '0.2.0',
                1.86, 4.50, 'unconfirmed', 'unconfirmed'
            )
            """,
            (task_id,),
        )
        connection.executemany(
            "INSERT INTO clearance_samples VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [
                (
                    0,
                    base_source_ns,
                    base_recorded_ns,
                    0.0,
                    2.90,
                    2.90,
                    1,
                    None,
                    0.95,
                    10,
                    5.0,
                    0,
                    0,
                ),
                (
                    1,
                    base_source_ns,
                    base_recorded_ns + 20_000_000,
                    20.0,
                    2.90,
                    2.90,
                    1,
                    None,
                    0.95,
                    10,
                    25.0,
                    1,
                    1,
                ),
                (
                    2,
                    base_source_ns,
                    base_recorded_ns + 40_000_000,
                    40.0,
                    None,
                    None,
                    0,
                    "source_timeout",
                    0.95,
                    10,
                    260.0,
                    1,
                    2,
                ),
                (
                    3,
                    base_source_ns + 55_000_000,
                    base_recorded_ns + 60_000_000,
                    60.0,
                    2.88,
                    2.88,
                    1,
                    None,
                    0.97,
                    11,
                    5.0,
                    0,
                    0,
                ),
            ],
        )


def test_schema_v2_history_and_txt_preserve_repeated_source_provenance(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        batch = client.post("/api/v1/batches").json()
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "V2-001", "tunnel_name": "50Hz来源追溯测试"},
        ).json()
        relative_path = f"{task['task_id']}/measurements.db"
        create_v2_recording(data_root / "tasks" / relative_path, task["task_id"])
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                """
                UPDATE tasks
                SET status='completed', operation_phase='completed', has_measurements=1,
                    recording_path=?, started_at='2026-08-06T01:00:00Z',
                    completed_at='2026-08-06T01:00:00.060Z'
                WHERE task_id=?
                """,
                (relative_path, task["task_id"]),
            )

        history_response = client.get(f"/api/v1/tasks/{task['task_id']}/measurements")
        export_response = client.post(f"/api/v1/tasks/{task['task_id']}/exports/txt")
        download_response = client.get(export_response.json()["download_url"])

    assert history_response.status_code == 200
    history = history_response.json()
    assert history["recording_schema_version"] == 2
    assert history["statistics"]["total_samples"] == 4
    assert history["statistics"]["valid_samples"] == 3
    assert history["statistics"]["actual_average_sample_rate_hz"] == 50.0
    assert history["samples"][2]["height_m"] is None
    assert history["samples"][2]["invalid_reason"] == "source_timeout"

    assert export_response.status_code == 200
    text = download_response.content.decode("utf-8-sig")
    assert "源帧序号" in text
    assert "源帧年龄 ms" in text
    assert "重复记录" in text
    assert "重复序号" in text
    assert "source_timeout" in text
    assert "\t10\t25.000\t是\t1" in text
