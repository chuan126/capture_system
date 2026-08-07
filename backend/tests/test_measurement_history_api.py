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


def create_measurement_database(path: Path, task_id: str) -> None:
    path.parent.mkdir(parents=True)
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
                software_version TEXT
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
                quality_score REAL
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
                1, 1, ?, 'test_fixture', 'left',
                '2026-08-06T01:00:00Z', '2026-08-06T01:00:00.080Z',
                1, 50.0, 'test-algorithm', 'test-config', '0.2.0-test'
            )
            """,
            (task_id,),
        )
        base_ns = 1_785_978_000_000_000_000
        rows = [
            (0, base_ns, base_ns + 1_000_000, 0.0, 2.90, 5.20, 1, None, 0.95),
            (1, base_ns + 20_000_000, base_ns + 21_000_000, 20.0, 2.91, 5.21, 1, None, 0.96),
            (2, base_ns + 40_000_000, base_ns + 41_000_000, 40.0, None, None, 0, "insufficient_points", None),
            (3, base_ns + 60_000_000, base_ns + 61_000_000, 60.0, 2.89, 5.19, 1, None, 0.94),
            (4, base_ns + 80_000_000, base_ns + 81_000_000, 80.0, 2.88, 5.18, 1, None, 0.93),
        ]
        connection.executemany(
            "INSERT INTO clearance_samples VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            rows,
        )
        connection.executemany(
            "INSERT INTO rtk_endpoints VALUES (?, ?, ?, ?, ?, ?, ?)",
            [
                ("entry", base_ns, 39.9, 116.39, 48.2, "RTK_FIXED", 1),
                ("exit", base_ns + 80_000_000, 39.9001, 116.3902, 48.4, "RTK_FIXED", 1),
            ],
        )
        connection.execute(
            "INSERT INTO pause_intervals(started_elapsed_ms, ended_elapsed_ms) VALUES (30.0, 35.0)"
        )


def test_measurement_history_returns_curve_gaps_statistics_and_endpoints(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
        batch = client.post("/api/v1/batches").json()
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "TEST-001", "tunnel_name": "曲线测试隧道"},
        ).json()
        relative_path = f"{task['task_id']}/measurements.db"
        create_measurement_database(data_root / "tasks" / relative_path, task["task_id"])
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                """
                UPDATE tasks
                SET status = 'completed', has_measurements = 1,
                    recording_path = ?, started_at = '2026-08-06T01:00:00Z',
                    completed_at = '2026-08-06T01:00:00.080Z'
                WHERE task_id = ?
                """,
                (relative_path, task["task_id"]),
            )
        response = client.get(f"/api/v1/tasks/{task['task_id']}/measurements")

    assert response.status_code == 200
    payload = response.json()
    assert payload["data_origin"] == "test_fixture"
    assert payload["lane"] == "left"
    assert payload["statistics"]["total_samples"] == 5
    assert payload["statistics"]["valid_samples"] == 4
    assert payload["statistics"]["invalid_samples"] == 1
    assert payload["statistics"]["minimum_height_m"] == 5.18
    assert payload["statistics"]["maximum_height_m"] == 5.21
    assert payload["samples"][2]["valid"] is False
    assert payload["samples"][2]["height_m"] is None
    assert payload["samples"][2]["invalid_reason"] == "insufficient_points"
    assert payload["entry_rtk"]["fix_type"] == "RTK_FIXED"
    assert payload["exit_rtk"]["longitude_deg"] == 116.3902
    assert len(payload["pause_intervals"]) == 1


def test_measurement_history_returns_404_without_record(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(
        create_app(static_dir, data_root=tmp_path / "runtime", start_ros_bridge=False)
    ) as client:
        batch = client.post("/api/v1/batches").json()
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "TEST-002", "tunnel_name": "空任务"},
        ).json()
        response = client.get(f"/api/v1/tasks/{task['task_id']}/measurements")

    assert response.status_code == 404
    assert response.json()["detail"] == "任务尚无测量记录"
