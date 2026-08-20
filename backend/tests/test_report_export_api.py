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
            json={
                "tunnel_code": "G45-001",
                "tunnel_name": "正式记录测试隧道",
                "clearance_threshold_m": 5.19,
                "clearance_upper_limit_m": 5.20,
            },
        ).json()
        attach_measurements(data_root, task)

        preview_response = client.post(
            "/api/v1/reports/clearance-summary/preview",
            json={"task_ids": [task["task_id"]]},
        )
        generation_response = client.post(f"/api/v1/tasks/{task['task_id']}/exports/txt")
        download_response = client.get(
            generation_response.json()["download_url"]
        )

    assert preview_response.status_code == 200
    preview = preview_response.json()
    assert preview["exportable_task_count"] == 1
    assert preview["tasks"][0]["exportable"] is True
    assert preview["tasks"][0]["pdf_exportable"] is True
    assert preview["tasks"][0]["minimum_height_m"] == 5.18
    assert preview["tasks"][0]["normal_minimum_height_m"] == 5.20
    assert preview["tasks"][0]["clearance_threshold_m"] == 5.19
    assert preview["tasks"][0]["clearance_upper_limit_m"] == 5.20
    assert generation_response.status_code == 200
    assert generation_response.json()["export_format"] == "txt"
    assert generation_response.json()["batch_id"] is None
    assert generation_response.json()["batch_code"] is None
    assert generation_response.json()["file_name"].startswith(f"{task['display_id']}_")
    assert download_response.status_code == 200
    text = download_response.content.decode("utf-8-sig")
    assert "隧道净空检测 50 Hz 测量明细" in text
    assert "记录时间" in text
    assert "G45-001" in text
    assert "5.180" in text
    assert "39.9000000, 116.3900000" in text
    header = text.splitlines()[2].split("\t")
    assert len(header) == 24
    assert "陀螺X rad/s" in header
    assert "加速度计Z m/s2" in header
    assert "俯仰 deg" in header
    assert "里程计位置z m" in header
    first_sample = dict(zip(header, text.splitlines()[3].split("\t"), strict=True))
    assert first_sample["雷达温度 °C"] == "0"
    assert first_sample["方位 deg"] == "0"
    assert first_sample["里程计位置z m"] == "0"
    assert "insufficient_points" not in text
    assert "attachment" in download_response.headers["content-disposition"]


def test_pdf_excludes_task_when_no_valid_sample_is_inside_frozen_height_range(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        task = client.post(
            "/api/v1/tasks",
            json={
                "tunnel_code": "G45-002",
                "tunnel_name": "无正常区间样本测试隧道",
                "clearance_threshold_m": 5.22,
                "clearance_upper_limit_m": 5.30,
            },
        ).json()
        attach_measurements(data_root, task)
        preview_response = client.post(
            "/api/v1/reports/clearance-summary/preview",
            json={"task_ids": [task["task_id"]]},
        )
        txt_response = client.post(f"/api/v1/tasks/{task['task_id']}/exports/txt")
        pdf_response = client.post(
            "/api/v1/reports/clearance-summary",
            json={"task_ids": [task["task_id"]]},
        )

    preview = preview_response.json()
    assert preview["exportable_task_count"] == 0
    assert preview["tasks"][0]["exportable"] is True
    assert preview["tasks"][0]["pdf_exportable"] is False
    assert preview["tasks"][0]["normal_minimum_height_m"] is None
    assert preview["tasks"][0]["pdf_blocked_reason"] == "测量区间内没有正常高度样本"
    assert txt_response.status_code == 200
    assert pdf_response.status_code == 409


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
        preview_response = client.post(
            "/api/v1/reports/clearance-summary/preview",
            json={"task_ids": [task["task_id"]]},
        )
        generation_response = client.post(f"/api/v1/tasks/{task['task_id']}/exports/txt")
        pdf_response = client.post(
            "/api/v1/reports/clearance-summary",
            json={"task_ids": [task["task_id"]]},
        )

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
        selected_tasks = []
        for index in range(2):
            task = client.post(
                "/api/v1/tasks",
                json={"tunnel_code": f"G45-{index + 1:03d}", "tunnel_name": f"汇总测试隧道{index + 1}"},
            ).json()
            attach_measurements(data_root, task)
            selected_tasks.append(task)

        unselected_task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "G45-999", "tunnel_name": "未选择隧道"},
        ).json()
        attach_measurements(data_root, unselected_task)

        generation_response = client.post(
            "/api/v1/reports/clearance-summary",
            json={"task_ids": [task["task_id"] for task in selected_tasks]},
        )
        download_response = client.get(generation_response.json()["download_url"])

    assert generation_response.status_code == 200
    payload = generation_response.json()
    assert payload["export_format"] == "pdf"
    assert payload["included_task_count"] == 2
    assert payload["batch_id"] is None
    assert payload["batch_code"] is None
    assert payload["file_name"].endswith("_隧道净空检测汇总报告.pdf")
    assert payload["report_id"]
    assert download_response.status_code == 200
    assert download_response.content.startswith(b"%PDF-")
    assert len(download_response.content) > 1000
    assert download_response.headers["content-type"] == "application/pdf"


def test_report_exports_accept_nanosecond_iso_timestamps(tmp_path: Path) -> None:
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
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "NS-001", "tunnel_name": "纳秒时间戳测试隧道"},
        ).json()
        attach_measurements(data_root, task)
        measurement_path = data_root / "tasks" / str(task["task_id"]) / "measurements.db"
        with sqlite3.connect(measurement_path) as connection:
            connection.execute(
                """
                UPDATE recording_metadata
                SET started_at = ?, ended_at = ?
                WHERE id = 1
                """,
                (
                    "2026-08-08T08:16:54.074757037+00:00",
                    "2026-08-08T08:16:54.134757037+00:00",
                ),
            )

        txt_response = client.post(f"/api/v1/tasks/{task['task_id']}/exports/txt")
        txt_download = client.get(txt_response.json()["download_url"])
        pdf_response = client.post(
            "/api/v1/reports/clearance-summary",
            json={"task_ids": [task["task_id"]]},
        )

    assert txt_response.status_code == 200
    assert txt_download.status_code == 200
    txt = txt_download.content.decode("utf-8-sig")
    assert "2026-08-08T16:16:54.074+08:00" in txt
    assert "2026-08-08T16:16:54.134+08:00" in txt
    assert pdf_response.status_code == 200


def test_report_preview_and_pdf_use_task_creation_order(tmp_path: Path) -> None:
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
        created = []
        for index in range(3):
            task = client.post(
                "/api/v1/tasks",
                json={"tunnel_code": f"ORDER-{index + 1}", "tunnel_name": f"顺序测试{index + 1}"},
            ).json()
            attach_measurements(data_root, task)
            created.append(task)

        reversed_ids = [task["task_id"] for task in reversed(created)]
        preview_response = client.post(
            "/api/v1/reports/clearance-summary/preview",
            json={"task_ids": reversed_ids},
        )
        pdf_response = client.post(
            "/api/v1/reports/clearance-summary",
            json={"task_ids": reversed_ids},
        )

    assert preview_response.status_code == 200
    preview_ids = [item["task_id"] for item in preview_response.json()["tasks"]]
    assert preview_ids == [task["task_id"] for task in created]
    assert pdf_response.status_code == 200
    report_id = pdf_response.json()["report_id"]
    manifest = __import__("json").loads(
        (data_root / "reports" / report_id / "manifest.json").read_text(encoding="utf-8")
    )
    assert manifest["task_ids"] == [task["task_id"] for task in created]
    assert manifest["task_display_ids"] == [task["display_id"] for task in created]


def test_report_lane_text_uses_actual_direction_and_lane_side() -> None:
    from backend.exports.service import _lane_text

    assert _lane_text("left", "up", "left") == "上行左车道"
    assert _lane_text("right", "up", "right") == "上行右车道"
    assert _lane_text("left", "down", "left") == "下行左车道"
    assert _lane_text("right", "down", "right") == "下行右车道"
    assert _lane_text("left", "unknown", "left") == "左车道"
