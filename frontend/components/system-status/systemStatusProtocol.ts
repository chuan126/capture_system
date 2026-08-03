export type HealthState = "ok" | "warn" | "error" | "stale" | "unknown";

export type DeviceStatus = {
  state: HealthState;
  message: string;
  values: Record<string, string> | null;
};

export type SystemStatusSnapshot = {
  type: "system_status_snapshot";
  sequence: number;
  emitted_at_ns: number;
  lidar: DeviceStatus;
  rtk: DeviceStatus;
  controller: DeviceStatus;
  storage: DeviceStatus;
};

export type SystemStreamStatus = {
  type: "status";
  state: "waiting" | "streaming" | "degraded" | "ros_unavailable";
  reason: string;
  detail: string;
};

export type SystemStatusMessage = SystemStatusSnapshot | SystemStreamStatus;

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

function isDevice(value: unknown): value is DeviceStatus {
  if (!isObject(value)) return false;
  if (!["ok", "warn", "error", "stale", "unknown"].includes(String(value.state))) return false;
  if (typeof value.message !== "string") return false;
  if (value.values === null) return true;
  return isObject(value.values) && Object.values(value.values).every((item) => typeof item === "string");
}

export function parseSystemStatusText(text: string): SystemStatusMessage {
  const value: unknown = JSON.parse(text);
  if (!isObject(value) || typeof value.type !== "string") throw new Error("系统状态消息不是有效对象");
  if (value.type === "status") {
    if (!["waiting", "streaming", "degraded", "ros_unavailable"].includes(String(value.state)) ||
      typeof value.reason !== "string" || typeof value.detail !== "string") {
      throw new Error("系统状态链路消息字段无效");
    }
    return value as SystemStreamStatus;
  }
  if (value.type !== "system_status_snapshot" || typeof value.sequence !== "number" ||
    typeof value.emitted_at_ns !== "number" || !isDevice(value.lidar) || !isDevice(value.rtk) ||
    !isDevice(value.controller) || !isDevice(value.storage)) {
    throw new Error("系统状态快照字段无效");
  }
  return value as SystemStatusSnapshot;
}
