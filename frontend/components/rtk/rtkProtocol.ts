export type RtkStatusMessage = {
  type: "status";
  state: "waiting" | "streaming" | "degraded" | "ros_unavailable";
  reason: string;
  detail: string;
};

export type RtkSnapshot = {
  type: "rtk_snapshot";
  sequence: number;
  emitted_at_ns: number;
  serial_connected: boolean | null;
  serial_message: string;
  status_stamp_ns: number | null;
  event_mask: number | null;
  rmc_validity: number | null;
  gps_state: number | null;
  satellite_count: number | null;
  hdop: number | null;
  pdop: number | null;
  latitude_sigma: number | null;
  longitude_sigma: number | null;
  height_sigma: number | null;
  speed_knots: number | null;
  track_degrees: number | null;
  fix_stamp_ns: number | null;
  fix_status: number | null;
  latitude: number | null;
  longitude: number | null;
  altitude: number | null;
  localization_stamp_ns: number | null;
  localization_valid: boolean | null;
  localization_mode: number | null;
  localization_heading_source: number | null;
  localization_latitude: number | null;
  localization_longitude: number | null;
  localization_altitude: number | null;
  localization_heading_deg: number | null;
  localization_heading_alignment_valid: boolean | null;
  localization_delta_yaw_deg: number | null;
  localization_scale_calibration_enabled: boolean | null;
  localization_scale_valid: boolean | null;
  localization_horizontal_scale: number | null;
  localization_vertical_scale: number | null;
  localization_scale_baseline_m: number | null;
  localization_heading_baseline_m: number | null;
  localization_distance_from_anchor_m: number | null;
  localization_dr_duration_s: number | null;
  localization_rtk_age_s: number | null;
  localization_odometry_age_s: number | null;
  localization_imu_age_s: number | null;
  localization_position_difference_to_rtk_m: number | null;
  localization_invalid_reason: string | null;
};

export type RtkTextMessage = RtkStatusMessage | RtkSnapshot;

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

function isNullableNumber(value: unknown): value is number | null {
  return value === null || (typeof value === "number" && Number.isFinite(value));
}

export function parseRtkText(text: string): RtkTextMessage {
  const value: unknown = JSON.parse(text);
  if (!isObject(value) || typeof value.type !== "string") {
    throw new Error("RTK消息不是有效对象");
  }

  if (value.type === "status") {
    if (
      !["waiting", "streaming", "degraded", "ros_unavailable"].includes(
        String(value.state),
      ) ||
      typeof value.reason !== "string" ||
      typeof value.detail !== "string"
    ) {
      throw new Error("RTK状态消息字段无效");
    }
    return value as RtkStatusMessage;
  }

  if (value.type !== "rtk_snapshot") {
    throw new Error(`未知RTK消息类型：${value.type}`);
  }

  if (
    typeof value.sequence !== "number" ||
    typeof value.emitted_at_ns !== "number" ||
    (value.serial_connected !== null && typeof value.serial_connected !== "boolean") ||
    typeof value.serial_message !== "string"
  ) {
    throw new Error("RTK快照基础字段无效");
  }

  const numericFields = [
    "status_stamp_ns",
    "event_mask",
    "rmc_validity",
    "gps_state",
    "satellite_count",
    "hdop",
    "pdop",
    "latitude_sigma",
    "longitude_sigma",
    "height_sigma",
    "speed_knots",
    "track_degrees",
    "fix_stamp_ns",
    "fix_status",
    "latitude",
    "longitude",
    "altitude",
    "localization_stamp_ns",
    "localization_mode",
    "localization_heading_source",
    "localization_latitude",
    "localization_longitude",
    "localization_altitude",
    "localization_heading_deg",
    "localization_delta_yaw_deg",
    "localization_horizontal_scale",
    "localization_vertical_scale",
    "localization_scale_baseline_m",
    "localization_heading_baseline_m",
    "localization_distance_from_anchor_m",
    "localization_dr_duration_s",
    "localization_rtk_age_s",
    "localization_odometry_age_s",
    "localization_imu_age_s",
    "localization_position_difference_to_rtk_m",
  ] as const;
  if (numericFields.some((field) => !isNullableNumber(value[field]))) {
    throw new Error("RTK快照数值字段无效");
  }

  const booleanFields = [
    "localization_valid",
    "localization_heading_alignment_valid",
    "localization_scale_calibration_enabled",
    "localization_scale_valid",
  ] as const;
  if (booleanFields.some((field) => value[field] !== null && typeof value[field] !== "boolean")) {
    throw new Error("RTK快照布尔字段无效");
  }
  if (
    value.localization_invalid_reason !== null &&
    typeof value.localization_invalid_reason !== "string"
  ) {
    throw new Error("RTK快照融合定位原因字段无效");
  }

  return value as RtkSnapshot;
}
