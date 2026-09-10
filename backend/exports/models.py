from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field

from backend.measurements.models import MeasurementDataOrigin, MeasurementLane, MeasurementTravelDirection, RtkEndpointResponse
from backend.tasks.models import TaskStatus


class TaskExportPreviewResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    task_id: str
    display_id: str
    tunnel_code: str
    tunnel_name: str
    status: TaskStatus
    exportable: bool
    blocked_reason: str | None
    pdf_exportable: bool
    pdf_blocked_reason: str | None
    data_origin: MeasurementDataOrigin | None
    lane: MeasurementLane | None
    travel_direction: MeasurementTravelDirection | None = None
    lane_side: MeasurementLane | None = None
    lane_number_from_right: int | None = Field(default=None, ge=1, le=4)
    started_at: datetime | None
    ended_at: datetime | None
    complete: bool | None
    total_samples: int | None
    valid_samples: int | None
    invalid_samples: int | None
    minimum_height_m: float | None
    normal_minimum_height_m: float | None
    clearance_threshold_m: float | None
    clearance_upper_limit_m: float | None
    raw_min_clearance_m: float | None = None
    effective_min_clearance_m: float | None = None
    recommended_min_clearance_m: float | None = None
    confidence_score: int | None = Field(default=None, ge=0, le=100)
    confidence_level: str | None = None
    confidence_reason: str | None = None
    has_review_required: bool | None = None
    review_required_count: int | None = None
    outlier_count: int | None = None
    protected_structure_count: int | None = None
    entry_rtk: RtkEndpointResponse | None
    exit_rtk: RtkEndpointResponse | None


class ReportSelectionRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    task_ids: list[str] = Field(min_length=1, max_length=500)


class DeepSeekReportRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    api_key: str = Field(min_length=1, max_length=2048)
    model: Literal["deepseek-v4-flash", "deepseek-v4-pro"] = "deepseek-v4-flash"
    api_url: str | None = Field(default=None, min_length=1, max_length=2048)
    skill_prompt: str | None = Field(default=None, min_length=1, max_length=20_000)


class DeepSeekSettings(BaseModel):
    model_config = ConfigDict(extra="forbid")

    api_url: str = Field(min_length=1, max_length=2048)
    api_key: str = Field(default="", max_length=2048)
    model: Literal["deepseek-v4-flash", "deepseek-v4-pro"] = "deepseek-v4-flash"
    skill_prompt: str = Field(min_length=1, max_length=20_000)


class ReportPreviewResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    task_count: int
    exportable_task_count: int
    generated_at: datetime
    tasks: list[TaskExportPreviewResponse]


class ExportFileResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    export_format: Literal["txt", "pdf", "deepseek_pdf"]
    file_name: str
    file_size_bytes: int
    generated_at: datetime
    download_url: str
    report_id: str | None = None
    task_id: str | None = None
    included_task_count: int | None = None
    # 兼容旧客户端，时间编号版本不再使用批次字段。
    batch_id: str | None = None
    batch_code: str | None = None


class ExportJobResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    job_id: str
    export_format: Literal["txt", "pdf", "deepseek_pdf"]
    task_ids: list[str]
    state: Literal["queued", "running", "completed", "failed", "cancelled"]
    phase: str
    progress: float = Field(ge=0.0, le=1.0)
    created_at: datetime
    updated_at: datetime
    error: str | None = None
    file_name: str | None = None
    file_size_bytes: int | None = None
    generated_at: datetime | None = None
    download_url: str | None = None
    report_id: str | None = None
    task_id: str | None = None
    included_task_count: int | None = None
