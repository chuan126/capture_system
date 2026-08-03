from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any


@dataclass(frozen=True, slots=True)
class DeviceStatus:
    state: str = "unknown"
    message: str = "等待诊断"
    values: dict[str, str] | None = None


@dataclass(frozen=True, slots=True)
class SystemStatusSnapshot:
    sequence: int
    emitted_at_ns: int
    lidar: DeviceStatus
    rtk: DeviceStatus
    controller: DeviceStatus
    storage: DeviceStatus

    def to_message(self) -> dict[str, Any]:
        return {"type": "system_status_snapshot", **asdict(self)}


DIAGNOSTIC_NAMES = {
    "system_monitor/lidar": "lidar",
    "system_monitor/rtk": "rtk",
    "system_monitor/controller": "controller",
    "system_monitor/storage": "storage",
}


def diagnostic_state(level: int | bytes) -> str:
    normalized = level[0] if isinstance(level, bytes) else int(level)
    return {0: "ok", 1: "warn", 2: "error", 3: "stale"}.get(
        normalized, "unknown"
    )


def device_status(status: object) -> DeviceStatus:
    return DeviceStatus(
        state=diagnostic_state(getattr(status, "level")),
        message=str(getattr(status, "message")),
        values={
            str(getattr(item, "key")): str(getattr(item, "value"))
            for item in getattr(status, "values")
        },
    )
