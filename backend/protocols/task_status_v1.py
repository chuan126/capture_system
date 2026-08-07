from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True, slots=True)
class TaskStatusSnapshot:
    task_id: str
    task_sequence: int
    status: str
    operation_phase: str
    status_revision: int
    command_id: str
    message: str
    error_code: str | None
    entry_rtk_status: str
    exit_rtk_status: str
    has_measurements: bool
    recording_path: str | None
    started_at_ns: int
    completed_at_ns: int
    emitted_at_ns: int

    def to_message(self) -> dict[str, Any]:
        return {
            "type": "task_status_snapshot",
            "task_id": self.task_id,
            "task_sequence": self.task_sequence,
            "status": self.status,
            "operation_phase": self.operation_phase,
            "status_revision": self.status_revision,
            "command_id": self.command_id,
            "message": self.message,
            "error_code": self.error_code,
            "entry_rtk_status": self.entry_rtk_status,
            "exit_rtk_status": self.exit_rtk_status,
            "has_measurements": self.has_measurements,
            "recording_path": self.recording_path,
            "started_at_ns": self.started_at_ns,
            "completed_at_ns": self.completed_at_ns,
            "emitted_at_ns": self.emitted_at_ns,
        }


def from_ros_message(message: object) -> TaskStatusSnapshot:
    header = getattr(message, "header")
    stamp = getattr(header, "stamp")
    emitted_at_ns = int(getattr(stamp, "sec")) * 1_000_000_000 + int(
        getattr(stamp, "nanosec")
    )
    error_code = str(getattr(message, "error_code", "")).strip() or None
    recording_path = str(getattr(message, "recording_path", "")).strip() or None
    return TaskStatusSnapshot(
        task_id=str(getattr(message, "task_id")),
        task_sequence=int(getattr(message, "task_sequence")),
        status=str(getattr(message, "status")),
        operation_phase=str(getattr(message, "operation_phase")),
        status_revision=int(getattr(message, "status_revision")),
        command_id=str(getattr(message, "command_id", "")),
        message=str(getattr(message, "message", "")),
        error_code=error_code,
        entry_rtk_status=str(getattr(message, "entry_rtk_status", "not_requested")),
        exit_rtk_status=str(getattr(message, "exit_rtk_status", "not_requested")),
        has_measurements=bool(getattr(message, "has_measurements")),
        recording_path=recording_path,
        started_at_ns=int(getattr(message, "started_at_ns")),
        completed_at_ns=int(getattr(message, "completed_at_ns")),
        emitted_at_ns=emitted_at_ns,
    )
