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
