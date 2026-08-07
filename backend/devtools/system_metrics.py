from __future__ import annotations

import os
import threading
from pathlib import Path


class SystemMetricsSampler:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._previous_cpu: tuple[int, int] | None = None

    def snapshot(self) -> dict[str, object]:
        with self._lock:
            cpu_percent = self._cpu_percent()
        memory = self._memory()
        return {
            "cpu_percent": cpu_percent,
            "load_1m": round(os.getloadavg()[0], 3) if hasattr(os, "getloadavg") else None,
            "memory_total_bytes": memory[0],
            "memory_available_bytes": memory[1],
            "memory_used_percent": None if not memory[0] else round((memory[0] - memory[1]) * 100.0 / memory[0], 1),
            "soc_temperature_c": self._temperature(),
            "uptime_seconds": self._uptime(),
        }

    def _cpu_percent(self) -> float | None:
        try:
            parts = Path("/proc/stat").read_text(encoding="utf-8").splitlines()[0].split()[1:]
            values = [int(value) for value in parts]
        except (OSError, ValueError, IndexError):
            return None
        idle = values[3] + (values[4] if len(values) > 4 else 0)
        total = sum(values)
        current = (total, idle)
        previous = self._previous_cpu
        self._previous_cpu = current
        if previous is None:
            return None
        total_delta = total - previous[0]
        idle_delta = idle - previous[1]
        if total_delta <= 0:
            return None
        return round(max(0.0, min(100.0, (total_delta - idle_delta) * 100.0 / total_delta)), 1)

    @staticmethod
    def _memory() -> tuple[int, int]:
        values: dict[str, int] = {}
        try:
            for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
                key, raw = line.split(":", 1)
                first = raw.strip().split()[0]
                values[key] = int(first) * 1024
        except (OSError, ValueError, IndexError):
            return 0, 0
        return values.get("MemTotal", 0), values.get("MemAvailable", 0)

    @staticmethod
    def _temperature() -> float | None:
        values: list[float] = []
        for path in Path("/sys/class/thermal").glob("thermal_zone*/temp"):
            try:
                raw = float(path.read_text(encoding="utf-8").strip())
            except (OSError, ValueError):
                continue
            value = raw / 1000.0 if raw > 1000 else raw
            if -40.0 <= value <= 150.0:
                values.append(value)
        return round(max(values), 1) if values else None

    @staticmethod
    def _uptime() -> float | None:
        try:
            return round(float(Path("/proc/uptime").read_text(encoding="utf-8").split()[0]), 1)
        except (OSError, ValueError, IndexError):
            return None
