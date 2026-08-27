import { TaskControlApiError } from "./taskControlApi";

export type ClearanceAlgorithmParameters = {
  detectionRadiusM: number;
  minSupportPoints: number;
};

const parse = (value: unknown): ClearanceAlgorithmParameters => {
  if (typeof value !== "object" || value === null) throw new TaskControlApiError("净空算法参数响应无效");
  const payload = value as Record<string, unknown>;
  if (typeof payload.detection_radius_m !== "number" || !Number.isInteger(payload.min_support_points)) {
    throw new TaskControlApiError("净空算法参数字段无效");
  }
  return {
    detectionRadiusM: payload.detection_radius_m,
    minSupportPoints: Number(payload.min_support_points),
  };
};

const request = async (method: "GET" | "PUT", value?: ClearanceAlgorithmParameters) => {
  const response = await fetch("/api/v1/algorithm-parameters", {
    method,
    headers: { Accept: "application/json", ...(value ? { "Content-Type": "application/json" } : {}) },
    body: value ? JSON.stringify({
      detection_radius_m: value.detectionRadiusM,
      min_support_points: value.minSupportPoints,
    }) : undefined,
    cache: "no-store",
  });
  if (!response.ok) {
    let detail = response.statusText;
    try {
      const payload = await response.json() as { detail?: unknown };
      if (typeof payload.detail === "string") detail = payload.detail;
    } catch {}
    throw new TaskControlApiError(detail || `HTTP ${response.status}`, response.status);
  }
  return parse(await response.json());
};

export const loadClearanceAlgorithmParameters = () => request("GET");
export const saveClearanceAlgorithmParameters = (value: ClearanceAlgorithmParameters) => request("PUT", value);
