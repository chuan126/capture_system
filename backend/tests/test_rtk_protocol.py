from types import SimpleNamespace

from backend.protocols.rtk_v1 import (
    RtkSnapshot,
    with_fix,
    with_localization_status,
    with_status,
)


def header(sec: int, nanosec: int) -> SimpleNamespace:
    return SimpleNamespace(stamp=SimpleNamespace(sec=sec, nanosec=nanosec))


def test_maps_status_and_fix_without_quality_inference() -> None:
    status_message = SimpleNamespace(
        header=header(12, 34),
        event_mask=4,
        rmc_validity=86,
        gps_state=0,
        satellite_count=0,
        hdop=0.0,
        pdop=0.0,
        latitude_sigma=0.0,
        longitude_sigma=0.0,
        height_sigma=0.0,
        speed_knots=0.0,
        track_degrees=0.0,
    )
    fix_message = SimpleNamespace(
        header=header(13, 56),
        status=SimpleNamespace(status=-1),
        latitude=0.0,
        longitude=0.0,
        altitude=0.0,
    )

    snapshot = with_status(RtkSnapshot(), status_message)
    snapshot = with_fix(snapshot, fix_message)

    assert snapshot.status_stamp_ns == 12_000_000_034
    assert snapshot.fix_stamp_ns == 13_000_000_056
    assert snapshot.event_mask == 4
    assert snapshot.rmc_validity == 86
    assert snapshot.gps_state == 0
    assert snapshot.fix_status == -1
    assert snapshot.latitude == 0.0
    assert "quality" not in snapshot.to_message()
    assert "stable" not in snapshot.to_message()


def test_maps_localization_status_as_fused_position_fields() -> None:
    message = SimpleNamespace(
        header=header(20, 5),
        valid=True,
        mode=2,
        heading_source=3,
        latitude=30.1234567,
        longitude=114.1234567,
        altitude=40.5,
        heading_deg=91.2,
        odin_attitude_valid=True,
        odin_pitch_deg=89.5,
        odin_roll_deg=0.2,
        odin_yaw_deg=45.0,
        heading_alignment_valid=True,
        delta_yaw_deg=12.5,
        scale_calibration_mode=0,
        scale_status=0,
        scale_valid=False,
        horizontal_scale=1.0,
        vertical_scale=1.0,
        scale_baseline_m=0.0,
        scale_fit_residual_m=0.0,
        heading_baseline_m=80.0,
        heading_alignment_reason="ALIGNED",
        distance_from_anchor_m=120.0,
        dr_duration_s=7.5,
        rtk_age_s=2.0,
        odometry_age_s=0.02,
        imu_age_s=0.01,
        position_difference_to_rtk_m=3.4,
        invalid_reason="NONE",
    )

    snapshot = with_localization_status(RtkSnapshot(), message)

    assert snapshot.localization_stamp_ns == 20_000_000_005
    assert snapshot.localization_valid is True
    assert snapshot.localization_mode == 2
    assert snapshot.localization_heading_source == 3
    assert snapshot.localization_latitude == 30.1234567
    assert snapshot.localization_longitude == 114.1234567
    assert snapshot.localization_odin_pitch_deg == 89.5
    assert snapshot.localization_scale_calibration_mode == 0
    assert snapshot.localization_scale_status == 0
    assert snapshot.localization_horizontal_scale == 1.0
    assert snapshot.localization_invalid_reason == "NONE"
