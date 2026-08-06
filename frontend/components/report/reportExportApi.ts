import { TaskApiError } from "../workflow/taskApi";
import type { CollectionTaskLane } from "../workflow/taskModel";

export type ReportRtkEndpoint = {
  latitudeDeg: number;
  longitudeDeg: number;
  altitudeM: number | null;
  valid: boolean;
};

export type ReportTaskPreview = {
  taskId: string;
  sequence: number;
  tunnelCode: string;
  tunnelName: string;
  status: "pending" | "running" | "paused" | "completed" | "interrupted" | "failed";
  exportable: boolean;
  blockedReason: string | null;
  dataOrigin: "recorded" | "test_fixture" | null;
  lane: CollectionTaskLane | null;
  startedAt: string | null;
  endedAt: string | null;
  complete: boolean | null;
  totalSamples: number | null;
  validSamples: number | null;
  invalidSamples: number | null;
  minimumHeightM: number | null;
  entryRtk: ReportRtkEndpoint | null;
  exitRtk: ReportRtkEndpoint | null;
};

export type ReportPreview = {
  taskCount: number;
  exportableTaskCount: number;
  generatedAt: string;
  tasks: ReportTaskPreview[];
};

export type GeneratedExportFile = {
  exportFormat: "txt" | "pdf";
  fileName: string;
  fileSizeBytes: number;
  generatedAt: string;
  downloadUrl: string;
  reportId: string | null;
  taskId: string | null;
  includedTaskCount: number | null;
};

const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const readString = (value: unknown, field: string): string => {
  if (typeof value !== "string") throw new TaskApiError(`导出接口字段 ${field} 无效`);
  return value;
};

const readNullableString = (value: unknown, field: string): string | null =>
  value === null ? null : readString(value, field);

const readNumber = (value: unknown, field: string): number => {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new TaskApiError(`导出接口字段 ${field} 无效`);
  }
  return value;
};

const readNullableNumber = (value: unknown, field: string): number | null =>
  value === null ? null : readNumber(value, field);

const readBoolean = (value: unknown, field: string): boolean => {
  if (typeof value !== "boolean") throw new TaskApiError(`导出接口字段 ${field} 无效`);
  return value;
};

const readNullableBoolean = (value: unknown, field: string): boolean | null =>
  value === null ? null : readBoolean(value, field);

const readErrorMessage = async (response: Response): Promise<string> => {
  try {
    const payload = await response.json();
    if (isObject(payload) && typeof payload.detail === "string") return payload.detail;
  } catch {
    // 非 JSON 错误响应使用状态文本。
  }
  return response.statusText || `HTTP ${response.status}`;
};

const requestJson = async (url: string, init: RequestInit): Promise<unknown> => {
  let response: Response;
  try {
    response = await fetch(url, { ...init, cache: "no-store" });
  } catch (error) {
    const detail = error instanceof Error ? error.message : "网络请求失败";
    throw new TaskApiError(`无法连接导出接口：${detail}`);
  }
  if (!response.ok) throw new TaskApiError(await readErrorMessage(response), response.status);
  try {
    return await response.json();
  } catch {
    throw new TaskApiError("导出接口返回了无效 JSON", response.status);
  }
};

const readRtk = (value: unknown, field: string): ReportRtkEndpoint | null => {
  if (value === null) return null;
  if (!isObject(value)) throw new TaskApiError(`导出接口字段 ${field} 无效`);
  return {
    latitudeDeg: readNumber(value.latitude_deg, `${field}.latitude_deg`),
    longitudeDeg: readNumber(value.longitude_deg, `${field}.longitude_deg`),
    altitudeM: readNullableNumber(value.altitude_m, `${field}.altitude_m`),
    valid: readBoolean(value.valid, `${field}.valid`),
  };
};

const readLane = (value: unknown): CollectionTaskLane | null => {
  if (value === null || value === "unknown") return null;
  if (value === "left") return "左车道";
  if (value === "right") return "右车道";
  throw new TaskApiError("导出接口字段 lane 无效");
};

const readTaskPreview = (value: unknown, index: number): ReportTaskPreview => {
  if (!isObject(value)) throw new TaskApiError(`导出预览任务 ${index} 无效`);
  const status = readString(value.status, `tasks.${index}.status`);
  if (!["pending", "running", "paused", "completed", "interrupted", "failed"].includes(status)) {
    throw new TaskApiError(`导出预览任务状态 ${status} 无效`);
  }
  const dataOrigin = readNullableString(value.data_origin, `tasks.${index}.data_origin`);
  if (dataOrigin !== null && dataOrigin !== "recorded" && dataOrigin !== "test_fixture") {
    throw new TaskApiError(`导出预览数据来源 ${dataOrigin} 无效`);
  }
  return {
    taskId: readString(value.task_id, `tasks.${index}.task_id`),
    sequence: readNumber(value.sequence, `tasks.${index}.sequence`),
    tunnelCode: readString(value.tunnel_code, `tasks.${index}.tunnel_code`),
    tunnelName: readString(value.tunnel_name, `tasks.${index}.tunnel_name`),
    status: status as ReportTaskPreview["status"],
    exportable: readBoolean(value.exportable, `tasks.${index}.exportable`),
    blockedReason: readNullableString(value.blocked_reason, `tasks.${index}.blocked_reason`),
    dataOrigin: dataOrigin as ReportTaskPreview["dataOrigin"],
    lane: readLane(value.lane),
    startedAt: readNullableString(value.started_at, `tasks.${index}.started_at`),
    endedAt: readNullableString(value.ended_at, `tasks.${index}.ended_at`),
    complete: readNullableBoolean(value.complete, `tasks.${index}.complete`),
    totalSamples: readNullableNumber(value.total_samples, `tasks.${index}.total_samples`),
    validSamples: readNullableNumber(value.valid_samples, `tasks.${index}.valid_samples`),
    invalidSamples: readNullableNumber(value.invalid_samples, `tasks.${index}.invalid_samples`),
    minimumHeightM: readNullableNumber(value.minimum_height_m, `tasks.${index}.minimum_height_m`),
    entryRtk: readRtk(value.entry_rtk, `tasks.${index}.entry_rtk`),
    exitRtk: readRtk(value.exit_rtk, `tasks.${index}.exit_rtk`),
  };
};

const readGeneratedFile = (value: unknown): GeneratedExportFile => {
  if (!isObject(value)) throw new TaskApiError("导出文件响应无效");
  const exportFormat = readString(value.export_format, "export_format");
  if (exportFormat !== "txt" && exportFormat !== "pdf") {
    throw new TaskApiError(`导出格式 ${exportFormat} 无效`);
  }
  return {
    exportFormat,
    fileName: readString(value.file_name, "file_name"),
    fileSizeBytes: readNumber(value.file_size_bytes, "file_size_bytes"),
    generatedAt: readString(value.generated_at, "generated_at"),
    downloadUrl: readString(value.download_url, "download_url"),
    reportId: readNullableString(value.report_id, "report_id"),
    taskId: readNullableString(value.task_id, "task_id"),
    includedTaskCount: readNullableNumber(value.included_task_count, "included_task_count"),
  };
};

export const loadReportPreview = async (): Promise<ReportPreview> => {
  const payload = await requestJson("/api/v1/reports/clearance-summary/preview", {
    method: "GET",
    headers: { Accept: "application/json" },
  });
  if (!isObject(payload) || !Array.isArray(payload.tasks)) {
    throw new TaskApiError("导出预览接口返回了无效对象");
  }
  return {
    taskCount: readNumber(payload.task_count, "task_count"),
    exportableTaskCount: readNumber(payload.exportable_task_count, "exportable_task_count"),
    generatedAt: readString(payload.generated_at, "generated_at"),
    tasks: payload.tasks.map(readTaskPreview),
  };
};

export const generateTaskTxt = async (taskId: string): Promise<GeneratedExportFile> =>
  readGeneratedFile(await requestJson(`/api/v1/tasks/${encodeURIComponent(taskId)}/exports/txt`, {
    method: "POST",
    headers: { Accept: "application/json" },
  }));

export const generateSummaryPdf = async (): Promise<GeneratedExportFile> =>
  readGeneratedFile(await requestJson("/api/v1/reports/clearance-summary", {
    method: "POST",
    headers: { Accept: "application/json" },
  }));

export const downloadGeneratedFile = async (file: GeneratedExportFile): Promise<void> => {
  const anchor = document.createElement("a");
  anchor.href = file.downloadUrl;
  anchor.download = file.fileName;
  anchor.rel = "noopener";
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
};
