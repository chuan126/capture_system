import type { ClearanceSample } from "./InteractiveClearanceChart";
import { TaskApiError } from "../workflow/taskApi";
import type { CollectionTaskLane } from "../workflow/taskModel";

export type MeasurementRtkEndpoint = {
  timestampMs: number;
  latitudeDeg: number;
  longitudeDeg: number;
  altitudeM: number | null;
  fixType: string;
  valid: boolean;
};

export type MeasurementStatistics = {
  totalSamples: number;
  validSamples: number;
  invalidSamples: number;
  minimumHeightM: number | null;
  averageHeightM: number | null;
  maximumHeightM: number | null;
  durationMs: number;
  nominalSampleRateHz: number;
  actualAverageSampleRateHz: number | null;
};

export type MeasurementHistory = {
  taskId: string;
  recordingSchemaVersion: number;
  dataOrigin: "recorded" | "test_fixture";
  lane: CollectionTaskLane | null;
  startedAt: string;
  endedAt: string | null;
  complete: boolean;
  algorithmVersion: string | null;
  configVersion: string | null;
  softwareVersion: string | null;
  statistics: MeasurementStatistics;
  entryRtk: MeasurementRtkEndpoint | null;
  exitRtk: MeasurementRtkEndpoint | null;
  pauseIntervalCount: number;
  samples: ClearanceSample[];
};

const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const readString = (value: unknown, field: string): string => {
  if (typeof value !== "string") throw new TaskApiError(`历史记录字段 ${field} 无效`);
  return value;
};

const readNullableString = (value: unknown, field: string): string | null => {
  if (value === null) return null;
  return readString(value, field);
};

const readNumber = (value: unknown, field: string): number => {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new TaskApiError(`历史记录字段 ${field} 无效`);
  }
  return value;
};

const readNullableNumber = (value: unknown, field: string): number | null => {
  if (value === null) return null;
  return readNumber(value, field);
};

const readBoolean = (value: unknown, field: string): boolean => {
  if (typeof value !== "boolean") throw new TaskApiError(`历史记录字段 ${field} 无效`);
  return value;
};

const readRtk = (value: unknown, field: string): MeasurementRtkEndpoint | null => {
  if (value === null) return null;
  if (!isObject(value)) throw new TaskApiError(`历史记录字段 ${field} 无效`);
  return {
    timestampMs: readNumber(value.timestamp_ms, `${field}.timestamp_ms`),
    latitudeDeg: readNumber(value.latitude_deg, `${field}.latitude_deg`),
    longitudeDeg: readNumber(value.longitude_deg, `${field}.longitude_deg`),
    altitudeM: readNullableNumber(value.altitude_m, `${field}.altitude_m`),
    fixType: readString(value.fix_type, `${field}.fix_type`),
    valid: readBoolean(value.valid, `${field}.valid`),
  };
};

const readErrorMessage = async (response: Response): Promise<string> => {
  try {
    const payload = await response.json();
    if (isObject(payload) && typeof payload.detail === "string") return payload.detail;
  } catch {
    // 非JSON错误响应使用状态文本。
  }
  return response.statusText || `HTTP ${response.status}`;
};

export const loadMeasurementHistory = async (taskId: string): Promise<MeasurementHistory> => {
  let response: Response;
  try {
    response = await fetch(`/api/v1/tasks/${encodeURIComponent(taskId)}/measurements`, {
      method: "GET",
      headers: { Accept: "application/json" },
      cache: "no-store",
    });
  } catch (error) {
    const detail = error instanceof Error ? error.message : "网络请求失败";
    throw new TaskApiError(`无法连接历史记录接口：${detail}`);
  }
  if (!response.ok) throw new TaskApiError(await readErrorMessage(response), response.status);

  let payload: unknown;
  try {
    payload = await response.json();
  } catch {
    throw new TaskApiError("历史记录接口返回了无效JSON", response.status);
  }
  if (!isObject(payload)) throw new TaskApiError("历史记录接口返回了无效对象");
  if (!isObject(payload.statistics)) throw new TaskApiError("历史记录统计字段无效");
  if (!Array.isArray(payload.pause_intervals)) throw new TaskApiError("历史记录暂停区间字段无效");
  if (!Array.isArray(payload.samples)) throw new TaskApiError("历史记录样本字段无效");

  const dataOrigin = readString(payload.data_origin, "data_origin");
  if (dataOrigin !== "recorded" && dataOrigin !== "test_fixture") {
    throw new TaskApiError(`历史记录数据来源无效 ${dataOrigin}`);
  }
  const laneValue = readString(payload.lane, "lane");
  const lane = laneValue === "left" ? "左车道" : laneValue === "right" ? "右车道" : null;

  return {
    taskId: readString(payload.task_id, "task_id"),
    recordingSchemaVersion: readNumber(payload.recording_schema_version, "recording_schema_version"),
    dataOrigin,
    lane,
    startedAt: readString(payload.started_at, "started_at"),
    endedAt: readNullableString(payload.ended_at, "ended_at"),
    complete: readBoolean(payload.complete, "complete"),
    algorithmVersion: readNullableString(payload.algorithm_version, "algorithm_version"),
    configVersion: readNullableString(payload.config_version, "config_version"),
    softwareVersion: readNullableString(payload.software_version, "software_version"),
    statistics: {
      totalSamples: readNumber(payload.statistics.total_samples, "statistics.total_samples"),
      validSamples: readNumber(payload.statistics.valid_samples, "statistics.valid_samples"),
      invalidSamples: readNumber(payload.statistics.invalid_samples, "statistics.invalid_samples"),
      minimumHeightM: readNullableNumber(payload.statistics.minimum_height_m, "statistics.minimum_height_m"),
      averageHeightM: readNullableNumber(payload.statistics.average_height_m, "statistics.average_height_m"),
      maximumHeightM: readNullableNumber(payload.statistics.maximum_height_m, "statistics.maximum_height_m"),
      durationMs: readNumber(payload.statistics.duration_ms, "statistics.duration_ms"),
      nominalSampleRateHz: readNumber(payload.statistics.nominal_sample_rate_hz, "statistics.nominal_sample_rate_hz"),
      actualAverageSampleRateHz: readNullableNumber(
        payload.statistics.actual_average_sample_rate_hz,
        "statistics.actual_average_sample_rate_hz",
      ),
    },
    entryRtk: readRtk(payload.entry_rtk, "entry_rtk"),
    exitRtk: readRtk(payload.exit_rtk, "exit_rtk"),
    pauseIntervalCount: payload.pause_intervals.length,
    samples: payload.samples.map((sample, index) => {
      if (!isObject(sample)) throw new TaskApiError(`历史记录样本 ${index} 无效`);
      return {
        timestampMs: readNumber(sample.timestamp_ms, `samples.${index}.timestamp_ms`),
        heightM: readNullableNumber(sample.height_m, `samples.${index}.height_m`),
        valid: readBoolean(sample.valid, `samples.${index}.valid`),
        reason: readNullableString(sample.invalid_reason, `samples.${index}.invalid_reason`) ?? undefined,
      };
    }),
  };
};
