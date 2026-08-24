from __future__ import annotations

import sqlite3
from pathlib import Path

from backend.measurements.repository import MeasurementRepository
from backend.exports.service import ReportExportService, _format_txt_number
from backend.tasks.models import TaskCreateRequest
from backend.tasks.repository import TaskRepository
from backend.tests.test_report_export_api import PDF_FONT, create_measurement_database


def test_schema_v13_txt_source_uses_one_row_per_received_imu_sample(tmp_path: Path) -> None:
    data_root = tmp_path / "runtime"
    task_repository = TaskRepository(data_root / "capture.db", data_root / "tasks")
    task_repository.initialize()
    task = task_repository.create_tasks(
        [TaskCreateRequest(tunnel_code="IMU-001", tunnel_name="IMU原始导出测试")]
    )[0]
    relative_path = f"{task.task_id}/measurements.db"
    database_path = data_root / "tasks" / relative_path
    create_measurement_database(database_path, task.task_id)
    with sqlite3.connect(database_path) as connection:
        connection.execute("UPDATE recording_metadata SET schema_version=13 WHERE id=1")
    with sqlite3.connect(data_root / "capture.db") as connection:
        connection.execute(
            """
            UPDATE tasks
            SET status='completed', operation_phase='completed', has_measurements=1,
                recording_path=?, started_at='2026-08-06T01:00:00Z',
                completed_at='2026-08-06T01:00:00.060Z'
            WHERE task_id=?
            """,
            (relative_path, task.task_id),
        )
    completed_task = task_repository.get_task(task.task_id)
    samples = list(
        MeasurementRepository(data_root / "tasks").iter_imu_export_samples(completed_task)
    )

    assert len(samples) == 2
    assert [sample.sample_index for sample in samples] == [0, 1]
    assert [sample.height_m for sample in samples] == [5.20, 5.18]
    assert [sample.minimum_height_m for sample in samples] == [5.20, 5.18]
    assert samples[0].gyro_x_rad_s == 0.01
    assert samples[0].rtk_timestamp_ms is None
    assert samples[0].vehicle_heading_deg is None

    export_service = ReportExportService(
        data_root,
        task_repository,
        MeasurementRepository(data_root / "tasks"),
        pdf_font_path=PDF_FONT,
    )
    generated = export_service.generate_txt(completed_task)
    text = generated.path.read_text(encoding="utf-8-sig")
    header = text.splitlines()[2].split("    ")
    rows = [line.split("    ") for line in text.splitlines()[3:]]
    assert generated.path.name.endswith("_原始数据保存.txt")
    assert len(header) == 38
    assert len(rows) == 2
    assert all(len(row) == 38 for row in rows)
    assert rows[0][12:18] == ["0", "0", "0", "0", "0", "0"]
    pdf = export_service.generate_pdf([completed_task.task_id])
    assert pdf.path.read_bytes().startswith(b"%PDF-")


def test_txt_non_finite_sensor_value_uses_zero_placeholder() -> None:
    assert _format_txt_number(float("nan"), 6) == "0"
    assert _format_txt_number(float("inf"), 6) == "0"
