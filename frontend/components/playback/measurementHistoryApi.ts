import type { ClearanceSample } from "./InteractiveClearanceChart";
import { TaskApiError } from "../workflow/taskApi";
import { formatLaneDisplay } from "../workflow/taskModel";
import type { CollectionTaskLaneDisplay } from "../workflow/taskModel";

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

export type MeasurementSummary = {
  taskId: string;
  recordingSchemaVersion: number;
  dataOrigin: "recorded" | "test_fixture";
  lane: CollectionTaskLaneDisplay | null;
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
  firstSampleIndex: number;
  lastSampleIndex: number;
  firstTimestampMs: number;
  lastTimestampMs: number;
};

export type MeasurementSeries = {
  taskId: string;
  domainStartTimestampMs: number;
  domainEndTimestampMs: number;
  requestedStartTimestampMs: number;
  requestedEndTimestampMs: number;
  sourceSampleCount: number;
  returnedSampleCount: number;
  downsampled: boolean;
  samples: ClearanceSample[];
};

export type MeasurementSeriesRequest = {
  startTimestampMs?: number;
  endTimestampMs?: number;
  maxPoints?: number;
  signal?: AbortSignal;
};

export type MeasurementPrefixRequest = {
  maxSamples?: number;
  signal?: AbortSignal;
};

const PLAYBACK_SESSION_KEY = "capture.playback.session.v1";
let inMemoryPlaybackSession: string | null = null;

const createPlaybackSession = (): string => {
  if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
    return crypto.randomUUID();
  }
  return `playback-${Date.now()}-${Math.random().toString(16).slice(2)}`;
};

export const getPlaybackSessionId = (): string => {
  if (typeof window === "undefined") {
    inMemoryPlaybackSession ??= createPlaybackSession();
    return inMemoryPlaybackSession;
  }
  try {
    const existing = window.sessionStorage.getItem(PLAYBACK_SESSION_KEY);
    if (existing) return existing;
    const created = createPlaybackSession();
    window.sessionStorage.setItem(PLAYBACK_SESSION_KEY, created);
    return created;
  } catch {
    inMemoryPlaybackSession ??= createPlaybackSession();
    return inMemoryPlaybackSession;
  }
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

const readStatistics = (value: unknown): MeasurementStatistics => {
  if (!isObject(value)) throw new TaskApiError("历史记录字段 statistics 无效");
  return {
    totalSamples: readNumber(value.total_samples, "statistics.total_samples"),
    validSamples: readNumber(value.valid_samples, "statistics.valid_samples"),
    invalidSamples: readNumber(value.invalid_samples, "statistics.invalid_samples"),
    minimumHeightM: readNullableNumber(value.minimum_height_m, "statistics.minimum_height_m"),
    averageHeightM: readNullableNumber(value.average_height_m, "statistics.average_height_m"),
    maximumHeightM: readNullableNumber(value.maximum_height_m, "statistics.maximum_height_m"),
    durationMs: readNumber(value.duration_ms, "statistics.duration_ms"),
    nominalSampleRateHz: readNumber(value.nominal_sample_rate_hz, "statistics.nominal_sample_rate_hz"),
    actualAverageSampleRateHz: readNullableNumber(
      value.actual_average_sample_rate_hz,
      "statistics.actual_average_sample_rate_hz",
    ),
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

const fetchJson = async (url: string, signal?: AbortSignal): Promise<unknown> => {
  let response: Response;
  try {
    response = await fetch(url, {
      method: "GET",
      headers: {
        Accept: "application/json",
        "X-Playback-Session": getPlaybackSessionId(),
      },
      cache: "no-store",
      signal,
    });
  } catch (error) {
    if (error instanceof DOMException && error.name === "AbortError") throw error;
    const detail = error instanceof Error ? error.message : "网络请求失败";
    throw new TaskApiError(`无法连接历史记录接口：${detail}`);
  }
  if (!response.ok) throw new TaskApiError(await readErrorMessage(response), response.status);
  try {
    return await response.json();
  } catch {
    throw new TaskApiError("历史记录接口返回了无效JSON", response.status);
  }
};

export const loadMeasurementSummary = async (
  taskId: string,
  signal?: AbortSignal,
): Promise<MeasurementSummary> => {
  const payload = await fetchJson(
    `/api/v1/tasks/${encodeURIComponent(taskId)}/measurements/summary`,
    signal,
  );
  if (!isObject(payload)) throw new TaskApiError("历史记录接口返回格式无效");
  const dataOrigin = readString(payload.data_origin, "data_origin");
  if (dataOrigin !== "recorded" && dataOrigin !== "test_fixture") {
    throw new TaskApiError("历史记录字段 data_origin 无效");
  }
  const rawLane = readString(payload.lane, "lane");
  const travelDirection = readString(payload.travel_direction, "travel_direction");
  const laneSide = readString(payload.lane_side, "lane_side");
  return {
    taskId: readString(payload.task_id, "task_id"),
    recordingSchemaVersion: readNumber(payload.recording_schema_version, "recording_schema_version"),
    dataOrigin,
    lane: formatLaneDisplay(travelDirection, laneSide, rawLane),
    startedAt: readString(payload.started_at, "started_at"),
    endedAt: readNullableString(payload.ended_at, "ended_at"),
    complete: readBoolean(payload.complete, "complete"),
    algorithmVersion: readNullableString(payload.algorithm_version, "algorithm_version"),
    configVersion: readNullableString(payload.config_version, "config_version"),
    softwareVersion: readNullableString(payload.software_version, "software_version"),
    statistics: readStatistics(payload.statistics),
    entryRtk: readRtk(payload.entry_rtk, "entry_rtk"),
    exitRtk: readRtk(payload.exit_rtk, "exit_rtk"),
    pauseIntervalCount: readNumber(payload.pause_interval_count, "pause_interval_count"),
    firstSampleIndex: readNumber(payload.first_sample_index, "first_sample_index"),
    lastSampleIndex: readNumber(payload.last_sample_index, "last_sample_index"),
    firstTimestampMs: readNumber(payload.first_timestamp_ms, "first_timestamp_ms"),
    lastTimestampMs: readNumber(payload.last_timestamp_ms, "last_timestamp_ms"),
  };
};

export const loadMeasurementSeries = async (
  taskId: string,
  request: MeasurementSeriesRequest = {},
): Promise<MeasurementSeries> => {
  const params = new URLSearchParams();
  if (request.startTimestampMs !== undefined) {
    params.set("start_timestamp_ms", String(Math.floor(request.startTimestampMs)));
  }
  if (request.endTimestampMs !== undefined) {
    params.set("end_timestamp_ms", String(Math.ceil(request.endTimestampMs)));
  }
  params.set("max_points", String(request.maxPoints ?? 4000));
  const payload = await fetchJson(
    `/api/v1/tasks/${encodeURIComponent(taskId)}/measurements/series?${params.toString()}`,
    request.signal,
  );
  if (!isObject(payload) || !Array.isArray(payload.samples)) {
    throw new TaskApiError("历史曲线接口返回格式无效");
  }
  const samples = payload.samples.map((raw, index): ClearanceSample => {
    if (!isObject(raw)) throw new TaskApiError(`历史曲线样本 ${index} 无效`);
    const valid = readBoolean(raw.valid, `samples[${index}].valid`);
    return {
      sampleIndex: readNumber(raw.sample_index, `samples[${index}].sample_index`),
      timestampMs: readNumber(raw.timestamp_ms, `samples[${index}].timestamp_ms`),
      elapsedMs: readNumber(raw.elapsed_ms, `samples[${index}].elapsed_ms`),
      heightM: readNullableNumber(raw.height_m, `samples[${index}].height_m`),
      valid,
      reason: readNullableString(raw.invalid_reason, `samples[${index}].invalid_reason`) ?? undefined,
    };
  });
  return {
    taskId: readString(payload.task_id, "task_id"),
    domainStartTimestampMs: readNumber(payload.domain_start_timestamp_ms, "domain_start_timestamp_ms"),
    domainEndTimestampMs: readNumber(payload.domain_end_timestamp_ms, "domain_end_timestamp_ms"),
    requestedStartTimestampMs: readNumber(
      payload.requested_start_timestamp_ms,
      "requested_start_timestamp_ms",
    ),
    requestedEndTimestampMs: readNumber(
      payload.requested_end_timestamp_ms,
      "requested_end_timestamp_ms",
    ),
    sourceSampleCount: readNumber(payload.source_sample_count, "source_sample_count"),
    returnedSampleCount: readNumber(payload.returned_sample_count, "returned_sample_count"),
    downsampled: readBoolean(payload.downsampled, "downsampled"),
    samples,
  };
};

export const loadMeasurementPrefix = async (
  taskId: string,
  request: MeasurementPrefixRequest = {},
): Promise<MeasurementSeries> => {
  const params = new URLSearchParams();
  params.set("max_samples", String(request.maxSamples ?? 2000));
  const payload = await fetchJson(
    `/api/v1/tasks/${encodeURIComponent(taskId)}/measurements/series-prefix?${params.toString()}`,
    request.signal,
  );
  if (!isObject(payload) || !Array.isArray(payload.samples)) {
    throw new TaskApiError("历史曲线首段接口返回格式无效");
  }
  const samples = payload.samples.map((raw, index): ClearanceSample => {
    if (!isObject(raw)) throw new TaskApiError(`历史曲线首段样本 ${index} 无效`);
    const valid = readBoolean(raw.valid, `samples[${index}].valid`);
    return {
      sampleIndex: readNumber(raw.sample_index, `samples[${index}].sample_index`),
      timestampMs: readNumber(raw.timestamp_ms, `samples[${index}].timestamp_ms`),
      elapsedMs: readNumber(raw.elapsed_ms, `samples[${index}].elapsed_ms`),
      heightM: readNullableNumber(raw.height_m, `samples[${index}].height_m`),
      valid,
      reason: readNullableString(raw.invalid_reason, `samples[${index}].invalid_reason`) ?? undefined,
    };
  });
  return {
    taskId: readString(payload.task_id, "task_id"),
    domainStartTimestampMs: readNumber(payload.domain_start_timestamp_ms, "domain_start_timestamp_ms"),
    domainEndTimestampMs: readNumber(payload.domain_end_timestamp_ms, "domain_end_timestamp_ms"),
    requestedStartTimestampMs: readNumber(payload.requested_start_timestamp_ms, "requested_start_timestamp_ms"),
    requestedEndTimestampMs: readNumber(payload.requested_end_timestamp_ms, "requested_end_timestamp_ms"),
    sourceSampleCount: readNumber(payload.source_sample_count, "source_sample_count"),
    returnedSampleCount: readNumber(payload.returned_sample_count, "returned_sample_count"),
    downsampled: readBoolean(payload.downsampled, "downsampled"),
    samples,
  };
};
