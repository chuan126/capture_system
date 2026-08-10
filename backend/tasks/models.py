from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

TaskStatus = Literal[
    "pending",
    "running",
    "paused",
    "completed",
    "interrupted",
    "failed",
]

TaskOperationPhase = Literal[
    "idle",
    "radar_initializing",
    "entry_rtk_capture",
    "recorder_preparing",
    "recording",
    "pausing",
    "paused",
    "resuming",
    "stop_requested",
    "exit_rtk_capture",
    "finalizing",
    "completed",
    "interrupted",
    "failed",
]

RtkCaptureStatus = Literal[
    "not_requested",
    "pending",
    "confirmed",
    "unconfirmed",
]

TaskLane = Literal["left", "right"]
TaskTravelDirection = Literal["up", "down"]


def _normalize_required_text(value: str, field_name: str, maximum_length: int) -> str:
    normalized = value.strip()
    if not normalized:
        raise ValueError(f"{field_name}不能为空")
    if len(normalized) > maximum_length:
        raise ValueError(f"{field_name}不能超过{maximum_length}个字符")
    if any(ord(character) < 32 for character in normalized):
        raise ValueError(f"{field_name}不能包含控制字符")
    return normalized


class TaskCreateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    tunnel_code: str = Field(description="隧道业务编号")
    tunnel_name: str = Field(description="隧道名称")
    travel_direction: TaskTravelDirection | None = Field(default=None, description="计划行驶方向")
    lane_side: TaskLane | None = Field(default=None, description="计划车道位置")
    clearance_threshold_m: float = Field(default=0.0, ge=0.0, le=20.0, description="计划高度阈值")

    @field_validator("tunnel_code")
    @classmethod
    def validate_tunnel_code(cls, value: str) -> str:
        return _normalize_required_text(value, "隧道编号", 128)

    @field_validator("tunnel_name")
    @classmethod
    def validate_tunnel_name(cls, value: str) -> str:
        return _normalize_required_text(value, "隧道名称", 256)


class TaskBatchCreateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    tasks: list[TaskCreateRequest] = Field(min_length=1, max_length=100)


class TaskDeleteManyRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    task_ids: list[str] = Field(min_length=1, max_length=500)


class TaskDeleteManyResponse(BaseModel):
    deleted_task_count: int
    task_ids: list[str]


class TaskPurgeDataRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    task_ids: list[str] = Field(min_length=1, max_length=500)


class TaskPurgeDataResponse(BaseModel):
    removed_task_count: int
    released_bytes: int
    task_ids: list[str]


class TaskStartRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    travel_direction: TaskTravelDirection | None = None
    lane_side: TaskLane | None = None
    lane: TaskLane | None = None
    lidar_mount_height_m: float = Field(ge=0.0, le=20.0)
    clearance_threshold_m: float = Field(ge=0.0, le=20.0)
    expected_revision: int = Field(ge=0)

    @model_validator(mode="after")
    def validate_lane_fields(self) -> "TaskStartRequest":
        resolved_lane = self.lane_side or self.lane
        if resolved_lane is None:
            raise ValueError("必须提供作业车道")
        if self.lane_side is not None and self.lane is not None and self.lane_side != self.lane:
            raise ValueError("lane 与 lane_side 不一致")
        return self


class TaskCommandRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    expected_revision: int = Field(ge=0)


class TaskControlResponse(BaseModel):
    command_id: str
    accepted: bool
    task_id: str
    status: TaskStatus
    operation_phase: TaskOperationPhase
    status_revision: int
    message: str
    error_code: str | None = None


class TaskControlReadinessResponse(BaseModel):
    ready: bool
    state: str
    detail: str
    bridge_available: bool = False
    services: dict[str, bool] = Field(default_factory=dict)
    missing_services: list[str] = Field(default_factory=list)
    active_task_id: str | None
    active_phase: TaskOperationPhase | None = None
    can_start: bool = False
    can_pause: bool = False
    can_resume: bool = False
    can_stop: bool = False
    can_recover: bool = False
    sensor_data_checked: bool = False
    lidar_online: bool = False
    rtk_online: bool = False
    sensor_blockers: list[str] = Field(default_factory=list)


class TaskResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    task_id: str
    display_id: str
    # 以下字段仅用于兼容旧数据库和旧接口消费者，不再承担前端任务编号语义。
    batch_id: str
    batch_code: str
    sequence: int
    display_sequence: str
    global_sequence: int
    tunnel_code: str
    tunnel_name: str
    status: TaskStatus
    operation_phase: TaskOperationPhase
    status_revision: int
    created_at: datetime
    updated_at: datetime
    start_requested_at: datetime | None
    started_at: datetime | None
    stop_requested_at: datetime | None
    completed_at: datetime | None
    entry_rtk_status: RtkCaptureStatus
    exit_rtk_status: RtkCaptureStatus
    has_measurements: bool
    recording_path: str | None
    local_data_purged_at: datetime | None
    purged_bytes: int
    last_error_code: str | None
    last_error_message: str | None
    warning_code: str | None
    planned_travel_direction: TaskTravelDirection | None = None
    planned_lane_side: TaskLane | None = None
    planned_clearance_threshold_m: float | None = None
    travel_direction: TaskTravelDirection | None = None
    lane_side: TaskLane | None = None
    lane: TaskLane | None = None
    lidar_mount_height_m: float | None = None
    clearance_threshold_m: float | None = None
    schema_version: int
    deleted_at: datetime | None
    delete_reason: str | None
