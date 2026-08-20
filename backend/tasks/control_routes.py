from __future__ import annotations

import uuid
from typing import Annotated

from fastapi import APIRouter, Header, HTTPException, Request, status

from backend.ros_bridge.task_control_bridge import TaskControlBridge, TaskControlResult
from backend.tasks.models import (
    TaskCommandRequest,
    TaskControlReadinessResponse,
    TaskControlResponse,
    TaskStartRequest,
)
from backend.tasks.repository import TaskNotFoundError, TaskStorageError

router = APIRouter(tags=["task-control"])

_SERVICE_LABELS = {
    "start": "开始",
    "pause": "暂停",
    "resume": "继续",
    "stop": "停止",
    "recover": "恢复",
}


def _service_availability(bridge: TaskControlBridge | None) -> tuple[bool, dict[str, bool]]:
    commands = tuple(_SERVICE_LABELS)
    if bridge is None or not bridge.available:
        return False, {command: False for command in commands}
    values = getattr(bridge, "service_availability", None)
    if isinstance(values, dict):
        return True, {command: bool(values.get(command, False)) for command in commands}
    return True, {command: True for command in commands}


def _missing_service_detail(services: dict[str, bool]) -> str:
    missing = [_SERVICE_LABELS[name] for name, ready in services.items() if not ready]
    if not missing:
        return ""
    return "、".join(missing) + "服务不可用"


def _bridge(request: Request) -> TaskControlBridge | None:
    return request.app.state.task_control_bridge


def _offline_replay_active(request: Request) -> bool:
    manager = getattr(request.app.state, "dev_offline_replay_manager", None)
    return bool(manager is not None and manager.active)


def _sensor_start_readiness(request: Request) -> tuple[bool, bool, bool, list[str], str]:
    hub = getattr(request.app.state, "system_status_hub", None)
    if hub is None:
        return False, False, False, ["system_status"], "系统状态服务不可用"
    stream = hub.current_status()
    snapshot = hub.latest_snapshot
    if stream.get("state") != "streaming" or snapshot is None:
        detail = str(stream.get("detail") or "正在等待系统诊断")
        return False, False, False, ["system_status"], detail

    lidar_online = snapshot.lidar.state == "ok"
    rtk_online = snapshot.rtk.state == "ok"
    blockers: list[str] = []
    if not lidar_online:
        blockers.append("lidar")
    if not rtk_online:
        blockers.append("rtk")
    if not blockers:
        return True, True, True, [], "雷达与RTK均已上线"

    names = []
    if "lidar" in blockers:
        names.append("雷达")
    if "rtk" in blockers:
        names.append("RTK")
    return False, lidar_online, rtk_online, blockers, "、".join(names) + "未上线"


def _command_id(value: str | None) -> str:
    normalized = (value or "").strip()
    if not normalized:
        return str(uuid.uuid4())
    if len(normalized) > 128 or any(ord(character) < 33 or ord(character) > 126 for character in normalized):
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Idempotency-Key只能包含不超过128个字符的可见ASCII字符",
        )
    return normalized


def _response(result: TaskControlResult) -> TaskControlResponse:
    return TaskControlResponse(
        command_id=result.command_id,
        accepted=result.accepted,
        task_id=result.task_id,
        status=result.status,
        operation_phase=result.operation_phase,
        status_revision=result.status_revision,
        message=result.message,
        error_code=result.error_code,
    )


def _raise_rejected(result: TaskControlResult) -> None:
    code = result.error_code or "control_rejected"
    if code == "task_not_found":
        http_status = status.HTTP_404_NOT_FOUND
    elif code in {"state_conflict", "revision_conflict", "active_task_exists", "duplicate_command_conflict"}:
        http_status = status.HTTP_409_CONFLICT
    elif code == "invalid_parameters":
        http_status = status.HTTP_422_UNPROCESSABLE_ENTITY
    elif code in {"recorder_unavailable", "storage_error", "task_database_unavailable"}:
        http_status = status.HTTP_503_SERVICE_UNAVAILABLE
    else:
        http_status = status.HTTP_409_CONFLICT
    raise HTTPException(status_code=http_status, detail=result.message or code)


def _invoke(request: Request, command: str, **kwargs: object) -> TaskControlResponse:
    bridge = _bridge(request)
    if bridge is None:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="任务控制ROS桥未启动",
        )
    try:
        result = bridge.invoke(command, **kwargs)
    except TimeoutError as error:
        raise HTTPException(status_code=status.HTTP_504_GATEWAY_TIMEOUT, detail=str(error)) from error
    except RuntimeError as error:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error)) from error
    if not result.accepted:
        _raise_rejected(result)
    return _response(result)


@router.get("/api/v1/task-control/readiness", response_model=TaskControlReadinessResponse)
def task_control_readiness(request: Request) -> TaskControlReadinessResponse:
    bridge = _bridge(request)
    bridge_available, services = _service_availability(bridge)
    missing_services = [name for name, ready in services.items() if not ready]
    repository = request.app.state.task_repository
    try:
        active = next(
            (task for task in repository.list_tasks(limit=500, offset=0) if task.active_slot is not None),
            None,
        )
    except TaskStorageError as error:
        return TaskControlReadinessResponse(
            ready=False,
            state="task_database_unavailable",
            detail=str(error),
            bridge_available=bridge_available,
            services=services,
            missing_services=missing_services,
            active_task_id=None,
            sensor_data_checked=False,
        )

    active_phase = active.operation_phase if active is not None else None
    sensors_ready, lidar_online, rtk_online, sensor_blockers, sensor_detail = _sensor_start_readiness(request)

    offline_replay_active = _offline_replay_active(request)
    can_start = bool(bridge_available and services["start"] and active is None and sensors_ready and not offline_replay_active)
    can_pause = bool(
        bridge_available
        and services["pause"]
        and active
        and active.status == "running"
        and active.operation_phase == "recording"
    )
    can_resume = bool(
        bridge_available
        and services["resume"]
        and active
        and active.status == "paused"
        and active.operation_phase == "paused"
    )
    can_stop = bool(
        bridge_available
        and services["stop"]
        and active
        and active.active_slot is not None
    )
    recoverable_phases = {
        "radar_initializing", "entry_rtk_capture", "recorder_preparing",
        "pausing", "resuming", "stop_requested", "exit_rtk_capture", "finalizing",
    }
    can_recover = bool(
        bridge_available
        and services["recover"]
        and active
        and active.operation_phase in recoverable_phases
    )

    if not bridge_available:
        readiness_state = "bridge_unavailable"
        readiness_detail = (bridge.error if bridge is not None else None) or "任务控制ROS桥未启动"
    elif active is not None:
        readiness_state = "active_task_exists"
        readiness_detail = (
            f"任务 {active.display_sequence} 当前阶段为 {active.operation_phase}，"
            "可使用当前阶段允许的控制操作"
        )
        missing_detail = _missing_service_detail(services)
        if missing_detail:
            readiness_detail += f"；{missing_detail}"
    elif offline_replay_active:
        readiness_state = "offline_debug_active"
        readiness_detail = "离线算法检测正在运行，停止离线检测后才能开始正式采集"
    elif not services["start"]:
        readiness_state = "start_service_unavailable"
        readiness_detail = "任务控制桥已启动，开始服务不可用"
    elif not sensors_ready:
        readiness_state = "sensor_offline"
        readiness_detail = f"{sensor_detail}，等待设备上线后才能开始采集"
    elif missing_services:
        readiness_state = "degraded"
        readiness_detail = f"任务控制可用；{_missing_service_detail(services)}"
    else:
        readiness_state = "ready"
        readiness_detail = "任务控制可用，雷达与RTK均已上线"

    return TaskControlReadinessResponse(
        ready=can_start,
        state=readiness_state,
        detail=readiness_detail,
        bridge_available=bridge_available,
        services=services,
        missing_services=missing_services,
        active_task_id=active.task_id if active is not None else None,
        active_phase=active_phase,
        can_start=can_start,
        can_pause=can_pause,
        can_resume=can_resume,
        can_stop=can_stop,
        can_recover=can_recover,
        sensor_data_checked=True,
        lidar_online=lidar_online,
        rtk_online=rtk_online,
        sensor_blockers=sensor_blockers,
    )


@router.post(
    "/api/v1/tasks/{task_id}/start",
    response_model=TaskControlResponse,
    status_code=status.HTTP_202_ACCEPTED,
)
def start_task(
    task_id: str,
    payload: TaskStartRequest,
    request: Request,
    idempotency_key: Annotated[str | None, Header(alias="Idempotency-Key")] = None,
) -> TaskControlResponse:
    try:
        request.app.state.task_repository.get_task(task_id)
    except TaskNotFoundError as error:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="任务不存在") from error
    except TaskStorageError as error:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error)) from error
    if _offline_replay_active(request):
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="离线算法检测正在运行，停止离线检测后才能开始正式采集",
        )
    bridge = _bridge(request)
    bridge_available, services = _service_availability(bridge)
    if not bridge_available:
        detail = (bridge.error if bridge is not None else None) or "任务控制ROS桥未启动"
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=detail)
    if not services["start"]:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="开始服务不可用")
    sensors_ready, _lidar_online, _rtk_online, _sensor_blockers, sensor_detail = _sensor_start_readiness(request)
    if not sensors_ready:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=f"{sensor_detail}，无法开始采集",
        )
    resolved_lane = payload.lane_side or payload.lane
    return _invoke(
        request,
        "start",
        task_id=task_id,
        command_id=_command_id(idempotency_key),
        expected_revision=payload.expected_revision,
        travel_direction=payload.travel_direction,
        lane_side=resolved_lane,
        lane=resolved_lane,
        lidar_mount_height_m=payload.lidar_mount_height_m,
        clearance_threshold_m=payload.clearance_threshold_m,
        clearance_upper_limit_m=payload.clearance_upper_limit_m,
    )


def _simple_command(
    command: str,
    task_id: str,
    payload: TaskCommandRequest,
    request: Request,
    idempotency_key: str | None,
) -> TaskControlResponse:
    return _invoke(
        request,
        command,
        task_id=task_id,
        command_id=_command_id(idempotency_key),
        expected_revision=payload.expected_revision,
    )


@router.post(
    "/api/v1/tasks/{task_id}/pause",
    response_model=TaskControlResponse,
    status_code=status.HTTP_202_ACCEPTED,
)
def pause_task(
    task_id: str,
    payload: TaskCommandRequest,
    request: Request,
    idempotency_key: Annotated[str | None, Header(alias="Idempotency-Key")] = None,
) -> TaskControlResponse:
    return _simple_command("pause", task_id, payload, request, idempotency_key)


@router.post(
    "/api/v1/tasks/{task_id}/resume",
    response_model=TaskControlResponse,
    status_code=status.HTTP_202_ACCEPTED,
)
def resume_task(
    task_id: str,
    payload: TaskCommandRequest,
    request: Request,
    idempotency_key: Annotated[str | None, Header(alias="Idempotency-Key")] = None,
) -> TaskControlResponse:
    return _simple_command("resume", task_id, payload, request, idempotency_key)


@router.post(
    "/api/v1/tasks/{task_id}/stop",
    response_model=TaskControlResponse,
    status_code=status.HTTP_202_ACCEPTED,
)
def stop_task(
    task_id: str,
    payload: TaskCommandRequest,
    request: Request,
    idempotency_key: Annotated[str | None, Header(alias="Idempotency-Key")] = None,
) -> TaskControlResponse:
    return _simple_command("stop", task_id, payload, request, idempotency_key)


@router.post(
    "/api/v1/tasks/{task_id}/recover",
    response_model=TaskControlResponse,
    status_code=status.HTTP_202_ACCEPTED,
)
def recover_task(
    task_id: str,
    payload: TaskCommandRequest,
    request: Request,
    idempotency_key: Annotated[str | None, Header(alias="Idempotency-Key")] = None,
) -> TaskControlResponse:
    return _simple_command("recover", task_id, payload, request, idempotency_key)
