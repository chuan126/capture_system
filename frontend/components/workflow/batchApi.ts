import { TaskApiError } from "./taskApi";
import type { CaptureBatch, CaptureBatchStatus } from "./batchModel";

type BatchApiResponse = {
  batch_id: string;
  batch_code: string;
  operation_date: string;
  daily_sequence: number;
  status: CaptureBatchStatus;
  created_at: string;
  started_at: string;
  completed_at: string | null;
  archived_at: string | null;
  purged_at: string | null;
  task_count: number;
  visible_task_count: number;
  measurement_bytes: number;
  report_id: string | null;
  report_path: string | null;
  report_sha256: string | null;
  report_generated_at: string | null;
  purged_bytes: number;
};

export type BatchPurgeResult = {
  batch: CaptureBatch;
  releasedBytes: number;
  removedTaskCount: number;
};

const statuses = new Set<CaptureBatchStatus>(["active", "completed", "archived", "purged"]);
const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const readString = (value: unknown, field: string) => {
  if (typeof value !== "string") throw new TaskApiError(`作业批次接口字段 ${field} 无效`);
  return value;
};
const readNullableString = (value: unknown, field: string) =>
  value === null ? null : readString(value, field);
const readInteger = (value: unknown, field: string) => {
  if (!Number.isInteger(value) || Number(value) < 0) {
    throw new TaskApiError(`作业批次接口字段 ${field} 无效`);
  }
  return Number(value);
};

const parseBatch = (value: unknown): CaptureBatch => {
  if (!isObject(value)) throw new TaskApiError("作业批次接口返回了无效对象");
  const status = readString(value.status, "status") as CaptureBatchStatus;
  if (!statuses.has(status)) throw new TaskApiError(`作业批次接口返回了未知状态 ${status}`);
  return {
    batchId: readString(value.batch_id, "batch_id"),
    batchCode: readString(value.batch_code, "batch_code"),
    operationDate: readString(value.operation_date, "operation_date"),
    dailySequence: readInteger(value.daily_sequence, "daily_sequence"),
    status,
    createdAt: readString(value.created_at, "created_at"),
    startedAt: readString(value.started_at, "started_at"),
    completedAt: readNullableString(value.completed_at, "completed_at"),
    archivedAt: readNullableString(value.archived_at, "archived_at"),
    purgedAt: readNullableString(value.purged_at, "purged_at"),
    taskCount: readInteger(value.task_count, "task_count"),
    visibleTaskCount: readInteger(value.visible_task_count, "visible_task_count"),
    measurementBytes: readInteger(value.measurement_bytes, "measurement_bytes"),
    reportId: readNullableString(value.report_id, "report_id"),
    reportPath: readNullableString(value.report_path, "report_path"),
    reportSha256: readNullableString(value.report_sha256, "report_sha256"),
    reportGeneratedAt: readNullableString(value.report_generated_at, "report_generated_at"),
    purgedBytes: readInteger(value.purged_bytes, "purged_bytes"),
  };
};

const errorMessage = async (response: Response) => {
  try {
    const payload = await response.json();
    if (isObject(payload) && typeof payload.detail === "string") return payload.detail;
  } catch {
    // 使用HTTP状态文本。
  }
  return response.statusText || `HTTP ${response.status}`;
};

const requestJson = async (url: string, init?: RequestInit): Promise<unknown> => {
  let response: Response;
  try {
    response = await fetch(url, { ...init, cache: "no-store" });
  } catch (error) {
    throw new TaskApiError(`无法连接作业批次接口：${error instanceof Error ? error.message : "网络请求失败"}`);
  }
  if (!response.ok) throw new TaskApiError(await errorMessage(response), response.status);
  return response.json();
};

export const listBatches = async (): Promise<CaptureBatch[]> => {
  const payload = await requestJson("/api/v1/batches", { headers: { Accept: "application/json" } });
  if (!Array.isArray(payload)) throw new TaskApiError("作业批次列表接口返回了无效数据");
  return payload.map(parseBatch);
};

export const createBatch = async (): Promise<CaptureBatch> =>
  parseBatch(await requestJson("/api/v1/batches", {
    method: "POST",
    headers: { Accept: "application/json" },
  }));

export const completeBatch = async (batchId: string): Promise<CaptureBatch> =>
  parseBatch(await requestJson(`/api/v1/batches/${encodeURIComponent(batchId)}/complete`, {
    method: "POST",
    headers: { Accept: "application/json" },
  }));

export const archiveBatch = async (batchId: string): Promise<CaptureBatch> =>
  parseBatch(await requestJson(`/api/v1/batches/${encodeURIComponent(batchId)}/archive`, {
    method: "POST",
    headers: { Accept: "application/json" },
  }));

export const purgeBatch = async (batchId: string): Promise<BatchPurgeResult> => {
  const payload = await requestJson(`/api/v1/batches/${encodeURIComponent(batchId)}/purge`, {
    method: "POST",
    headers: { Accept: "application/json" },
  });
  if (!isObject(payload)) throw new TaskApiError("批次清理接口返回了无效对象");
  return {
    batch: parseBatch(payload.batch),
    releasedBytes: readInteger(payload.released_bytes, "released_bytes"),
    removedTaskCount: readInteger(payload.removed_task_count, "removed_task_count"),
  };
};
