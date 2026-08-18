import sqlite3
import threading
from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app
from backend.measurements.repository import MeasurementRepository


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
            CREATE INDEX clearance_samples_timestamp_idx
            ON clearance_samples(source_timestamp_ns);
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


def test_measurement_summary_and_adaptive_series_preserve_extrema_and_gaps(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
        client.post("/api/v1/batches")
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "TEST-003", "tunnel_name": "长曲线测试隧道"},
        ).json()
        relative_path = f"{task['task_id']}/measurements.db"
        database_path = data_root / "tasks" / relative_path
        create_measurement_database(database_path, task["task_id"])

        base_ns = 1_785_978_000_000_000_000
        rows = []
        for index in range(1000):
            valid = index != 700
            height = 3.0 if index == 523 else 5.2 + (index % 11) * 0.001
            rows.append(
                (
                    index,
                    base_ns + index * 20_000_000,
                    base_ns + index * 20_000_000 + 1_000_000,
                    float(index * 20),
                    2.9 if valid else None,
                    height if valid else None,
                    1 if valid else 0,
                    None if valid else "insufficient_points",
                    0.95 if valid else None,
                )
            )
        with sqlite3.connect(database_path) as connection:
            connection.execute("DELETE FROM clearance_samples")
            connection.executemany(
                "INSERT INTO clearance_samples VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                rows,
            )
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                """
                UPDATE tasks
                SET status = 'completed', has_measurements = 1,
                    recording_path = ?, started_at = '2026-08-06T01:00:00Z',
                    completed_at = '2026-08-06T01:00:20Z'
                WHERE task_id = ?
                """,
                (relative_path, task["task_id"]),
            )

        summary_response = client.get(f"/api/v1/tasks/{task['task_id']}/measurements/summary")
        series_response = client.get(
            f"/api/v1/tasks/{task['task_id']}/measurements/series?max_points=200"
        )
        prefix_response = client.get(
            f"/api/v1/tasks/{task['task_id']}/measurements/series-prefix?max_samples=200"
        )
        detail_start_ms = (base_ns + 500 * 20_000_000) // 1_000_000
        detail_end_ms = (base_ns + 550 * 20_000_000) // 1_000_000
        detail_response = client.get(
            f"/api/v1/tasks/{task['task_id']}/measurements/series"
            f"?start_timestamp_ms={detail_start_ms}&end_timestamp_ms={detail_end_ms}&max_points=200"
        )

    assert summary_response.status_code == 200
    summary = summary_response.json()
    assert "samples" not in summary
    assert summary["statistics"]["total_samples"] == 1000
    assert summary["first_sample_index"] == 0
    assert summary["last_sample_index"] == 999
    assert summary["first_timestamp_ms"] == base_ns // 1_000_000
    assert summary["last_timestamp_ms"] == (base_ns + 999 * 20_000_000) // 1_000_000

    assert series_response.status_code == 200
    series = series_response.json()
    assert series["domain_start_timestamp_ms"] == base_ns // 1_000_000
    assert series["domain_end_timestamp_ms"] == (base_ns + 999 * 20_000_000) // 1_000_000
    assert series["source_sample_count"] == 1000
    assert series["returned_sample_count"] <= 200
    assert series["downsampled"] is True
    assert any(sample["sample_index"] == 523 and sample["height_m"] == 3.0 for sample in series["samples"])
    assert any(sample["height_m"] == 5.21 for sample in series["samples"])
    assert any(sample["sample_index"] == 700 and sample["valid"] is False for sample in series["samples"])
    assert series["samples"][0]["sample_index"] == 0
    assert series["samples"][-1]["sample_index"] == 999

    assert prefix_response.status_code == 200
    prefix = prefix_response.json()
    assert prefix["source_sample_count"] == 200
    assert prefix["returned_sample_count"] == 200
    assert prefix["downsampled"] is False
    assert prefix["samples"][0]["sample_index"] == 0
    assert prefix["samples"][-1]["sample_index"] == 199
    assert prefix["requested_start_timestamp_ms"] == base_ns // 1_000_000
    assert prefix["requested_end_timestamp_ms"] == (base_ns + 199 * 20_000_000) // 1_000_000

    assert detail_response.status_code == 200
    detail = detail_response.json()
    assert detail["source_sample_count"] == 51
    assert detail["returned_sample_count"] == 51
    assert detail["downsampled"] is False
    assert detail["samples"][0]["sample_index"] == 500
    assert detail["samples"][-1]["sample_index"] == 550


def test_measurement_series_window_uses_source_time_across_pause_gap(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
        client.post("/api/v1/batches")
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "TEST-004", "tunnel_name": "暂停时间轴测试隧道"},
        ).json()
        relative_path = f"{task['task_id']}/measurements.db"
        database_path = data_root / "tasks" / relative_path
        create_measurement_database(database_path, task["task_id"])

        base_ns = 1_785_978_000_000_000_000
        timestamps = [
            base_ns,
            base_ns + 20_000_000,
            base_ns + 10_000_000_000,
            base_ns + 10_020_000_000,
        ]
        with sqlite3.connect(database_path) as connection:
            connection.execute("DELETE FROM clearance_samples")
            connection.executemany(
                "INSERT INTO clearance_samples VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                [
                    (index, timestamp_ns, timestamp_ns + 1_000_000, float(index * 20),
                     2.9, 5.2 - index * 0.01, 1, None, 0.95)
                    for index, timestamp_ns in enumerate(timestamps)
                ],
            )
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                """
                UPDATE tasks
                SET status = 'completed', has_measurements = 1, recording_path = ?
                WHERE task_id = ?
                """,
                (relative_path, task["task_id"]),
            )

        start_ms = (base_ns + 9_000_000_000) // 1_000_000
        end_ms = (base_ns + 11_000_000_000) // 1_000_000
        response = client.get(
            f"/api/v1/tasks/{task['task_id']}/measurements/series"
            f"?start_timestamp_ms={start_ms}&end_timestamp_ms={end_ms}&max_points=200"
        )

    assert response.status_code == 200
    payload = response.json()
    assert payload["source_sample_count"] == 2
    assert [sample["sample_index"] for sample in payload["samples"]] == [2, 3]
    assert payload["requested_start_timestamp_ms"] == start_ms
    assert payload["requested_end_timestamp_ms"] == end_ms


def test_measurement_prefix_caps_long_task_at_two_thousand_and_uses_bounded_plan(
    tmp_path: Path,
) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    ) as client:
        client.post("/api/v1/batches")
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "TEST-005", "tunnel_name": "首段上限测试隧道"},
        ).json()
        relative_path = f"{task['task_id']}/measurements.db"
        database_path = data_root / "tasks" / relative_path
        create_measurement_database(database_path, task["task_id"])

        base_ns = 1_785_978_000_000_000_000
        rows = [
            (
                index,
                base_ns + index * 20_000_000,
                base_ns + index * 20_000_000 + 1_000_000,
                float(index * 20),
                2.9,
                5.2,
                1,
                None,
                0.95,
            )
            for index in range(2505)
        ]
        with sqlite3.connect(database_path) as connection:
            connection.execute("DELETE FROM clearance_samples")
            connection.executemany(
                "INSERT INTO clearance_samples VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                rows,
            )
            prefix_plan = connection.execute(
                """
                EXPLAIN QUERY PLAN
                SELECT sample_index, source_timestamp_ns, elapsed_ms,
                       clearance_height_m, valid, invalid_reason
                FROM clearance_samples
                ORDER BY sample_index ASC
                LIMIT 2000
                """
            ).fetchall()
            window_plan = connection.execute(
                """
                EXPLAIN QUERY PLAN
                SELECT sample_index, source_timestamp_ns
                FROM clearance_samples
                WHERE source_timestamp_ns BETWEEN ? AND ?
                ORDER BY source_timestamp_ns ASC, sample_index ASC
                """,
                (base_ns, base_ns + 1_000_000_000),
            ).fetchall()
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                """
                UPDATE tasks
                SET status = 'completed', has_measurements = 1, recording_path = ?
                WHERE task_id = ?
                """,
                (relative_path, task["task_id"]),
            )

        response = client.get(
            f"/api/v1/tasks/{task['task_id']}/measurements/series-prefix?max_samples=2000"
        )

    assert response.status_code == 200
    payload = response.json()
    assert payload["source_sample_count"] == 2000
    assert payload["returned_sample_count"] == 2000
    assert payload["samples"][0]["sample_index"] == 0
    assert payload["samples"][-1]["sample_index"] == 1999
    assert all("USE TEMP B-TREE" not in str(row[3]).upper() for row in prefix_plan)
    assert any("clearance_samples_timestamp_idx" in str(row[3]) for row in window_plan)
    assert all("USE TEMP B-TREE" not in str(row[3]).upper() for row in window_plan)


def test_new_playback_request_interrupts_stale_series_for_same_browser_session(
    tmp_path: Path,
    monkeypatch,
) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"
    app = create_app(static_dir, data_root=data_root, start_ros_bridge=False)
    slow_query_started = threading.Event()

    def slow_downsample(
        cls,
        connection: sqlite3.Connection,
        *,
        start_timestamp_ns: int,
        end_timestamp_ns: int,
        max_points: int,
    ):
        del cls, start_timestamp_ns, end_timestamp_ns, max_points
        slow_query_started.set()
        connection.execute(
            """
            WITH RECURSIVE counter(value) AS (
                VALUES(0)
                UNION ALL
                SELECT value + 1 FROM counter WHERE value < 100000000
            )
            SELECT SUM(value) FROM counter
            """
        ).fetchone()
        return []

    monkeypatch.setattr(
        MeasurementRepository,
        "_load_downsampled_series",
        classmethod(slow_downsample),
    )

    with TestClient(app) as client:
        client.post("/api/v1/batches")
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "TEST-006", "tunnel_name": "切换取消测试隧道"},
        ).json()
        relative_path = f"{task['task_id']}/measurements.db"
        database_path = data_root / "tasks" / relative_path
        create_measurement_database(database_path, task["task_id"])
        base_ns = 1_785_978_000_000_000_000
        with sqlite3.connect(database_path) as connection:
            connection.execute("DELETE FROM clearance_samples")
            connection.executemany(
                "INSERT INTO clearance_samples VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                [
                    (
                        index,
                        base_ns + index * 20_000_000,
                        base_ns + index * 20_000_000 + 1_000_000,
                        float(index * 20),
                        2.9,
                        5.2,
                        1,
                        None,
                        0.95,
                    )
                    for index in range(1000)
                ],
            )
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                "UPDATE tasks SET status = 'completed', has_measurements = 1, recording_path = ? WHERE task_id = ?",
                (relative_path, task["task_id"]),
            )

        responses: list = []

        def request_stale_series() -> None:
            responses.append(
                client.get(
                    f"/api/v1/tasks/{task['task_id']}/measurements/series?max_points=200",
                    headers={"X-Playback-Session": "same-browser"},
                )
            )

        worker = threading.Thread(target=request_stale_series)
        worker.start()
        assert slow_query_started.wait(timeout=2.0)
        prefix_response = client.get(
            f"/api/v1/tasks/{task['task_id']}/measurements/series-prefix?max_samples=200",
            headers={"X-Playback-Session": "same-browser"},
        )
        worker.join(timeout=2.0)

    assert not worker.is_alive()
    assert prefix_response.status_code == 200
    assert prefix_response.json()["returned_sample_count"] == 200
    assert len(responses) == 1
    assert responses[0].status_code == 499
