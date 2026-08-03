from types import SimpleNamespace

from backend.protocols.rtk_v1 import RtkSnapshot, with_fix, with_status


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
