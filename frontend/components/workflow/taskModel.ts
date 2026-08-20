export type WorkflowPageId = "dashboard" | "playback" | "report";

export type CollectionTaskStatus =
  | "待执行"
  | "采集中"
  | "已暂停"
  | "已停止"
  | "异常中断"
  | "失败";

export type TaskOperationPhase =
  | "idle" | "radar_initializing" | "entry_rtk_capture" | "recorder_preparing"
  | "recording" | "pausing" | "paused" | "resuming" | "stop_requested"
  | "exit_rtk_capture" | "finalizing" | "completed" | "interrupted" | "failed";

export type RtkCaptureStatus = "not_requested" | "pending" | "confirmed" | "unconfirmed";
export type TaskTravelDirection = "up" | "down";
export type TaskLaneSide = "left" | "right";
export type CollectionTaskLane = "上行左车道" | "上行右车道" | "下行左车道" | "下行右车道";
export type CollectionTaskLaneDisplay = CollectionTaskLane | "左车道" | "右车道";

export const laneSelectionParts: Record<CollectionTaskLane, { travelDirection: TaskTravelDirection; laneSide: TaskLaneSide }> = {
  上行左车道: { travelDirection: "up", laneSide: "left" },
  上行右车道: { travelDirection: "up", laneSide: "right" },
  下行左车道: { travelDirection: "down", laneSide: "left" },
  下行右车道: { travelDirection: "down", laneSide: "right" },
};

export const formatLaneDisplay = (
  travelDirection: string | null | undefined,
  laneSide: string | null | undefined,
  legacyLane?: string | null,
): CollectionTaskLaneDisplay | null => {
  const side = laneSide === "left" || laneSide === "right" ? laneSide : legacyLane;
  if (travelDirection === "up" && side === "left") return "上行左车道";
  if (travelDirection === "up" && side === "right") return "上行右车道";
  if (travelDirection === "down" && side === "left") return "下行左车道";
  if (travelDirection === "down" && side === "right") return "下行右车道";
  if (side === "left") return "左车道";
  if (side === "right") return "右车道";
  return null;
};

export type CollectionTask = {
  taskId: string;
  displayId: string;
  tunnelCode: string;
  tunnelName: string;
  status: CollectionTaskStatus;
  operationPhase: TaskOperationPhase;
  statusRevision: number;
  lane: CollectionTaskLaneDisplay | null;
  lidarMountHeightM: number | null;
  clearanceThresholdM: number | null;
  clearanceUpperLimitM: number | null;
  createdAt: string;
  updatedAt: string;
  startRequestedAt: string | null;
  startedAt: string | null;
  stopRequestedAt: string | null;
  completedAt: string | null;
  entryRtkStatus: RtkCaptureStatus;
  exitRtkStatus: RtkCaptureStatus;
  hasMeasurements: boolean;
  recordingPath: string | null;
  localDataPurgedAt: string | null;
  purgedBytes: number;
  lastErrorCode: string | null;
  lastErrorMessage: string | null;
  warningCode: string | null;
  schemaVersion: number;
};

export const formatTaskDisplayId = (task: CollectionTask | null | undefined) => task?.displayId ?? "--";

export const taskPhaseLabels: Record<TaskOperationPhase, string> = {
  idle: "等待开始", radar_initializing: "雷达初始化", entry_rtk_capture: "记录入口 RTK",
  recorder_preparing: "创建记录文件", recording: "正式记录", pausing: "正在暂停",
  paused: "暂停等待", resuming: "正在继续", stop_requested: "停止记录",
  exit_rtk_capture: "记录出口 RTK", finalizing: "文件收尾", completed: "任务完成",
  interrupted: "异常中断", failed: "操作失败",
};

export const rtkCaptureLabels: Record<RtkCaptureStatus, string> = {
  not_requested: "未记录", pending: "正在记录", confirmed: "已确认", unconfirmed: "坐标未确认",
};

export const isTaskControlBusy = (task: CollectionTask | null | undefined) => {
  if (!task) return false;
  return ["radar_initializing", "entry_rtk_capture", "recorder_preparing", "pausing", "resuming", "stop_requested", "exit_rtk_capture", "finalizing"].includes(task.operationPhase);
};

export const isTaskActive = (task: CollectionTask | null | undefined) => {
  if (!task) return false;
  return task.status === "采集中" || task.status === "已暂停" || isTaskControlBusy(task);
};

export const taskDateKey = (task: CollectionTask) => task.displayId.slice(0, 8);
export const formatTaskDateKey = (key: string) => key.length === 8 ? `${key.slice(0, 4)}-${key.slice(4, 6)}-${key.slice(6, 8)}` : key;
