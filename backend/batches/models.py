from __future__ import annotations

from datetime import date, datetime
from typing import Literal

from pydantic import BaseModel, ConfigDict

BatchStatus = Literal["active", "completed", "archived", "purged"]


class BatchResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True, extra="forbid")

    batch_id: str
    batch_code: str
    operation_date: date
    daily_sequence: int
    status: BatchStatus
    created_at: datetime
    started_at: datetime
    completed_at: datetime | None
    archived_at: datetime | None
    purged_at: datetime | None
    task_count: int
    visible_task_count: int
    measurement_bytes: int
    report_id: str | None
    report_path: str | None
    report_sha256: str | None
    report_generated_at: datetime | None
    purged_bytes: int


class BatchPurgeResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    batch: BatchResponse
    released_bytes: int
    removed_task_count: int
