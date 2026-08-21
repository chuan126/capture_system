export type ClearanceStatusMessage = {
  type: "status";
  state: "waiting" | "streaming" | "degraded" | "ros_unavailable";
  reason: string;
  detail: string;
};

export type ClearanceSnapshot = {
  type: "clearance_snapshot";
  sequence: number;
  emitted_at_ns: number;
  stamp_ns: number;
  frame_id: string;
  valid: boolean;
  lidar_to_top_m: number | null;
  ransac_plane_count: number;
  surface_count: number;
  candidate_count: number;
  selected_inlier_count: number;
  selected_area_m2: number | null;
  selected_tilt_deg: number | null;
  residual_median_m: number | null;
  residual_p95_m: number | null;
  minimum_position_east_m: number | null;
  minimum_position_north_m: number | null;
  minimum_position_up_m: number | null;
  valid_point_ratio: number | null;
  invalid_reason: string;
  processing_time_ms: number | null;
};

export type ClearanceTextMessage = ClearanceStatusMessage | ClearanceSnapshot;

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

function isNullableNumber(value: unknown): value is number | null {
  return value === null || isFiniteNumber(value);
}

export function parseClearanceText(text: string): ClearanceTextMessage {
  const value: unknown = JSON.parse(text);
  if (!isObject(value) || typeof value.type !== "string") {
    throw new Error("净空消息不是有效对象");
  }
  if (value.type === "status") {
    if (
      !["waiting", "streaming", "degraded", "ros_unavailable"].includes(String(value.state)) ||
      typeof value.reason !== "string" ||
      typeof value.detail !== "string"
    ) {
      throw new Error("净空状态消息字段无效");
    }
    return value as ClearanceStatusMessage;
  }
  if (value.type !== "clearance_snapshot") {
    throw new Error(`未知净空消息类型：${value.type}`);
  }

  const integerFields = ["sequence", "stamp_ns", "ransac_plane_count", "surface_count", "candidate_count", "selected_inlier_count"] as const;
  const nullableFields = [
    "lidar_to_top_m",
    "selected_area_m2",
    "selected_tilt_deg",
    "residual_median_m",
    "residual_p95_m",
    "minimum_position_east_m",
    "minimum_position_north_m",
    "minimum_position_up_m",
    "valid_point_ratio",
    "processing_time_ms",
  ] as const;
  if (
    integerFields.some((field) => !isFiniteNumber(value[field])) ||
    !isFiniteNumber(value.emitted_at_ns) ||
    nullableFields.some((field) => !isNullableNumber(value[field])) ||
    typeof value.frame_id !== "string" ||
    typeof value.valid !== "boolean" ||
    typeof value.invalid_reason !== "string"
  ) {
    throw new Error("净空快照字段无效");
  }
  return value as ClearanceSnapshot;
}
