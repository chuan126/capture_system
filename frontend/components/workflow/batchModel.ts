export type CaptureBatchStatus = "active" | "completed" | "archived" | "purged";

export type CaptureBatch = {
  batchId: string;
  batchCode: string;
  operationDate: string;
  dailySequence: number;
  status: CaptureBatchStatus;
  createdAt: string;
  startedAt: string;
  completedAt: string | null;
  archivedAt: string | null;
  purgedAt: string | null;
  taskCount: number;
  visibleTaskCount: number;
  measurementBytes: number;
  reportId: string | null;
  reportPath: string | null;
  reportSha256: string | null;
  reportGeneratedAt: string | null;
  purgedBytes: number;
};

export const batchStatusLabels: Record<CaptureBatchStatus, string> = {
  active: "进行中",
  completed: "已结束",
  archived: "已暂存",
  purged: "已清理",
};

export const formatStorageBytes = (bytes: number) => {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let value = bytes;
  let unitIndex = 0;
  while (value >= 1024 && unitIndex < units.length - 1) {
    value /= 1024;
    unitIndex += 1;
  }
  return `${value.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
};
