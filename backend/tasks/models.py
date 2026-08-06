from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator

TaskStatus = Literal[
    "pending",
    "running",
    "paused",
    "completed",
    "interrupted",
    "failed",
]


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


class TaskResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    task_id: str
    sequence: int
    display_sequence: str
    tunnel_code: str
    tunnel_name: str
    status: TaskStatus
    created_at: datetime
    updated_at: datetime
    started_at: datetime | None
    completed_at: datetime | None
    has_measurements: bool
    recording_path: str | None
    schema_version: int
    deleted_at: datetime | None
    delete_reason: str | None
