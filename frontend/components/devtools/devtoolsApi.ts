export type DevTopicTelemetry = {
  key: string;
  topic: string;
  message_type: string;
  received_count: number;
  rate_hz: number;
  last_received_ns: number | null;
  last_sensor_stamp_ns: number | null;
  age_ms: number | null;
  state: "waiting" | "stale" | "streaming";

  // PointCloud2 telemetry.
  point_count?: number;
  frame_id?: string;
  point_step?: number;
  data_bytes?: number;

  // ClearanceResult telemetry.
  valid?: boolean;
  lidar_to_top_m?: number | null;
  candidate_count?: number;
  selected_inlier_count?: number;
  selected_area_m2?: number | null;
  selected_tilt_deg?: number | null;
  residual_p95_m?: number | null;
  valid_point_ratio?: number | null;
  processing_time_ms?: number | null;
  invalid_reason?: string;

  // RTK telemetry.
  fix_status?: number;
  latitude?: number | null;
  longitude?: number | null;
  altitude?: number | null;
  satellite_count?: number;
  hdop?: number | null;
  pdop?: number | null;
  gps_state?: number;

  // TaskStatus / RecordingStatus telemetry.
  task_id?: string;
  status?: string;
  operation_phase?: string;
  status_revision?: number;
  message?: string;
  total_samples?: number;
  valid_samples?: number;
  invalid_samples?: number;
  recording_path?: string;
};

export type DevOverview = {
  build_variant: "development";
  version: string;
  emitted_at_ns: number;
  data_root: string;
  storage: { total_bytes: number; used_bytes: number; free_bytes: number };
  system: {
    cpu_percent: number | null; load_1m: number | null; memory_total_bytes: number; memory_available_bytes: number; memory_used_percent: number | null; soc_temperature_c: number | null; uptime_seconds: number | null;
  };
  telemetry: {
    bridge_available: boolean;
    bridge_error: string | null;
    emitted_at_ns: number;
    topics: Record<string, DevTopicTelemetry>;
  };
};

export type DevRecordingStatus = {
  active: boolean;
  profile: "raw_cloud" | "diagnostic" | "raw_sensor" | "algorithm_debug" | "full_debug" | null;
  recording_id: string | null;
  path: string | null;
  started_at_ns: number | null;
  elapsed_seconds: number;
  bytes: number;
  last_error: string | null;
  free_bytes: number;
  parameter_snapshot_complete: boolean | null;
};

export type DevRecording = {
  recording_id: string;
  profile: "raw_cloud" | "diagnostic" | "raw_sensor" | "algorithm_debug" | "full_debug";
  path: string;
  bytes: number;
  modified_at_ns: number;
  active: boolean;
  parameter_snapshot_complete: boolean | null;
};

export type DevParameter = {
  key: string;
  node: string;
  parameter: string;
  label: string;
  unit: string;
  kind: "float" | "int" | "bool";
  minimum: number | null;
  maximum: number | null;
  writable: boolean;
  note: string;
  config_available: boolean;
  configured_value: number | boolean | string | null;
  config_detail: string;
  available: boolean;
  value: number | boolean | string | null;
  detail: string;
  source_config: string;
  ui_visible: boolean;
};

const readError = async (response: Response) => {
  try {
    const payload = await response.json();
    if (payload && typeof payload.detail === "string") return payload.detail;
  } catch {}
  return response.statusText || `HTTP ${response.status}`;
};

const requestJson = async <T>(url: string, options?: RequestInit): Promise<T> => {
  let response: Response;
  try {
    response = await fetch(url, { cache: "no-store", ...options });
  } catch (error) {
    throw new Error(`开发接口连接失败：${error instanceof Error ? error.message : "网络异常"}`);
  }
  if (!response.ok) throw new Error(await readError(response));
  return await response.json() as T;
};

export const getDevOverview = () => requestJson<DevOverview>("/api/dev/overview");

export const captureRtkSnapshot = () => requestJson<{
  confirmed: boolean;
  detail: string;
  captured_at_ns: number;
  snapshot: Record<string, unknown> | null;
}>("/api/dev/rtk/snapshot");

export const getDevParameters = async () => {
  const response = await requestJson<{ parameters: DevParameter[] }>("/api/dev/parameters");
  return response.parameters;
};

export const setDevParameter = (key: string, value: number | boolean) =>
  requestJson<DevParameter>(`/api/dev/parameters/${encodeURIComponent(key)}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ value }),
  });

export const getDevRecordingStatus = () => requestJson<DevRecordingStatus>("/api/dev/recordings/status");
export const listDevRecordings = async () => {
  const response = await requestJson<{ recordings: DevRecording[] }>("/api/dev/recordings");
  return response.recordings;
};
export type DevRecordingRouteProfile = "raw-cloud" | "diagnostic" | "raw-sensor" | "algorithm-debug" | "full-debug";
export const startDevRecording = (profile: DevRecordingRouteProfile, durationSeconds: 5 | 10 | 30 | null) =>
  requestJson<DevRecordingStatus>(`/api/dev/recordings/${profile}/start`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ duration_seconds: durationSeconds }),
  });
export const stopDevRecording = () => requestJson<DevRecordingStatus>("/api/dev/recordings/stop", { method: "POST" });
export const deleteDevRecording = async (recordingId: string) => {
  const response = await fetch(`/api/dev/recordings/${encodeURIComponent(recordingId)}`, { method: "DELETE" });
  if (!response.ok) throw new Error(await readError(response));
};
