import type { CollectionTaskLane, RtkCaptureStatus, TaskOperationPhase } from "@/components/workflow/taskModel";

export class TaskControlApiError extends Error {
  readonly status: number | null;

  constructor(message: string, status: number | null = null) {
    super(message);
    this.name = "TaskControlApiError";
    this.status = status;
  }
}

type TaskApiStatus = "pending" | "running" | "paused" | "completed" | "interrupted" | "failed";

export type TaskControlResult = {
  commandId: string;
  accepted: boolean;
  taskId: string;
  status: TaskApiStatus;
  operationPhase: TaskOperationPhase;
  statusRevision: number;
  message: string;
  errorCode: string | null;
};

export type TaskControlServiceName = "start" | "pause" | "resume" | "stop" | "recover";

export type TaskControlReadiness = {
  ready: boolean;
  state: string;
  detail: string;
  bridgeAvailable: boolean;
  services: Record<TaskControlServiceName, boolean>;
  missingServices: TaskControlServiceName[];
  activeTaskId: string | null;
  activePhase: TaskOperationPhase | null;
  canStart: boolean;
  canPause: boolean;
  canResume: boolean;
  canStop: boolean;
  canRecover: boolean;
  sensorDataChecked: false;
};

const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const readErrorMessage = async (response: Response): Promise<string> => {
  try {
    const payload = await response.json();
    if (isObject(payload) && typeof payload.detail === "string") return payload.detail;
  } catch {
    // 非JSON错误使用状态文本。
  }
  return response.statusText || `HTTP ${response.status}`;
};

const requestControl = async (
  url: string,
  body: Record<string, unknown>,
  idempotencyKey: string,
): Promise<TaskControlResult> => {
  let response: Response;
  try {
    response = await fetch(url, {
      method: "POST",
      headers: {
        Accept: "application/json",
        "Content-Type": "application/json",
        "Idempotency-Key": idempotencyKey,
      },
      body: JSON.stringify(body),
    });
  } catch (error) {
    const detail = error instanceof Error ? error.message : "网络请求失败";
    throw new TaskControlApiError(`无法连接任务控制接口：${detail}`);
  }
  if (!response.ok) throw new TaskControlApiError(await readErrorMessage(response), response.status);
  const payload: unknown = await response.json();
  if (!isObject(payload)) throw new TaskControlApiError("任务控制接口返回了无效对象", response.status);
  if (
    typeof payload.command_id !== "string" ||
    typeof payload.accepted !== "boolean" ||
    typeof payload.task_id !== "string" ||
    typeof payload.status !== "string" ||
    typeof payload.operation_phase !== "string" ||
    !Number.isInteger(payload.status_revision) ||
    typeof payload.message !== "string" ||
    !(payload.error_code === null || typeof payload.error_code === "string")
  ) {
    throw new TaskControlApiError("任务控制接口字段无效", response.status);
  }
  return {
    commandId: payload.command_id,
    accepted: payload.accepted,
    taskId: payload.task_id,
    status: payload.status as TaskApiStatus,
    operationPhase: payload.operation_phase as TaskOperationPhase,
    statusRevision: Number(payload.status_revision),
    message: payload.message,
    errorCode: payload.error_code,
  };
};

const laneValues: Record<CollectionTaskLane, "left" | "right"> = {
  左车道: "left",
  右车道: "right",
};

export const startTaskControl = (
  taskId: string,
  options: {
    lane: CollectionTaskLane;
    lidarMountHeightM: number;
    clearanceThresholdM: number;
    expectedRevision: number;
    idempotencyKey: string;
  },
) => requestControl(
  `/api/v1/tasks/${encodeURIComponent(taskId)}/start`,
  {
    lane: laneValues[options.lane],
    lidar_mount_height_m: options.lidarMountHeightM,
    clearance_threshold_m: options.clearanceThresholdM,
    expected_revision: options.expectedRevision,
  },
  options.idempotencyKey,
);

const simpleCommand = (
  taskId: string,
  command: "pause" | "resume" | "stop" | "recover",
  expectedRevision: number,
  idempotencyKey: string,
) => requestControl(
  `/api/v1/tasks/${encodeURIComponent(taskId)}/${command}`,
  { expected_revision: expectedRevision },
  idempotencyKey,
);

export const pauseTaskControl = (taskId: string, expectedRevision: number, idempotencyKey: string) =>
  simpleCommand(taskId, "pause", expectedRevision, idempotencyKey);

export const resumeTaskControl = (taskId: string, expectedRevision: number, idempotencyKey: string) =>
  simpleCommand(taskId, "resume", expectedRevision, idempotencyKey);

export const stopTaskControl = (taskId: string, expectedRevision: number, idempotencyKey: string) =>
  simpleCommand(taskId, "stop", expectedRevision, idempotencyKey);

export const recoverTaskControl = (taskId: string, expectedRevision: number, idempotencyKey: string) =>
  simpleCommand(taskId, "recover", expectedRevision, idempotencyKey);

export const getTaskControlReadiness = async (): Promise<TaskControlReadiness> => {
  let response: Response;
  try {
    response = await fetch("/api/v1/task-control/readiness", {
      method: "GET",
      headers: { Accept: "application/json" },
      cache: "no-store",
    });
  } catch (error) {
    const detail = error instanceof Error ? error.message : "网络请求失败";
    throw new TaskControlApiError(`无法连接任务控制接口：${detail}`);
  }
  if (!response.ok) throw new TaskControlApiError(await readErrorMessage(response), response.status);
  const payload: unknown = await response.json();
  if (!isObject(payload) || typeof payload.ready !== "boolean" || typeof payload.detail !== "string") {
    throw new TaskControlApiError("任务控制准备状态无效", response.status);
  }
  const serviceNames: TaskControlServiceName[] = ["start", "pause", "resume", "stop", "recover"];
  const rawServices = isObject(payload.services) ? payload.services : {};
  const services = Object.fromEntries(serviceNames.map((name) => [name, rawServices[name] === true])) as Record<TaskControlServiceName, boolean>;
  const missingServices = Array.isArray(payload.missing_services)
    ? payload.missing_services.filter((value): value is TaskControlServiceName =>
      typeof value === "string" && serviceNames.includes(value as TaskControlServiceName))
    : serviceNames.filter((name) => !services[name]);
  return {
    ready: payload.ready,
    state: typeof payload.state === "string" ? payload.state : "unknown",
    detail: payload.detail,
    bridgeAvailable: payload.bridge_available === true,
    services,
    missingServices,
    activeTaskId: typeof payload.active_task_id === "string" ? payload.active_task_id : null,
    activePhase: typeof payload.active_phase === "string" ? payload.active_phase as TaskOperationPhase : null,
    canStart: payload.can_start === true,
    canPause: payload.can_pause === true,
    canResume: payload.can_resume === true,
    canStop: payload.can_stop === true,
    canRecover: payload.can_recover === true,
    sensorDataChecked: false,
  };
};

export type { RtkCaptureStatus };
