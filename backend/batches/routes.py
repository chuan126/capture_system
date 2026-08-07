from __future__ import annotations

from fastapi import APIRouter, HTTPException, Request, status

from backend.batches.models import BatchPurgeResponse, BatchResponse
from backend.tasks.repository import (
    BatchConflictError,
    BatchNotFoundError,
    BatchPurgeResult,
    BatchRecord,
    TaskRepository,
    TaskStorageError,
)

router = APIRouter(prefix="/api/v1/batches", tags=["batches"])


def _repository(request: Request) -> TaskRepository:
    return request.app.state.task_repository


def _response(record: BatchRecord) -> BatchResponse:
    return BatchResponse(
        batch_id=record.batch_id,
        batch_code=record.batch_code,
        operation_date=record.operation_date,
        daily_sequence=record.daily_sequence,
        status=record.status,
        created_at=record.created_at,
        started_at=record.started_at,
        completed_at=record.completed_at,
        archived_at=record.archived_at,
        purged_at=record.purged_at,
        task_count=record.task_count,
        visible_task_count=record.visible_task_count,
        measurement_bytes=record.measurement_bytes,
        report_id=record.report_id,
        report_path=record.report_path,
        report_sha256=record.report_sha256,
        report_generated_at=record.report_generated_at,
        purged_bytes=record.purged_bytes,
    )


def _storage_failure(error: TaskStorageError) -> HTTPException:
    return HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error))


def _conflict(error: BatchConflictError) -> HTTPException:
    return HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error))


@router.get("", response_model=list[BatchResponse])
def list_batches(request: Request) -> list[BatchResponse]:
    try:
        return [_response(record) for record in _repository(request).list_batches()]
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.get("/active", response_model=BatchResponse | None)
def get_active_batch(request: Request) -> BatchResponse | None:
    try:
        record = _repository(request).get_active_batch()
        return _response(record) if record is not None else None
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.post("", response_model=BatchResponse, status_code=status.HTTP_201_CREATED)
def create_batch(request: Request) -> BatchResponse:
    try:
        return _response(_repository(request).create_batch())
    except BatchConflictError as error:
        raise _conflict(error) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.post("/{batch_id}/complete", response_model=BatchResponse)
def complete_batch(batch_id: str, request: Request) -> BatchResponse:
    try:
        return _response(_repository(request).complete_batch(batch_id))
    except BatchNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="作业批次不存在") from error
    except BatchConflictError as error:
        raise _conflict(error) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.post("/{batch_id}/archive", response_model=BatchResponse)
def archive_batch(batch_id: str, request: Request) -> BatchResponse:
    try:
        return _response(_repository(request).archive_batch(batch_id))
    except BatchNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="作业批次不存在") from error
    except BatchConflictError as error:
        raise _conflict(error) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.post("/{batch_id}/purge", response_model=BatchPurgeResponse)
def purge_batch(batch_id: str, request: Request) -> BatchPurgeResponse:
    try:
        result: BatchPurgeResult = _repository(request).purge_batch(batch_id)
        return BatchPurgeResponse(
            batch=_response(result.batch),
            released_bytes=result.released_bytes,
            removed_task_count=result.removed_task_count,
        )
    except BatchNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="作业批次不存在") from error
    except BatchConflictError as error:
        raise _conflict(error) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error
