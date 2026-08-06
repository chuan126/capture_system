import type { CollectionTask } from "@/components/workflow/taskModel";

export type TaskCreateDraft = {
  tunnelCode: string;
  tunnelName: string;
};

type TaskApiStatus =
  | "pending"
  | "running"
  | "paused"
  | "completed"
  | "interrupted"
  | "failed";

type TaskApiResponse = {
  task_id: string;
  sequence: number;
  display_sequence: string;
  tunnel_code: string;
  tunnel_name: string;
  status: TaskApiStatus;
  created_at: string;
  updated_at: string;
  started_at: string | null;
  completed_at: string | null;
  has_measurements: boolean;
  recording_path: string | null;
  schema_version: number;
};

const statusLabels: Record<TaskApiStatus, CollectionTask["status"]> = {
  pending: "待执行",
  running: "采集中",
  paused: "已暂停",
  completed: "已停止",
  interrupted: "异常中断",
  failed: "失败",
};

export class TaskApiError extends Error {
  readonly status: number | null;

  constructor(message: string, status: number | null = null) {
    super(message);
    this.name = "TaskApiError";
    this.status = status;
  }
}

const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const readString = (value: unknown, field: string): string => {
  if (typeof value !== "string") throw new TaskApiError(`任务接口字段 ${field} 无效`);
  return value;
};

const readNullableString = (value: unknown, field: string): string | null => {
  if (value === null) return null;
  return readString(value, field);
};

const parseTask = (value: unknown): CollectionTask => {
  if (!isObject(value)) throw new TaskApiError("任务接口返回了无效对象");
  const status = readString(value.status, "status") as TaskApiStatus;
  if (!(status in statusLabels)) throw new TaskApiError(`任务接口返回了未知状态 ${status}`);
  if (!Number.isInteger(value.sequence) || Number(value.sequence) <= 0) {
    throw new TaskApiError("任务接口字段 sequence 无效");
  }
  if (typeof value.has_measurements !== "boolean") {
    throw new TaskApiError("任务接口字段 has_measurements 无效");
  }
  if (!Number.isInteger(value.schema_version) || Number(value.schema_version) <= 0) {
    throw new TaskApiError("任务接口字段 schema_version 无效");
  }

  return {
    taskId: readString(value.task_id, "task_id"),
    sequence: Number(value.sequence),
    tunnelCode: readString(value.tunnel_code, "tunnel_code"),
    tunnelName: readString(value.tunnel_name, "tunnel_name"),
    status: statusLabels[status],
    lane: null,
    createdAt: readString(value.created_at, "created_at"),
    updatedAt: readString(value.updated_at, "updated_at"),
    startedAt: readNullableString(value.started_at, "started_at"),
    completedAt: readNullableString(value.completed_at, "completed_at"),
    hasMeasurements: value.has_measurements,
    recordingPath: readNullableString(value.recording_path, "recording_path"),
    schemaVersion: Number(value.schema_version),
  };
};

const readErrorMessage = async (response: Response): Promise<string> => {
  try {
    const payload = await response.json();
    if (isObject(payload) && typeof payload.detail === "string") return payload.detail;
  } catch {
    // 非JSON错误响应使用统一状态文本。
  }
  return response.statusText || `HTTP ${response.status}`;
};

const requestJson = async (input: RequestInfo | URL, init?: RequestInit): Promise<unknown> => {
  let response: Response;
  try {
    response = await fetch(input, init);
  } catch (error) {
    const detail = error instanceof Error ? error.message : "网络请求失败";
    throw new TaskApiError(`无法连接任务接口：${detail}`);
  }
  if (!response.ok) {
    throw new TaskApiError(await readErrorMessage(response), response.status);
  }
  try {
    return await response.json();
  } catch {
    throw new TaskApiError("任务接口返回了无效JSON", response.status);
  }
};

export const listTasks = async (): Promise<CollectionTask[]> => {
  const pageSize = 500;
  const tasks: CollectionTask[] = [];
  for (let offset = 0; ; offset += pageSize) {
    const payload = await requestJson(
      `/api/v1/tasks?limit=${pageSize}&offset=${offset}&order=asc`,
      {
        method: "GET",
        headers: { Accept: "application/json" },
        cache: "no-store",
      },
    );
    if (!Array.isArray(payload)) throw new TaskApiError("任务列表接口返回了无效数据");
    tasks.push(...payload.map(parseTask));
    if (payload.length < pageSize) return tasks;
  }
};

export const createTaskBatch = async (
  drafts: TaskCreateDraft[],
  idempotencyKey: string,
): Promise<CollectionTask[]> => {
  const payload = await requestJson("/api/v1/tasks/batch", {
    method: "POST",
    headers: {
      Accept: "application/json",
      "Content-Type": "application/json",
      "Idempotency-Key": idempotencyKey,
    },
    body: JSON.stringify({
      tasks: drafts.map((draft) => ({
        tunnel_code: draft.tunnelCode,
        tunnel_name: draft.tunnelName,
      })),
    }),
  });
  if (!Array.isArray(payload)) throw new TaskApiError("任务创建接口返回了无效数据");
  return payload.map(parseTask);
};

export const deleteTask = async (taskId: string): Promise<void> => {
  let response: Response;
  try {
    response = await fetch(`/api/v1/tasks/${encodeURIComponent(taskId)}`, {
      method: "DELETE",
      headers: { Accept: "application/json" },
    });
  } catch (error) {
    const detail = error instanceof Error ? error.message : "网络请求失败";
    throw new TaskApiError(`无法连接任务接口：${detail}`);
  }
  if (!response.ok) {
    throw new TaskApiError(await readErrorMessage(response), response.status);
  }
};
