from __future__ import annotations

from datetime import datetime, timezone

from fastapi import APIRouter, HTTPException, Request, status
from fastapi.responses import FileResponse

from backend.exports.models import (
    ExportFileResponse,
    ReportPreviewResponse,
    ReportSelectionRequest,
    TaskExportPreviewResponse,
)
from backend.exports.service import (
    ExportBlockedError,
    ExportNotFoundError,
    ExportStorageError,
    GeneratedExport,
    ReportExportService,
    TaskExportAssessment,
)
from backend.measurements.models import RtkEndpointResponse
from backend.measurements.repository import RtkEndpointRecord
from backend.tasks.repository import TaskNotFoundError, TaskRepository, TaskStorageError

router = APIRouter(prefix="/api/v1", tags=["exports"])


def _task_repository(request: Request) -> TaskRepository:
    return request.app.state.task_repository


def _service(request: Request) -> ReportExportService:
    return request.app.state.report_export_service


def _rtk_response(record: RtkEndpointRecord | None) -> RtkEndpointResponse | None:
    if record is None:
        return None
    return RtkEndpointResponse(
        timestamp_ms=record.timestamp_ms,
        latitude_deg=record.latitude_deg,
        longitude_deg=record.longitude_deg,
        altitude_m=record.altitude_m,
        fix_type=record.fix_type,
        valid=record.valid,
    )


def _preview_response(assessment: TaskExportAssessment) -> TaskExportPreviewResponse:
    summary = assessment.summary
    return TaskExportPreviewResponse(
        task_id=assessment.task.task_id,
        display_id=assessment.task.display_id,
        tunnel_code=assessment.task.tunnel_code,
        tunnel_name=assessment.task.tunnel_name,
        status=assessment.task.status,
        exportable=assessment.exportable,
        blocked_reason=assessment.blocked_reason,
        pdf_exportable=assessment.pdf_exportable,
        pdf_blocked_reason=assessment.pdf_blocked_reason,
        data_origin=summary.data_origin if summary else None,
        lane=summary.lane if summary else None,
        travel_direction=summary.travel_direction if summary else None,
        lane_side=summary.lane_side if summary else None,
        started_at=summary.started_at if summary else assessment.task.started_at,
        ended_at=summary.ended_at if summary else assessment.task.completed_at,
        complete=summary.complete if summary else None,
        total_samples=summary.statistics.total_samples if summary else None,
        valid_samples=summary.statistics.valid_samples if summary else None,
        invalid_samples=summary.statistics.invalid_samples if summary else None,
        minimum_height_m=summary.statistics.minimum_height_m if summary else None,
        normal_minimum_height_m=(
            assessment.normal_height_statistics.minimum_height_m
            if assessment.normal_height_statistics else None
        ),
        clearance_threshold_m=(
            assessment.normal_height_statistics.clearance_threshold_m
            if assessment.normal_height_statistics else None
        ),
        clearance_upper_limit_m=(
            assessment.normal_height_statistics.clearance_upper_limit_m
            if assessment.normal_height_statistics else None
        ),
        entry_rtk=_rtk_response(summary.entry_rtk) if summary else None,
        exit_rtk=_rtk_response(summary.exit_rtk) if summary else None,
    )


def _file_response(record: GeneratedExport, download_url: str) -> ExportFileResponse:
    generated_at = datetime.fromisoformat(record.generated_at.replace("Z", "+00:00"))
    return ExportFileResponse(
        export_format=record.export_format,
        file_name=record.path.name,
        file_size_bytes=record.path.stat().st_size,
        generated_at=generated_at,
        download_url=download_url,
        report_id=record.report_id,
        task_id=record.task_id,
        included_task_count=record.included_task_count,
        batch_id=None,
        batch_code=None,
    )


def _export_http_error(error: Exception) -> HTTPException:
    if isinstance(error, (ExportNotFoundError, TaskNotFoundError)):
        return HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail=str(error))
    if isinstance(error, ExportBlockedError):
        return HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error))
    return HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error))


@router.post("/reports/clearance-summary/preview", response_model=ReportPreviewResponse)
def report_preview(payload: ReportSelectionRequest, request: Request) -> ReportPreviewResponse:
    try:
        assessments = _service(request).preview_tasks(payload.task_ids)
    except (TaskNotFoundError, TaskStorageError, ExportBlockedError, ExportStorageError) as error:
        raise _export_http_error(error) from error
    return ReportPreviewResponse(
        task_count=len(assessments),
        exportable_task_count=sum(1 for item in assessments if item.pdf_exportable),
        generated_at=datetime.now(timezone.utc),
        tasks=[_preview_response(item) for item in assessments],
    )


@router.post("/tasks/{task_id}/exports/txt", response_model=ExportFileResponse)
def generate_task_txt(task_id: str, request: Request) -> ExportFileResponse:
    try:
        task = _task_repository(request).get_task(task_id)
        generated = _service(request).generate_txt(task)
        return _file_response(generated, f"/api/v1/tasks/{task_id}/exports/txt/download")
    except TaskNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务不存在") from error
    except (TaskStorageError, ExportBlockedError, ExportStorageError) as error:
        raise _export_http_error(error) from error


@router.get("/tasks/{task_id}/exports/txt/download")
def download_task_txt(task_id: str, request: Request) -> FileResponse:
    try:
        task = _task_repository(request).get_task(task_id)
        path = _service(request).resolve_txt_download(task)
        return FileResponse(path, media_type="text/plain; charset=utf-8", filename=path.name)
    except TaskNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务不存在") from error
    except (TaskStorageError, ExportNotFoundError, ExportStorageError) as error:
        raise _export_http_error(error) from error


@router.post("/reports/clearance-summary", response_model=ExportFileResponse)
def generate_clearance_summary_pdf(payload: ReportSelectionRequest, request: Request) -> ExportFileResponse:
    try:
        generated = _service(request).generate_pdf(payload.task_ids)
        return _file_response(generated, f"/api/v1/reports/{generated.report_id}/download")
    except (TaskNotFoundError, TaskStorageError, ExportBlockedError, ExportStorageError) as error:
        raise _export_http_error(error) from error


@router.get("/reports/{report_id}/download")
def download_clearance_summary_pdf(report_id: str, request: Request) -> FileResponse:
    try:
        path = _service(request).resolve_pdf_download(report_id)
        return FileResponse(path, media_type="application/pdf", filename=path.name)
    except (ExportNotFoundError, ExportStorageError) as error:
        raise _export_http_error(error) from error
