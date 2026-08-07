import type { RtkCaptureStatus, TaskOperationPhase } from "@/components/workflow/taskModel";

export type TaskStatusSnapshot = {
  taskId: string;
  taskSequence: number;
  status: "pending" | "running" | "paused" | "completed" | "interrupted" | "failed";
  operationPhase: TaskOperationPhase;
  statusRevision: number;
  commandId: string;
  message: string;
  errorCode: string | null;
  entryRtkStatus: RtkCaptureStatus;
  exitRtkStatus: RtkCaptureStatus;
  hasMeasurements: boolean;
  recordingPath: string | null;
  startedAtNs: number;
  completedAtNs: number;
  emittedAtNs: number;
};

export type TaskStatusMessage =
  | { type: "status"; state: string; reason: string; detail: string }
  | ({ type: "task_status_snapshot" } & Record<string, unknown>);

const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

export const parseTaskStatusSnapshot = (value: unknown): TaskStatusSnapshot | null => {
  if (!isObject(value) || value.type !== "task_status_snapshot") return null;
  if (
    typeof value.task_id !== "string" ||
    !Number.isInteger(value.task_sequence) ||
    typeof value.status !== "string" ||
    typeof value.operation_phase !== "string" ||
    !Number.isInteger(value.status_revision) ||
    typeof value.command_id !== "string" ||
    typeof value.message !== "string" ||
    !(value.error_code === null || typeof value.error_code === "string") ||
    typeof value.entry_rtk_status !== "string" ||
    typeof value.exit_rtk_status !== "string" ||
    typeof value.has_measurements !== "boolean" ||
    !(value.recording_path === null || typeof value.recording_path === "string") ||
    typeof value.started_at_ns !== "number" ||
    typeof value.completed_at_ns !== "number" ||
    typeof value.emitted_at_ns !== "number"
  ) return null;
  return {
    taskId: value.task_id,
    taskSequence: Number(value.task_sequence),
    status: value.status as TaskStatusSnapshot["status"],
    operationPhase: value.operation_phase as TaskOperationPhase,
    statusRevision: Number(value.status_revision),
    commandId: value.command_id,
    message: value.message,
    errorCode: value.error_code,
    entryRtkStatus: value.entry_rtk_status as RtkCaptureStatus,
    exitRtkStatus: value.exit_rtk_status as RtkCaptureStatus,
    hasMeasurements: value.has_measurements,
    recordingPath: value.recording_path,
    startedAtNs: value.started_at_ns,
    completedAtNs: value.completed_at_ns,
    emittedAtNs: value.emitted_at_ns,
  };
};
