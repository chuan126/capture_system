from types import SimpleNamespace

from backend.protocols.clearance_v1 import from_ros_message


def make_message(*, valid: bool, height: float) -> SimpleNamespace:
    return SimpleNamespace(
        header=SimpleNamespace(
            stamp=SimpleNamespace(sec=12, nanosec=34), frame_id="odin_lidar"
        ),
        valid=valid,
        lidar_to_top_m=height,
        ransac_plane_count=3,
        surface_count=2,
        candidate_count=4,
        selected_inlier_count=1234,
        selected_area_m2=1.25,
        selected_tilt_deg=2.5,
        residual_median_m=0.01,
        residual_p95_m=0.03,
        minimum_position_east_m=-0.4,
        minimum_position_north_m=0.7,
        minimum_position_up_m=height,
        valid_point_ratio=0.51,
        invalid_reason="NONE" if valid else "NO_PLANE_FOUND",
        processing_time_ms=46.8,
    )


def test_maps_valid_clearance_result() -> None:
    snapshot = from_ros_message(make_message(valid=True, height=1.723), 7)

    assert snapshot.sequence == 7
    assert snapshot.stamp_ns == 12_000_000_034
    assert snapshot.frame_id == "odin_lidar"
    assert snapshot.valid is True
    assert snapshot.lidar_to_top_m == 1.723
    assert snapshot.surface_count == 2
    assert snapshot.minimum_position_east_m == -0.4
    assert snapshot.minimum_position_up_m == 1.723
    assert snapshot.to_message()["type"] == "clearance_snapshot"


def test_converts_invalid_nan_fields_to_json_null() -> None:
    snapshot = from_ros_message(make_message(valid=False, height=float("nan")), 1)

    assert snapshot.valid is False
    assert snapshot.lidar_to_top_m is None
    assert snapshot.invalid_reason == "NO_PLANE_FOUND"
