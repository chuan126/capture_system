export type WorkflowPageId = "dashboard" | "playback" | "report";

export type CollectionTaskStatus = "待执行" | "采集中" | "已暂停" | "已停止";

export type CollectionTaskLane = "左车道" | "右车道";

export type CollectionTask = {
  taskId: string;
  sequence: number;
  tunnelCode: string;
  tunnelName: string;
  status: CollectionTaskStatus;
  lane: CollectionTaskLane | null;
};

export const formatTaskSequence = (sequence: number) =>
  String(sequence).padStart(2, "0");
