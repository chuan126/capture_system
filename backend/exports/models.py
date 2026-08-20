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
    entry_rtk: RtkEndpointResponse | None
    exit_rtk: RtkEndpointResponse | None


class ReportSelectionRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    task_ids: list[str] = Field(min_length=1, max_length=500)


class ReportPreviewResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    task_count: int
    exportable_task_count: int
    generated_at: datetime
    tasks: list[TaskExportPreviewResponse]


class ExportFileResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    export_format: Literal["txt", "pdf"]
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
