from __future__ import annotations

from dataclasses import asdict, dataclass, replace
from typing import Any


def _stamp_to_nanoseconds(message: object) -> int:
    header = getattr(message, "header")
    stamp = getattr(header, "stamp")
    return int(getattr(stamp, "sec")) * 1_000_000_000 + int(
        getattr(stamp, "nanosec")
    )


@dataclass(frozen=True, slots=True)
class RtkSnapshot:
    """浏览器所需的RTK最新值快照，不包含质量推断。"""

    sequence: int = 0
    emitted_at_ns: int = 0
    serial_connected: bool | None = None
    serial_message: str = "等待RTK诊断"
    status_stamp_ns: int | None = None
    event_mask: int | None = None
    rmc_validity: int | None = None
    gps_state: int | None = None
    satellite_count: int | None = None
    hdop: float | None = None
    pdop: float | None = None
    latitude_sigma: float | None = None
    longitude_sigma: float | None = None
    height_sigma: float | None = None
    speed_knots: float | None = None
    track_degrees: float | None = None
    fix_stamp_ns: int | None = None
    fix_status: int | None = None
    latitude: float | None = None
    longitude: float | None = None
    altitude: float | None = None
    localization_stamp_ns: int | None = None
    localization_valid: bool | None = None
    localization_mode: int | None = None
    localization_heading_source: int | None = None
    localization_latitude: float | None = None
    localization_longitude: float | None = None
    localization_altitude: float | None = None
    localization_heading_deg: float | None = None
    localization_vehicle_attitude_valid: bool | None = None
    localization_vehicle_pitch_deg: float | None = None
    localization_vehicle_roll_deg: float | None = None
    localization_vehicle_heading_deg: float | None = None
    localization_heading_alignment_valid: bool | None = None
    localization_delta_yaw_deg: float | None = None
    localization_scale_calibration_mode: int | None = None
    localization_scale_status: int | None = None
    localization_scale_valid: bool | None = None
    localization_horizontal_scale: float | None = None
    localization_vertical_scale: float | None = None
    localization_scale_baseline_m: float | None = None
    localization_scale_fit_residual_m: float | None = None
    localization_heading_baseline_m: float | None = None
    localization_heading_alignment_reason: str | None = None
    localization_heading_fit_sample_count: int | None = None
    localization_heading_fit_baseline_m: float | None = None
    localization_heading_fit_rmse_m: float | None = None
    localization_heading_fit_p95_residual_m: float | None = None
    localization_heading_fit_inlier_ratio: float | None = None
    localization_heading_fit_delta_yaw_deg: float | None = None
    localization_heading_fit_valid: bool | None = None
    localization_heading_fit_window_span_m: float | None = None
    localization_heading_error_before_deg: float | None = None
    localization_heading_error_after_deg: float | None = None
    localization_simulation_test_mode: int | None = None
    localization_simulation_progress_percent: float | None = None
    localization_distance_from_anchor_m: float | None = None
    localization_dr_duration_s: float | None = None
    localization_rtk_age_s: float | None = None
    localization_odometry_age_s: float | None = None
    localization_imu_age_s: float | None = None
    localization_position_difference_to_rtk_m: float | None = None
    localization_invalid_reason: str | None = None

    def to_message(self) -> dict[str, Any]:
        return {"type": "rtk_snapshot", **asdict(self)}


def with_status(snapshot: RtkSnapshot, message: object) -> RtkSnapshot:
    return replace(
        snapshot,
        status_stamp_ns=_stamp_to_nanoseconds(message),
        event_mask=int(getattr(message, "event_mask")),
        rmc_validity=int(getattr(message, "rmc_validity")),
        gps_state=int(getattr(message, "gps_state")),
        satellite_count=int(getattr(message, "satellite_count")),
        hdop=float(getattr(message, "hdop")),
        pdop=float(getattr(message, "pdop")),
        latitude_sigma=float(getattr(message, "latitude_sigma")),
        longitude_sigma=float(getattr(message, "longitude_sigma")),
        height_sigma=float(getattr(message, "height_sigma")),
        speed_knots=float(getattr(message, "speed_knots")),
        track_degrees=float(getattr(message, "track_degrees")),
    )


def with_fix(snapshot: RtkSnapshot, message: object) -> RtkSnapshot:
    status = getattr(message, "status")
    return replace(
        snapshot,
        fix_stamp_ns=_stamp_to_nanoseconds(message),
        fix_status=int(getattr(status, "status")),
        latitude=float(getattr(message, "latitude")),
        longitude=float(getattr(message, "longitude")),
        altitude=float(getattr(message, "altitude")),
    )


def with_localization_status(snapshot: RtkSnapshot, message: object) -> RtkSnapshot:
    return replace(
        snapshot,
        localization_stamp_ns=_stamp_to_nanoseconds(message),
        localization_valid=bool(getattr(message, "valid")),
        localization_mode=int(getattr(message, "mode")),
        localization_heading_source=int(getattr(message, "heading_source")),
        localization_latitude=float(getattr(message, "latitude")),
        localization_longitude=float(getattr(message, "longitude")),
        localization_altitude=float(getattr(message, "altitude")),
        localization_heading_deg=float(getattr(message, "heading_deg")),
        localization_vehicle_attitude_valid=bool(getattr(message, "vehicle_attitude_valid")),
        localization_vehicle_pitch_deg=float(getattr(message, "vehicle_pitch_deg")),
        localization_vehicle_roll_deg=float(getattr(message, "vehicle_roll_deg")),
        localization_vehicle_heading_deg=float(getattr(message, "vehicle_heading_deg")),
        localization_heading_alignment_valid=bool(
            getattr(message, "heading_alignment_valid")
        ),
        localization_delta_yaw_deg=float(getattr(message, "delta_yaw_deg")),
        localization_scale_calibration_mode=int(
            getattr(message, "scale_calibration_mode")
        ),
        localization_scale_status=int(getattr(message, "scale_status")),
        localization_scale_valid=bool(getattr(message, "scale_valid")),
        localization_horizontal_scale=float(getattr(message, "horizontal_scale")),
        localization_vertical_scale=float(getattr(message, "vertical_scale")),
        localization_scale_baseline_m=float(getattr(message, "scale_baseline_m")),
        localization_scale_fit_residual_m=float(
            getattr(message, "scale_fit_residual_m")
        ),
        localization_heading_baseline_m=float(getattr(message, "heading_baseline_m")),
        localization_heading_alignment_reason=str(
            getattr(message, "heading_alignment_reason")
        ),
        localization_heading_fit_sample_count=int(
            getattr(message, "heading_fit_sample_count")
        ),
        localization_heading_fit_baseline_m=float(
            getattr(message, "heading_fit_baseline_m")
        ),
        localization_heading_fit_rmse_m=float(getattr(message, "heading_fit_rmse_m")),
        localization_heading_fit_p95_residual_m=float(
            getattr(message, "heading_fit_p95_residual_m")
        ),
        localization_heading_fit_inlier_ratio=float(
            getattr(message, "heading_fit_inlier_ratio")
        ),
        localization_heading_fit_delta_yaw_deg=float(
            getattr(message, "heading_fit_delta_yaw_deg")
        ),
        localization_heading_fit_valid=bool(getattr(message, "heading_fit_valid")),
        localization_heading_fit_window_span_m=float(
            getattr(message, "heading_fit_window_span_m")
        ),
        localization_heading_error_before_deg=float(
            getattr(message, "heading_error_before_deg")
        ),
        localization_heading_error_after_deg=float(
            getattr(message, "heading_error_after_deg")
        ),
        localization_simulation_test_mode=int(getattr(message, "simulation_test_mode")),
        localization_simulation_progress_percent=float(
            getattr(message, "simulation_progress_percent")
        ),
        localization_distance_from_anchor_m=float(
            getattr(message, "distance_from_anchor_m")
        ),
        localization_dr_duration_s=float(getattr(message, "dr_duration_s")),
        localization_rtk_age_s=float(getattr(message, "rtk_age_s")),
        localization_odometry_age_s=float(getattr(message, "odometry_age_s")),
        localization_imu_age_s=float(getattr(message, "imu_age_s")),
        localization_position_difference_to_rtk_m=float(
            getattr(message, "position_difference_to_rtk_m")
        ),
        localization_invalid_reason=str(getattr(message, "invalid_reason")),
    )


def with_serial_diagnostic(
    snapshot: RtkSnapshot,
    *,
    connected: bool,
    message: str,
) -> RtkSnapshot:
    return replace(
        snapshot,
        serial_connected=connected,
        serial_message=message,
    )
