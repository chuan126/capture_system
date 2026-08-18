from __future__ import annotations

from typing import Annotated, Literal

from fastapi import APIRouter, Header, HTTPException, Query, Request, Response, status

from backend.measurements.models import (
    ClearanceHistorySampleResponse,
    ClearanceSeriesSampleResponse,
    MeasurementHistoryResponse,
    MeasurementSeriesResponse,
    MeasurementStatisticsResponse,
    MeasurementSummaryResponse,
    PauseIntervalResponse,
    RtkEndpointResponse,
)
from backend.measurements.repository import (
    MeasurementHistoryRecord,
    MeasurementSeriesRecord,
    MeasurementSummaryRecord,
    RtkEndpointRecord,
    MeasurementNotFoundError,
    MeasurementRepository,
    MeasurementStorageError,
)
from backend.measurements.query_coordinator import (
    MeasurementQueryCancelledError,
    MeasurementQueryCoordinator,
)
from backend.tasks.models import (
    TaskBatchCreateRequest,
    TaskCreateRequest,
    TaskDeleteManyRequest,
    TaskDeleteManyResponse,
    TaskPurgeDataRequest,
    TaskPurgeDataResponse,
    TaskResponse,
    TaskStatus,
)
from backend.tasks.repository import (
    BatchConflictError,
    TaskDeleteConflictError,
    TaskIdempotencyConflictError,
    TaskNotFoundError,
    TaskRecord,
    TaskRepository,
    TaskStorageError,
)

router = APIRouter(prefix="/api/v1/tasks", tags=["tasks"])


def _repository(request: Request) -> TaskRepository:
    return request.app.state.task_repository


def _measurement_repository(request: Request) -> MeasurementRepository:
    return request.app.state.measurement_repository


def _measurement_query_coordinator(request: Request) -> MeasurementQueryCoordinator:
    return request.app.state.measurement_query_coordinator


def _cancelled_measurement_query(error: MeasurementQueryCancelledError) -> HTTPException:
    return HTTPException(status_code=499, detail=str(error))


def _response(record: TaskRecord) -> TaskResponse:
    return TaskResponse(
        task_id=record.task_id,
        display_id=record.display_id,
        batch_id=record.batch_id,
        batch_code=record.batch_code,
        sequence=record.sequence,
        global_sequence=record.global_sequence,
        display_sequence=record.display_sequence,
        tunnel_code=record.tunnel_code,
        tunnel_name=record.tunnel_name,
        status=record.status,
        operation_phase=record.operation_phase,
        status_revision=record.status_revision,
        created_at=record.created_at,
        updated_at=record.updated_at,
        start_requested_at=record.start_requested_at,
        started_at=record.started_at,
        stop_requested_at=record.stop_requested_at,
        completed_at=record.completed_at,
        entry_rtk_status=record.entry_rtk_status,
        exit_rtk_status=record.exit_rtk_status,
        has_measurements=record.has_measurements,
        recording_path=record.recording_path,
        local_data_purged_at=record.local_data_purged_at,
        purged_bytes=record.purged_bytes,
        last_error_code=record.last_error_code,
        last_error_message=record.last_error_message,
        warning_code=record.warning_code,
        planned_travel_direction=record.planned_travel_direction,
        planned_lane_side=record.planned_lane_side,
        planned_clearance_threshold_m=record.planned_clearance_threshold_m,
        travel_direction=record.travel_direction,
        lane_side=record.lane_side,
        lane=record.lane,
        lidar_mount_height_m=record.lidar_mount_height_m,
        clearance_threshold_m=record.clearance_threshold_m,
        schema_version=record.schema_version,
        deleted_at=record.deleted_at,
        delete_reason=record.delete_reason,
    )


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


def _measurement_response(record: MeasurementHistoryRecord) -> MeasurementHistoryResponse:
    return MeasurementHistoryResponse(
        task_id=record.task_id,
        recording_schema_version=record.recording_schema_version,
        data_origin=record.data_origin,
        lane=record.lane,
        travel_direction=record.travel_direction,
        lane_side=record.lane_side,
        started_at=record.started_at,
        ended_at=record.ended_at,
        complete=record.complete,
        algorithm_version=record.algorithm_version,
        config_version=record.config_version,
        software_version=record.software_version,
        statistics=MeasurementStatisticsResponse(
            total_samples=record.statistics.total_samples,
            valid_samples=record.statistics.valid_samples,
            invalid_samples=record.statistics.invalid_samples,
            minimum_height_m=record.statistics.minimum_height_m,
            average_height_m=record.statistics.average_height_m,
            maximum_height_m=record.statistics.maximum_height_m,
            duration_ms=record.statistics.duration_ms,
            nominal_sample_rate_hz=record.statistics.nominal_sample_rate_hz,
            actual_average_sample_rate_hz=record.statistics.actual_average_sample_rate_hz,
        ),
        entry_rtk=_rtk_response(record.entry_rtk),
        exit_rtk=_rtk_response(record.exit_rtk),
        pause_intervals=[
            PauseIntervalResponse(
                started_elapsed_ms=interval.started_elapsed_ms,
                ended_elapsed_ms=interval.ended_elapsed_ms,
            )
            for interval in record.pause_intervals
        ],
        samples=[
            ClearanceHistorySampleResponse(
                sample_index=sample.sample_index,
                timestamp_ms=sample.timestamp_ms,
                elapsed_ms=sample.elapsed_ms,
                height_m=sample.height_m,
                lidar_to_top_m=sample.lidar_to_top_m,
                valid=sample.valid,
                invalid_reason=sample.invalid_reason,
                quality_score=sample.quality_score,
            )
            for sample in record.samples
        ],
    )


def _measurement_statistics_response(record: MeasurementSummaryRecord | MeasurementHistoryRecord) -> MeasurementStatisticsResponse:
    return MeasurementStatisticsResponse(
        total_samples=record.statistics.total_samples,
        valid_samples=record.statistics.valid_samples,
        invalid_samples=record.statistics.invalid_samples,
        minimum_height_m=record.statistics.minimum_height_m,
        average_height_m=record.statistics.average_height_m,
        maximum_height_m=record.statistics.maximum_height_m,
        duration_ms=record.statistics.duration_ms,
        nominal_sample_rate_hz=record.statistics.nominal_sample_rate_hz,
        actual_average_sample_rate_hz=record.statistics.actual_average_sample_rate_hz,
    )


def _measurement_summary_response(record: MeasurementSummaryRecord) -> MeasurementSummaryResponse:
    return MeasurementSummaryResponse(
        task_id=record.task_id,
        recording_schema_version=record.recording_schema_version,
        data_origin=record.data_origin,
        lane=record.lane,
        travel_direction=record.travel_direction,
        lane_side=record.lane_side,
        started_at=record.started_at,
        ended_at=record.ended_at,
        complete=record.complete,
        algorithm_version=record.algorithm_version,
        config_version=record.config_version,
        software_version=record.software_version,
        statistics=_measurement_statistics_response(record),
        entry_rtk=_rtk_response(record.entry_rtk),
        exit_rtk=_rtk_response(record.exit_rtk),
        pause_interval_count=record.pause_interval_count,
        first_sample_index=record.first_sample_index,
        last_sample_index=record.last_sample_index,
        first_timestamp_ms=record.first_timestamp_ms,
        last_timestamp_ms=record.last_timestamp_ms,
    )


def _measurement_series_response(record: MeasurementSeriesRecord) -> MeasurementSeriesResponse:
    return MeasurementSeriesResponse(
        task_id=record.task_id,
        domain_start_timestamp_ms=record.domain_start_timestamp_ms,
        domain_end_timestamp_ms=record.domain_end_timestamp_ms,
        requested_start_timestamp_ms=record.requested_start_timestamp_ms,
        requested_end_timestamp_ms=record.requested_end_timestamp_ms,
        source_sample_count=record.source_sample_count,
        returned_sample_count=len(record.samples),
        downsampled=record.downsampled,
        samples=[
            ClearanceSeriesSampleResponse(
                sample_index=sample.sample_index,
                timestamp_ms=sample.timestamp_ms,
                elapsed_ms=sample.elapsed_ms,
                height_m=sample.height_m,
                valid=sample.valid,
                invalid_reason=sample.invalid_reason,
            )
            for sample in record.samples
        ],
    )


def _storage_failure(error: TaskStorageError) -> HTTPException:
    return HTTPException(
        status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
        detail=str(error),
    )


@router.post("", response_model=TaskResponse, status_code=status.HTTP_201_CREATED)
def create_task(
    payload: TaskCreateRequest,
    request: Request,
    idempotency_key: Annotated[str | None, Header(alias="Idempotency-Key")] = None,
) -> TaskResponse:
    try:
        record = _repository(request).create_tasks(
            [payload],
            idempotency_key=idempotency_key,
        )[0]
        return _response(record)
    except (TaskIdempotencyConflictError, BatchConflictError) as error:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.post("/batch", response_model=list[TaskResponse], status_code=status.HTTP_201_CREATED)
def create_task_batch(
    payload: TaskBatchCreateRequest,
    request: Request,
    idempotency_key: Annotated[str | None, Header(alias="Idempotency-Key")] = None,
) -> list[TaskResponse]:
    try:
        records = _repository(request).create_tasks(
            payload.tasks,
            idempotency_key=idempotency_key,
        )
        return [_response(record) for record in records]
    except (TaskIdempotencyConflictError, BatchConflictError) as error:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.get("", response_model=list[TaskResponse])
def list_tasks(
    request: Request,
    task_status: Annotated[TaskStatus | None, Query(alias="status")] = None,
    has_measurements: bool | None = None,
    limit: Annotated[int, Query(ge=1, le=500)] = 100,
    offset: Annotated[int, Query(ge=0)] = 0,
    order: Literal["asc", "desc"] = "asc",
    batch_id: str | None = None,
) -> list[TaskResponse]:
    try:
        records = _repository(request).list_tasks(
            status=task_status,
            has_measurements=has_measurements,
            limit=limit,
            offset=offset,
            order=order,
            batch_id=batch_id,
        )
        return [_response(record) for record in records]
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.get("/{task_id}/measurements/summary", response_model=MeasurementSummaryResponse)
def get_task_measurement_summary(
    task_id: str,
    request: Request,
    playback_session: Annotated[
        str | None,
        Header(alias="X-Playback-Session", max_length=128),
    ] = None,
) -> MeasurementSummaryResponse:
    query = _measurement_query_coordinator(request).begin(playback_session)
    try:
        task = _repository(request).get_task(task_id)
        return _measurement_summary_response(
            _measurement_repository(request).load_summary(task, query=query)
        )
    except TaskNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务不存在") from error
    except MeasurementNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务尚无测量记录") from error
    except MeasurementStorageError as error:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail=str(error),
        ) from error
    except MeasurementQueryCancelledError as error:
        raise _cancelled_measurement_query(error) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error
    finally:
        if query is not None:
            query.close()


@router.get("/{task_id}/measurements/series", response_model=MeasurementSeriesResponse)
def get_task_measurement_series(
    task_id: str,
    request: Request,
    start_timestamp_ms: Annotated[int | None, Query(ge=0)] = None,
    end_timestamp_ms: Annotated[int | None, Query(ge=0)] = None,
    max_points: Annotated[int, Query(ge=200, le=10_000)] = 4_000,
    playback_session: Annotated[
        str | None,
        Header(alias="X-Playback-Session", max_length=128),
    ] = None,
) -> MeasurementSeriesResponse:
    query = _measurement_query_coordinator(request).begin(playback_session)
    try:
        task = _repository(request).get_task(task_id)
        record = _measurement_repository(request).load_series(
            task,
            start_timestamp_ms=start_timestamp_ms,
            end_timestamp_ms=end_timestamp_ms,
            max_points=max_points,
            query=query,
        )
        return _measurement_series_response(record)
    except TaskNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务不存在") from error
    except MeasurementNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务尚无测量记录") from error
    except ValueError as error:
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail=str(error)) from error
    except MeasurementStorageError as error:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail=str(error),
        ) from error
    except MeasurementQueryCancelledError as error:
        raise _cancelled_measurement_query(error) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error
    finally:
        if query is not None:
            query.close()


@router.get("/{task_id}/measurements/series-prefix", response_model=MeasurementSeriesResponse)
def get_task_measurement_series_prefix(
    task_id: str,
    request: Request,
    max_samples: Annotated[int, Query(ge=200, le=10_000)] = 2_000,
    playback_session: Annotated[
        str | None,
        Header(alias="X-Playback-Session", max_length=128),
    ] = None,
) -> MeasurementSeriesResponse:
    query = _measurement_query_coordinator(request).begin(playback_session)
    try:
        task = _repository(request).get_task(task_id)
        record = _measurement_repository(request).load_prefix(
            task,
            max_samples=max_samples,
            query=query,
        )
        return _measurement_series_response(record)
    except TaskNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务不存在") from error
    except MeasurementNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务尚无测量记录") from error
    except ValueError as error:
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail=str(error)) from error
    except MeasurementStorageError as error:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail=str(error),
        ) from error
    except MeasurementQueryCancelledError as error:
        raise _cancelled_measurement_query(error) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error
    finally:
        if query is not None:
            query.close()


@router.get("/{task_id}/measurements", response_model=MeasurementHistoryResponse)
def get_task_measurements(task_id: str, request: Request) -> MeasurementHistoryResponse:
    try:
        task = _repository(request).get_task(task_id)
        history = _measurement_repository(request).load_history(task)
        return _measurement_response(history)
    except TaskNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务不存在") from error
    except MeasurementNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务尚无测量记录") from error
    except MeasurementStorageError as error:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail=str(error),
        ) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.post("/delete-selected", response_model=TaskDeleteManyResponse)
def delete_selected_tasks(payload: TaskDeleteManyRequest, request: Request) -> TaskDeleteManyResponse:
    try:
        records = _repository(request).soft_delete_tasks(payload.task_ids)
        return TaskDeleteManyResponse(
            deleted_task_count=len(records),
            task_ids=[record.task_id for record in records],
        )
    except TaskNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务不存在") from error
    except TaskDeleteConflictError as error:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.post("/purge-data", response_model=TaskPurgeDataResponse)
def purge_task_data(payload: TaskPurgeDataRequest, request: Request) -> TaskPurgeDataResponse:
    try:
        task_ids, released_bytes = _repository(request).purge_task_data(payload.task_ids)
        return TaskPurgeDataResponse(
            removed_task_count=len(task_ids),
            released_bytes=released_bytes,
            task_ids=task_ids,
        )
    except TaskNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务不存在") from error
    except TaskDeleteConflictError as error:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.get("/{task_id}", response_model=TaskResponse)
def get_task(task_id: str, request: Request) -> TaskResponse:
    try:
        return _response(_repository(request).get_task(task_id))
    except TaskNotFoundError as error:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail="任务不存在",
        ) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error


@router.delete("/{task_id}", status_code=status.HTTP_204_NO_CONTENT)
def delete_task(task_id: str, request: Request) -> Response:
    try:
        _repository(request).soft_delete_task(task_id)
        return Response(status_code=status.HTTP_204_NO_CONTENT)
    except TaskNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务不存在") from error
    except TaskDeleteConflictError as error:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error
    except TaskStorageError as error:
        raise _storage_failure(error) from error
