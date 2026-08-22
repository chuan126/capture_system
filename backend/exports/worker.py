from __future__ import annotations

import argparse
import os
import traceback
from pathlib import Path

from backend.exports.jobs import read_job_payload, utc_now_text, write_job_payload
from backend.exports.service import ReportExportService
from backend.measurements.repository import MeasurementRepository
from backend.tasks.repository import TaskRepository


def _update(job_file: Path, **changes: object) -> dict[str, object]:
    payload = read_job_payload(job_file)
    if payload.get("state") == "cancelled":
        raise RuntimeError("导出任务已取消")
    payload.update(changes, updated_at=utc_now_text())
    write_job_payload(job_file, payload)
    return payload


def run(data_root: Path, job_file: Path, pdf_font: Path | None) -> int:
    try:
        try:
            os.nice(10)
        except OSError:
            pass
        payload = _update(job_file, state="running", phase="读取任务与测量数据", progress=0.2)
        task_ids = payload.get("task_ids")
        if not isinstance(task_ids, list) or not all(isinstance(item, str) for item in task_ids):
            raise RuntimeError("导出任务的task_ids无效")
        task_repository = TaskRepository(data_root / "capture.db", data_root / "tasks")
        task_repository.initialize()
        measurement_repository = MeasurementRepository(data_root / "tasks")
        service = ReportExportService(
            data_root,
            task_repository,
            measurement_repository,
            pdf_font_path=pdf_font,
        )
        export_format = str(payload.get("export_format"))
        _update(job_file, phase="生成导出文件", progress=0.65)
        if export_format == "txt":
            generated = service.generate_txt(task_repository.get_task(task_ids[0]))
        elif export_format == "pdf":
            generated = service.generate_pdf(task_ids)
        else:
            raise RuntimeError(f"不支持的导出格式：{export_format}")
        _update(
            job_file,
            state="completed",
            phase="导出完成",
            progress=1.0,
            error=None,
            file_name=generated.path.name,
            file_size_bytes=generated.path.stat().st_size,
            generated_at=generated.generated_at,
            download_path=str(generated.path.resolve()),
            report_id=generated.report_id,
            task_id=generated.task_id,
            included_task_count=generated.included_task_count,
        )
        return 0
    except Exception as error:
        try:
            _update(
                job_file,
                state="failed",
                phase="导出失败",
                error=str(error),
            )
        except Exception:
            pass
        traceback.print_exc()
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture System异步导出工作进程")
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--job-file", type=Path, required=True)
    parser.add_argument("--pdf-font", type=Path)
    args = parser.parse_args()
    return run(args.data_root.resolve(), args.job_file.resolve(), args.pdf_font)


if __name__ == "__main__":
    raise SystemExit(main())
