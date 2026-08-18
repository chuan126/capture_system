from __future__ import annotations

import math
import time
from dataclasses import asdict, dataclass
from typing import Any


def _finite_or_none(value: object) -> float | None:
    number = float(value)
    return number if math.isfinite(number) else None


def _stamp_to_nanoseconds(message: object) -> int:
    header = getattr(message, "header")
    stamp = getattr(header, "stamp")
    return int(getattr(stamp, "sec")) * 1_000_000_000 + int(
        getattr(stamp, "nanosec")
    )


@dataclass(frozen=True, slots=True)
class ClearanceSnapshot:
    """浏览器所需的单帧净空结果，保留算法有效性和质量字段。"""

    sequence: int
    emitted_at_ns: int
    stamp_ns: int
    frame_id: str
    valid: bool
    lidar_to_top_m: float | None
    ransac_plane_count: int
    candidate_count: int
    selected_inlier_count: int
    selected_area_m2: float | None
    selected_tilt_deg: float | None
    residual_median_m: float | None
    residual_p95_m: float | None
    minimum_position_east_m: float | None
    minimum_position_north_m: float | None
    minimum_position_up_m: float | None
    valid_point_ratio: float | None
    invalid_reason: str
    processing_time_ms: float | None

    def to_message(self) -> dict[str, Any]:
        return {"type": "clearance_snapshot", **asdict(self)}


def from_ros_message(message: object, sequence: int) -> ClearanceSnapshot:
    return ClearanceSnapshot(
        sequence=sequence,
        emitted_at_ns=time.time_ns(),
        stamp_ns=_stamp_to_nanoseconds(message),
        frame_id=str(getattr(getattr(message, "header"), "frame_id")),
        valid=bool(getattr(message, "valid")),
        lidar_to_top_m=_finite_or_none(getattr(message, "lidar_to_top_m")),
        ransac_plane_count=int(getattr(message, "ransac_plane_count", 0)),
        candidate_count=int(getattr(message, "candidate_count")),
        selected_inlier_count=int(getattr(message, "selected_inlier_count")),
        selected_area_m2=_finite_or_none(getattr(message, "selected_area_m2")),
        selected_tilt_deg=_finite_or_none(getattr(message, "selected_tilt_deg")),
        residual_median_m=_finite_or_none(getattr(message, "residual_median_m")),
        residual_p95_m=_finite_or_none(getattr(message, "residual_p95_m")),
        minimum_position_east_m=_finite_or_none(
            getattr(message, "minimum_position_east_m")
        ),
        minimum_position_north_m=_finite_or_none(
            getattr(message, "minimum_position_north_m")
        ),
        minimum_position_up_m=_finite_or_none(
            getattr(message, "minimum_position_up_m")
        ),
        valid_point_ratio=_finite_or_none(getattr(message, "valid_point_ratio")),
        invalid_reason=str(getattr(message, "invalid_reason")),
        processing_time_ms=_finite_or_none(getattr(message, "processing_time_ms")),
    )
