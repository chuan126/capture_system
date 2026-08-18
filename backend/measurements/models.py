from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, ConfigDict

MeasurementDataOrigin = Literal["recorded", "test_fixture"]
MeasurementLane = Literal["left", "right", "unknown"]
MeasurementTravelDirection = Literal["up", "down", "unknown"]


class ClearanceHistorySampleResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    sample_index: int
    timestamp_ms: int
    elapsed_ms: float
    height_m: float | None
    lidar_to_top_m: float | None
    valid: bool
    invalid_reason: str | None
    quality_score: float | None


class RtkEndpointResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    timestamp_ms: int
    latitude_deg: float
    longitude_deg: float
    altitude_m: float | None
    fix_type: str
    valid: bool


class PauseIntervalResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    started_elapsed_ms: float
    ended_elapsed_ms: float


class MeasurementStatisticsResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    total_samples: int
    valid_samples: int
    invalid_samples: int
    minimum_height_m: float | None
    average_height_m: float | None
    maximum_height_m: float | None
    duration_ms: float
    nominal_sample_rate_hz: float
    actual_average_sample_rate_hz: float | None


class MeasurementHistoryResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    task_id: str
    recording_schema_version: int
    data_origin: MeasurementDataOrigin
    lane: MeasurementLane
    travel_direction: MeasurementTravelDirection = "unknown"
    lane_side: MeasurementLane = "unknown"
    started_at: datetime
    ended_at: datetime | None
    complete: bool
    algorithm_version: str | None
    config_version: str | None
    software_version: str | None
    statistics: MeasurementStatisticsResponse
    entry_rtk: RtkEndpointResponse | None
    exit_rtk: RtkEndpointResponse | None
    pause_intervals: list[PauseIntervalResponse]
    samples: list[ClearanceHistorySampleResponse]


class MeasurementSummaryResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    task_id: str
    recording_schema_version: int
    data_origin: MeasurementDataOrigin
    lane: MeasurementLane
    travel_direction: MeasurementTravelDirection = "unknown"
    lane_side: MeasurementLane = "unknown"
    started_at: datetime
    ended_at: datetime | None
    complete: bool
    algorithm_version: str | None
    config_version: str | None
    software_version: str | None
    statistics: MeasurementStatisticsResponse
    entry_rtk: RtkEndpointResponse | None
    exit_rtk: RtkEndpointResponse | None
    pause_interval_count: int
    first_sample_index: int
    last_sample_index: int
    first_timestamp_ms: int
    last_timestamp_ms: int


class ClearanceSeriesSampleResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    sample_index: int
    timestamp_ms: int
    elapsed_ms: float
    height_m: float | None
    valid: bool
    invalid_reason: str | None


class MeasurementSeriesResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    task_id: str
    domain_start_timestamp_ms: int
    domain_end_timestamp_ms: int
    requested_start_timestamp_ms: int
    requested_end_timestamp_ms: int
    source_sample_count: int
    returned_sample_count: int
    downsampled: bool
    samples: list[ClearanceSeriesSampleResponse]
