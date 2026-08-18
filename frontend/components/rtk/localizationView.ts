import type { RtkSnapshot } from "./rtkProtocol";

export const RTK_SOLUTION_LABELS: Record<number, string> = {
  0: "未定位",
  1: "单点定位",
  2: "差分定位",
  4: "RTK固定",
  5: "RTK浮动",
};

export const LOCALIZATION_MODE_LABELS: Record<number, string> = {
  0: "无效",
  1: "RTK",
  2: "航位推算",
  3: "RTK恢复",
};

export type LocalizationStatusView = {
  valid: boolean;
  modeText: string;
  tone: "ok" | "warn" | "danger";
  statusText: string;
};

export function rtkSolutionLabel(gpsState: number | null | undefined): string {
  if (gpsState === null || gpsState === undefined) return "--";
  return RTK_SOLUTION_LABELS[gpsState] ?? `状态 ${gpsState}`;
}

export function localizationModeLabel(mode: number | null | undefined): string {
  if (mode === null || mode === undefined) return "--";
  return LOCALIZATION_MODE_LABELS[mode] ?? `模式 ${mode}`;
}

export function deriveLocalizationStatus(
  snapshot: RtkSnapshot | null,
  streamAvailable: boolean,
): LocalizationStatusView {
  const latitude = snapshot?.localization_latitude;
  const longitude = snapshot?.localization_longitude;
  const valid = streamAvailable &&
    snapshot?.localization_valid === true &&
    typeof latitude === "number" && Number.isFinite(latitude) &&
    typeof longitude === "number" && Number.isFinite(longitude);
  const modeText = localizationModeLabel(snapshot?.localization_mode);
  return {
    valid,
    modeText,
    tone: valid ? snapshot?.localization_mode === 2 ? "warn" : "ok" : "danger",
    statusText: valid ? modeText : snapshot?.localization_invalid_reason ?? "等待融合定位",
  };
}
