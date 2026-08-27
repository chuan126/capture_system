from __future__ import annotations

from fastapi import APIRouter, HTTPException, Request, status
from pydantic import BaseModel, ConfigDict, Field

from backend.device_settings import DeviceSettingsError
from backend.devtools.parameters import DevParameterError, DevParameterService
from backend.tasks.repository import TaskStorageError


router = APIRouter(prefix="/api/v1/algorithm-parameters", tags=["algorithm-parameters"])


class ClearanceAlgorithmParameters(BaseModel):
    model_config = ConfigDict(extra="forbid")

    detection_radius_m: float = Field(ge=0.1, le=5.0)
    min_support_points: int = Field(ge=1, le=10_000)


def apply_clearance_algorithm_parameters(request: Request, payload: ClearanceAlgorithmParameters) -> None:
    service: DevParameterService = request.app.state.algorithm_parameter_service
    try:
        service.set_parameters({
            "clearance.detection_radius_m": payload.detection_radius_m,
            "clearance.min_support_points": payload.min_support_points,
        })
        request.app.state.device_settings_store.set_clearance_algorithm(
            payload.detection_radius_m,
            payload.min_support_points,
        )
    except (DevParameterError, DeviceSettingsError) as error:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error


@router.get("", response_model=ClearanceAlgorithmParameters)
def get_parameters(request: Request) -> ClearanceAlgorithmParameters:
    try:
        return ClearanceAlgorithmParameters(**request.app.state.device_settings_store.get_clearance_algorithm())
    except DeviceSettingsError as error:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error)) from error


@router.put("", response_model=ClearanceAlgorithmParameters)
def put_parameters(payload: ClearanceAlgorithmParameters, request: Request) -> ClearanceAlgorithmParameters:
    try:
        active = next(
            (task for task in request.app.state.task_repository.list_tasks(limit=500) if task.active_slot is not None),
            None,
        )
    except TaskStorageError as error:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error)) from error
    if active is not None:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="正式采集任务活动期间不能修改净空算法参数")
    apply_clearance_algorithm_parameters(request, payload)
    return payload
