import sqlite3
from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app


PDF_FONT = Path("/usr/share/fonts/truetype/arphic-gbsn00lp/gbsn00lp.ttf")


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text(
        "<!doctype html><html lang='zh-CN'><body>Capture System</body></html>",
        encoding="utf-8",
    )


def create_measurement_database(
    path: Path,
    task_id: str,
    *,
    data_origin: str = "recorded",
    complete: int = 1,
    lane: str = "left",
) -> None:
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
                1, 1, ?, ?, ?,
                '2026-08-06T01:00:00Z', '2026-08-06T01:00:00.060Z',
                ?, 50.0, 'clearance-1.0', 'field-config-1', '0.2.0'
            )
            """,
            (task_id, data_origin, lane, complete),
        )
        base_ns = 1_785_978_000_000_000_000
        connection.executemany(
            "INSERT INTO clearance_samples VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [
                (0, base_ns, base_ns + 1_000_000, 0.0, 2.90, 5.20, 1, None, 0.95),
                (1, base_ns + 20_000_000, base_ns + 21_000_000, 20.0, None, None, 0, "insufficient_points", None),
                (2, base_ns + 40_000_000, base_ns + 41_000_000, 40.0, 2.89, 5.18, 1, None, 0.94),
                (3, base_ns + 60_000_000, base_ns + 61_000_000, 60.0, 2.91, 5.21, 1, None, 0.96),
            ],
        )
        connection.executemany(
            "INSERT INTO rtk_endpoints VALUES (?, ?, ?, ?, ?, ?, ?)",
            [
                ("entry", base_ns, 39.9, 116.39, 48.2, "RTK_FIXED", 1),
                ("exit", base_ns + 60_000_000, 39.9001, 116.3902, 48.4, "RTK_FIXED", 1),
            ],
        )


def attach_measurements(data_root: Path, task: dict[str, object], *, data_origin: str = "recorded") -> None:
    relative_path = f"{task['task_id']}/measurements.db"
    create_measurement_database(
        data_root / "tasks" / relative_path,
        str(task["task_id"]),
        data_origin=data_origin,
    )
    with sqlite3.connect(data_root / "capture.db") as connection:
        connection.execute(
            """
            UPDATE tasks
            SET status = 'completed', has_measurements = 1,
                recording_path = ?, started_at = '2026-08-06T01:00:00Z',
                completed_at = '2026-08-06T01:00:00.060Z'
            WHERE task_id = ?
            """,
            (relative_path, task["task_id"]),
        )


def test_preview_and_txt_export_use_only_recorded_completed_task(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "G45-001", "tunnel_name": "正式记录测试隧道"},
        ).json()
        attach_measurements(data_root, task)

        preview_response = client.get("/api/v1/reports/clearance-summary/preview")
        generation_response = client.post(f"/api/v1/tasks/{task['task_id']}/exports/txt")
        download_response = client.get(
            generation_response.json()["download_url"]
        )

    assert preview_response.status_code == 200
    preview = preview_response.json()
    assert preview["exportable_task_count"] == 1
    assert preview["tasks"][0]["exportable"] is True
    assert preview["tasks"][0]["minimum_height_m"] == 5.18
    assert generation_response.status_code == 200
    assert generation_response.json()["export_format"] == "txt"
    assert download_response.status_code == 200
    text = download_response.content.decode("utf-8-sig")
    assert "隧道净空检测 50 Hz 测量明细" in text
    assert "记录时间" in text
    assert "G45-001" in text
    assert "5.180" in text
    assert "insufficient_points" in text
    assert "39.9000000, 116.3900000" in text
    assert "attachment" in download_response.headers["content-disposition"]


def test_test_fixture_is_visible_but_blocked_from_formal_export(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "TEST-001", "tunnel_name": "界面测试隧道"},
        ).json()
        attach_measurements(data_root, task, data_origin="test_fixture")
        preview_response = client.get("/api/v1/reports/clearance-summary/preview")
        generation_response = client.post(f"/api/v1/tasks/{task['task_id']}/exports/txt")
        pdf_response = client.post("/api/v1/reports/clearance-summary")

    preview = preview_response.json()
    assert preview["tasks"][0]["data_origin"] == "test_fixture"
    assert preview["tasks"][0]["exportable"] is False
    assert preview["tasks"][0]["blocked_reason"] == "界面测试数据不能用于正式导出"
    assert generation_response.status_code == 409
    assert pdf_response.status_code == 409


def test_pdf_summary_generation_and_download(tmp_path: Path) -> None:
    assert PDF_FONT.is_file(), "测试环境缺少 PDF 中文字体"
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(
            static_dir,
            data_root=data_root,
            pdf_font_path=PDF_FONT,
            start_ros_bridge=False,
        )
    ) as client:
        for index in range(2):
            task = client.post(
                "/api/v1/tasks",
                json={"tunnel_code": f"G45-{index + 1:03d}", "tunnel_name": f"汇总测试隧道{index + 1}"},
            ).json()
            attach_measurements(data_root, task)
        generation_response = client.post("/api/v1/reports/clearance-summary")
        download_response = client.get(generation_response.json()["download_url"])

    assert generation_response.status_code == 200
    payload = generation_response.json()
    assert payload["export_format"] == "pdf"
    assert payload["included_task_count"] == 2
    assert payload["report_id"]
    assert download_response.status_code == 200
    assert download_response.content.startswith(b"%PDF-")
    assert len(download_response.content) > 1000
    assert download_response.headers["content-type"] == "application/pdf"
