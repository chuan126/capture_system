"use client";

import { useEffect, useMemo, useState } from "react";

import { useClearanceSocket } from "@/components/clearance/useClearanceSocket";
import type { ClearanceSnapshot } from "@/components/clearance/clearanceProtocol";
import RealtimeAmap from "@/components/map/RealtimeAmap";
import PointCloudViewer from "@/components/point-cloud/PointCloudViewer";
import { useRtkSocket } from "@/components/rtk/useRtkSocket";
import { useSystemStatusSocket } from "@/components/system-status/useSystemStatusSocket";
import { isDeviceConnected } from "@/components/system-status/systemStatusProtocol";
import type { DeviceStatus, HealthState } from "@/components/system-status/systemStatusProtocol";
import PlaybackWorkspace from "@/components/playback/PlaybackWorkspace";
import ReportWorkspace from "@/components/report/ReportWorkspace";
import { formatTaskSequence } from "@/components/workflow/taskModel";
import type { CollectionTask, CollectionTaskLane, CollectionTaskStatus, WorkflowPageId } from "@/components/workflow/taskModel";

type PageId = WorkflowPageId;
type CollectionTaskDraft = {
  tunnelCode: string;
  tunnelName: string;
};

const createTaskDraft = (): CollectionTaskDraft => ({
  tunnelCode: "",
  tunnelName: "",
});

const createTaskId = (sequence: number, tunnelCode: string) =>
  JSON.stringify([sequence, tunnelCode.trim()]);

function taskTone(status: CollectionTaskStatus): "idle" | "ok" | "warn" | "danger" {
  if (status === "采集中") return "ok";
  if (status === "已暂停") return "warn";
  if (status === "已停止") return "danger";
  return "idle";
}

function TaskCreateDialog({
  startingSequence,
  onClose,
  onCreate,
}: {
  startingSequence: number;
  onClose: () => void;
  onCreate: (drafts: CollectionTaskDraft[]) => void;
}) {
  const [rows, setRows] = useState<CollectionTaskDraft[]>(() => [createTaskDraft()]);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") onClose();
    };
    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [onClose]);

  const updateRow = <K extends keyof CollectionTaskDraft,>(
    index: number,
    key: K,
    value: CollectionTaskDraft[K],
  ) => {
    setRows((current) => current.map((row, rowIndex) =>
      rowIndex === index ? { ...row, [key]: value } : row,
    ));
    setError(null);
  };

  const validateRows = () => {
    for (let index = 0; index < rows.length; index += 1) {
      const row = rows[index];
      const rowName = `第 ${index + 1} 行`;
      const tunnelCode = row.tunnelCode.trim();
      const tunnelName = row.tunnelName.trim();

      if (!tunnelCode) return `${rowName}缺少隧道编号`;
      if (!tunnelName) return `${rowName}缺少隧道名称`;
    }
    return null;
  };

  const submit = () => {
    const validationError = validateRows();
    if (validationError) {
      setError(validationError);
      return;
    }
    onCreate(rows.map((row) => ({
      tunnelCode: row.tunnelCode.trim(),
      tunnelName: row.tunnelName.trim(),
    })));
  };

  return (
    <div
      className="task-dialog-mask"
      role="dialog"
      aria-modal="true"
      aria-labelledby="task-dialog-title"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose();
      }}
    >
      <section className="task-dialog-panel task-dialog-panel--wide">
        <header className="task-dialog-head">
          <div>
            <h2 id="task-dialog-title">创建检测任务</h2>
            <p>可一次录入一条或多条隧道记录。任务编号由系统按创建顺序自动生成，只需填写隧道编号和隧道名称。</p>
          </div>
          <button type="button" onClick={onClose} aria-label="关闭任务创建窗口">×</button>
        </header>

        <div className="task-dialog-rows">
          {rows.map((row, index) => (
            <article className="task-dialog-row" key={`task-draft-${index}`}>
              <span className="task-dialog-row__index">{formatTaskSequence(startingSequence + index)}</span>
              <label>
                <span>隧道编号</span>
                <input
                  value={row.tunnelCode}
                  onChange={(event) => updateRow(index, "tunnelCode", event.target.value)}
                  placeholder="例如 T-001"
                />
              </label>
              <label>
                <span>隧道名称</span>
                <input
                  value={row.tunnelName}
                  onChange={(event) => updateRow(index, "tunnelName", event.target.value)}
                  placeholder="请输入隧道名称"
                />
              </label>
              <div className="task-dialog-row__actions">
                <button
                  type="button"
                  onClick={() => setRows((current) => [
                    ...current.slice(0, index + 1),
                    { ...row },
                    ...current.slice(index + 1),
                  ])}
                >复制</button>
                <button
                  type="button"
                  disabled={rows.length === 1}
                  onClick={() => setRows((current) => current.filter((_, rowIndex) => rowIndex !== index))}
                >删除</button>
              </div>
            </article>
          ))}
        </div>

        <button
          type="button"
          className="task-dialog-add-row"
          onClick={() => setRows((current) => [...current, createTaskDraft()])}
        >＋ 添加任务</button>

        {error && <p className="task-dialog-error" role="alert">{error}</p>}

        <footer className="task-dialog-actions">
          <button type="button" className="button" onClick={onClose}>取消</button>
          <button type="button" className="button button--primary" onClick={submit}>
            保存 {rows.length} 项任务
          </button>
        </footer>
      </section>
    </div>
  );
}

function TaskSwitchDialog({
  tasks,
  currentTaskId,
  onClose,
  onSelect,
}: {
  tasks: CollectionTask[];
  currentTaskId: string | null;
  onClose: () => void;
  onSelect: (taskId: string) => void;
}) {
  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") onClose();
    };
    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [onClose]);

  return (
    <div
      className="task-dialog-mask"
      role="dialog"
      aria-modal="true"
      aria-labelledby="task-switch-dialog-title"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose();
      }}
    >
      <section className="task-dialog-panel task-switch-dialog">
        <header className="task-dialog-head">
          <div>
            <h2 id="task-switch-dialog-title">切换检测任务</h2>
            <p>选择需要显示或执行的任务。采集过程中不能切换。</p>
          </div>
          <button type="button" onClick={onClose} aria-label="关闭任务选择窗口">×</button>
        </header>

        <div className="task-switch-list">
          {tasks.map((task) => (
            <button
              type="button"
              key={task.taskId}
              className={task.taskId === currentTaskId ? "active" : ""}
              onClick={() => onSelect(task.taskId)}
            >
              <span>{formatTaskSequence(task.sequence)}</span>
              <div>
                <strong>任务 {formatTaskSequence(task.sequence)}</strong>
                <small>{task.tunnelCode} · {task.tunnelName}</small>
              </div>
              <StatusPill tone={taskTone(task.status)}>{task.status}</StatusPill>
            </button>
          ))}
        </div>
      </section>
    </div>
  );
}

const navigation: Array<{ id: PageId; label: string; index: string }> = [
  { id: "dashboard", label: "采集首页", index: "01" },
  { id: "playback", label: "数据回放", index: "02" },
  { id: "report", label: "报告导出", index: "03" },
];

function StatusPill({
  children,
  tone = "idle",
}: {
  children: React.ReactNode;
  tone?: "idle" | "ok" | "warn" | "danger";
}) {
  return (
    <span className={`status-pill status-pill--${tone}`}>
      <i />
      {children}
    </span>
  );
}

function PanelHead({
  title,
  description,
  trailing,
}: {
  title: string;
  description?: string;
  trailing?: React.ReactNode;
}) {
  return (
    <div className="panel-head">
      <div>
        <h2>{title}</h2>
        {description && <p>{description}</p>}
      </div>
      {trailing}
    </div>
  );
}

function ExpandButton({
  expanded,
  label,
  onClick,
}: {
  expanded: boolean;
  label: string;
  onClick: () => void;
}) {
  return (
    <button
      type="button"
      className="panel-expand-button"
      aria-label={expanded ? `退出${label}放大显示` : `放大显示${label}`}
      aria-pressed={expanded}
      title={expanded ? "退出放大显示" : "放大显示"}
      onClick={onClick}
    >
      {expanded ? (
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M9 4v5H4M15 4v5h5M9 20v-5H4M15 20v-5h5" />
        </svg>
      ) : (
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M9 4H4v5M15 4h5v5M9 20H4v-5M15 20h5v-5" />
        </svg>
      )}
    </button>
  );
}

function EmptyChart({
  historical = false,
  clearanceActive = false,
}: {
  historical?: boolean;
  clearanceActive?: boolean;
}) {
  return (
    <div className="chart">
      <div className="chart__grid" />
      <div className="chart__axis chart__axis--y">
        <span>6.0</span><span>5.5</span><span>5.0</span><span>4.5</span>
      </div>
      <div className="chart__axis chart__axis--x">
        <span>0</span><span>25</span><span>50</span><span>75</span><span>100</span>
      </div>
      <EmptyState
        compact
        icon="⌁"
        title={historical
          ? "请选择任务查看净空曲线"
          : clearanceActive
            ? "实时高度已经接入"
            : "等待净空测量数据"}
        description={historical
          ? "历史任务接入后显示"
          : clearanceActive
            ? "当前数值显示在右侧，曲线等待任务里程链路"
            : "采集开始后在此显示实时结果"}
      />
    </div>
  );
}

type LiveClearanceSample = {
  sequence: number;
  heightM: number | null;
};

function LiveClearanceChart({
  snapshot,
  streaming,
  detail,
}: {
  snapshot: ClearanceSnapshot | null;
  streaming: boolean;
  detail: string;
}) {
  const [samples, setSamples] = useState<LiveClearanceSample[]>([]);

  useEffect(() => {
    if (!snapshot) return;
    const heightM = snapshot.valid && snapshot.lidar_to_top_m !== null
      ? snapshot.lidar_to_top_m
      : null;
    setSamples((current) => {
      if (current.at(-1)?.sequence === snapshot.sequence) return current;
      return [...current, { sequence: snapshot.sequence, heightM }].slice(-120);
    });
  }, [snapshot]);

  const chart = useMemo(() => {
    const values = samples
      .map((sample) => sample.heightM)
      .filter((height): height is number => height !== null);
    if (values.length === 0) return null;

    const valueMin = Math.min(...values);
    const valueMax = Math.max(...values);
    const padding = Math.max((valueMax - valueMin) * 0.2, 0.1);
    const yMin = Math.max(0, valueMin - padding);
    const yMax = valueMax + padding;
    const xFor = (index: number) => samples.length <= 1
      ? 50
      : 8 + index / (samples.length - 1) * 90;
    const yFor = (height: number) => 92 - (height - yMin) / (yMax - yMin) * 84;

    const segments: string[] = [];
    let segment = "";
    samples.forEach((sample, index) => {
      if (sample.heightM === null) {
        if (segment) segments.push(segment);
        segment = "";
        return;
      }
      const point = `${xFor(index).toFixed(2)} ${yFor(sample.heightM).toFixed(2)}`;
      segment += `${segment ? " L" : "M"}${point}`;
    });
    if (segment) segments.push(segment);

    const latestIndex = samples.findLastIndex((sample) => sample.heightM !== null);
    const latest = latestIndex >= 0 && samples[latestIndex].heightM !== null
      ? { x: xFor(latestIndex), y: yFor(samples[latestIndex].heightM!) }
      : null;
    return { yMin, yMax, segments, latest };
  }, [samples]);

  if (!chart) {
    return <EmptyChart clearanceActive={streaming} />;
  }

  return (
    <div className="chart live-clearance-chart" aria-label="实时净空高度曲线">
      <div className="chart__grid" />
      <div className="chart__axis chart__axis--y">
        <span>{chart.yMax.toFixed(2)}</span>
        <span>{((chart.yMax + chart.yMin) / 2).toFixed(2)}</span>
        <span>{chart.yMin.toFixed(2)}</span>
      </div>
      <div className="chart__axis chart__axis--x">
        <span>较早</span><span>最近120帧</span><span>当前</span>
      </div>
      <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
        {chart.segments.map((path, index) => (
          <path key={`${samples[0]?.sequence}-${index}`} d={path} />
        ))}
        {chart.latest && <circle cx={chart.latest.x} cy={chart.latest.y} r="1.25" />}
      </svg>
      {!streaming && (
        <div className="live-clearance-chart__status" role="status">{detail}</div>
      )}
    </div>
  );
}

function EmptyState({
  icon,
  title,
  description,
  compact = false,
}: {
  icon: string;
  title: string;
  description: string;
  compact?: boolean;
}) {
  return (
    <div className={`empty-state${compact ? " empty-state--compact" : ""}`}>
      <span className="empty-state__icon">{icon}</span>
      <strong>{title}</strong>
      <small>{description}</small>
    </div>
  );
}

function Header({ page, task }: { page: PageId; task: CollectionTask | null }) {
  const title = navigation.find((item) => item.id === page)?.label;
  return (
    <header className="topbar">
      <div>
        <div className="breadcrumb">三维采集系统 / {title}</div>
        <h1>车载隧道净空高度测量</h1>
        <p>Odin1 Lite 激光雷达 · RK3588 设备端显控界面</p>
      </div>
      <div className="topbar__right">
        <div className="connection">
          <span>系统状态</span>
          <StatusPill tone={task ? taskTone(task.status) : "idle"}>{task?.status ?? "未选择任务"}</StatusPill>
        </div>
        <div className="clock">
          <strong>--:--</strong>
          <span>设备时间</span>
        </div>
      </div>
    </header>
  );
}

function Dashboard({
  tasks,
  setTasks,
  selectedTaskId,
  setSelectedTaskId,
  onNavigate,
}: {
  tasks: CollectionTask[];
  setTasks: React.Dispatch<React.SetStateAction<CollectionTask[]>>;
  selectedTaskId: string | null;
  setSelectedTaskId: React.Dispatch<React.SetStateAction<string | null>>;
  onNavigate: (page: PageId) => void;
}) {
  const [expandedVisual, setExpandedVisual] = useState<"cloud" | "map" | null>(null);
  const [taskDialogOpen, setTaskDialogOpen] = useState(false);
  const [taskSwitchOpen, setTaskSwitchOpen] = useState(false);
  const [heightThreshold, setHeightThreshold] = useState("4.50");
  const [mountHeight, setMountHeight] = useState("1.86");
  const [operationLane, setOperationLane] = useState<CollectionTaskLane>("右车道");
  const rtk = useRtkSocket();
  const rtkSnapshot = rtk.snapshot;
  const clearance = useClearanceSocket();
  const clearanceSnapshot = clearance.snapshot;
  const systemStatus = useSystemStatusSocket();
  const systemSnapshot = systemStatus.snapshot;

  useEffect(() => {
    if (!expandedVisual) return;

    const previousOverflow = document.body.style.overflow;
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") setExpandedVisual(null);
    };
    document.body.style.overflow = "hidden";
    window.addEventListener("keydown", handleKeyDown);
    return () => {
      document.body.style.overflow = previousOverflow;
      window.removeEventListener("keydown", handleKeyDown);
    };
  }, [expandedVisual]);

  const systemStreamAvailable = systemStatus.connection === "connected" && systemStatus.streamState === "streaming";
  const rtkDeviceText = systemStreamAvailable ? systemSnapshot?.rtk.message ?? "检查中" : "检查中";
  const rtkTone = !systemStreamAvailable
    ? "idle"
    : systemSnapshot?.rtk.state === "ok"
      ? "ok"
      : systemSnapshot?.rtk.state === "warn"
        ? "warn"
        : systemSnapshot?.rtk.state === "error" || systemSnapshot?.rtk.state === "stale"
          ? "danger"
          : "idle";
  const solutionLabels: Record<number, string> = {
    0: "未定位",
    1: "单点定位",
    2: "差分定位",
    4: "RTK固定",
    5: "RTK浮动",
  };
  const gpsState = rtkSnapshot?.gps_state;
  const rtkCardValue = gpsState === null || gpsState === undefined
    ? "--"
    : solutionLabels[gpsState] ?? `状态 ${gpsState}`;
  const rmcCharacter = rtkSnapshot?.rmc_validity
    ? String.fromCharCode(rtkSnapshot.rmc_validity)
    : null;
  const formatMetric = (value: number | null | undefined, digits = 2) =>
    value === null || value === undefined ? "--" : value.toFixed(digits);
  const hasFix = rtkSnapshot?.fix_status !== null &&
    rtkSnapshot?.fix_status !== undefined && rtkSnapshot.fix_status !== -1;
  const altitudeText = hasFix ? `${formatMetric(rtkSnapshot?.altitude)} m` : "--";
  const latitudeValue = rtkSnapshot?.latitude;
  const longitudeValue = rtkSnapshot?.longitude;
  const coordinateAvailable = rtk.connection === "connected" &&
    rtk.streamState === "streaming" &&
    hasFix &&
    rmcCharacter !== "V" &&
    typeof latitudeValue === "number" &&
    Number.isFinite(latitudeValue) &&
    typeof longitudeValue === "number" &&
    Number.isFinite(longitudeValue);
  const latitudeText = coordinateAvailable && typeof latitudeValue === "number"
    ? latitudeValue.toFixed(7)
    : "--";
  const longitudeText = coordinateAvailable && typeof longitudeValue === "number"
    ? longitudeValue.toFixed(7)
    : "--";
  const monitorUnavailable = !systemStreamAvailable;
  const lidarConnected = !monitorUnavailable && isDeviceConnected("lidar", systemSnapshot?.lidar);
  const rtkConnected = !monitorUnavailable && isDeviceConnected("rtk", systemSnapshot?.rtk);
  const controllerConnected = !monitorUnavailable && isDeviceConnected("controller", systemSnapshot?.controller);
  const storageConnected = !monitorUnavailable && isDeviceConnected("storage", systemSnapshot?.storage);
  const availableBytes = Number(systemSnapshot?.storage.values?.available_bytes);
  const storageText = !monitorUnavailable && Number.isFinite(availableBytes)
    ? `${(availableBytes / 1024 ** 3).toFixed(1)} GiB`
    : "--";
  const devices = systemSnapshot
    ? [systemSnapshot.lidar, systemSnapshot.rtk, systemSnapshot.controller, systemSnapshot.storage]
    : [];
  const aggregateState: HealthState = monitorUnavailable
    ? "unknown"
    : devices.some((device) => device.state === "error" || device.state === "stale")
      ? "error"
      : devices.some((device) => device.state === "warn")
        ? "warn"
        : devices.length === 4 && devices.every((device) => device.state === "ok")
          ? "ok"
          : "unknown";
  const aggregateText = {ok: "系统正常", warn: "系统告警", error: "系统异常", stale: "系统异常", unknown: "检查中"}[aggregateState];
  const clearanceStreaming = clearance.connection === "connected" && clearance.streamState === "streaming";
  const clearanceValid = clearanceStreaming && clearanceSnapshot?.valid === true &&
    clearanceSnapshot.lidar_to_top_m !== null;
  const currentHeightText = clearanceValid
    ? clearanceSnapshot.lidar_to_top_m!.toFixed(3)
    : "--";
  const deviceValue = (device: DeviceStatus | undefined, key: string) =>
    monitorUnavailable ? null : device?.values?.[key] ?? null;
  const formatPercentValue = (device: DeviceStatus | undefined, key: string) => {
    const value = Number(deviceValue(device, key));
    return Number.isFinite(value) && value >= 0 ? `${value.toFixed(1)}%` : "--";
  };
  const controllerCpuText = formatPercentValue(systemSnapshot?.controller, "cpu_percent");
  const activeTask = tasks.find((task) => task.status === "采集中" || task.status === "已暂停");
  const selectedTask = tasks.find((task) => task.taskId === selectedTaskId);
  const currentTask = activeTask ?? selectedTask ?? tasks.find((task) => task.status === "待执行") ?? null;
  const pendingTasks = tasks.filter((task) => task.status === "待执行" && task.taskId !== currentTask?.taskId);
  const parsedHeightThreshold = Number(heightThreshold);
  const heightThresholdValid = Number.isFinite(parsedHeightThreshold) && parsedHeightThreshold > 0;
  const parsedMountHeight = Number(mountHeight);
  const mountHeightValid = Number.isFinite(parsedMountHeight) && parsedMountHeight > 0;
  const captureReady = lidarConnected && rtkConnected && storageConnected;
  const taskLocked = currentTask?.status === "采集中" || currentTask?.status === "已暂停";
  const currentTaskStatus = currentTask?.status ?? "待执行";
  const taskRunning = currentTask?.status === "采集中" || currentTask?.status === "已暂停";
  const nextTaskSequence = tasks.reduce((maximum, task) => Math.max(maximum, task.sequence), 0) + 1;

  const createTasks = (drafts: CollectionTaskDraft[]) => {
    const nextSequence = nextTaskSequence;
    const created: CollectionTask[] = drafts.map((draft, index) => {
      const sequence = nextSequence + index;
      return {
        taskId: createTaskId(sequence, draft.tunnelCode),
        sequence,
        tunnelCode: draft.tunnelCode,
        tunnelName: draft.tunnelName,
        status: "待执行",
        lane: operationLane,
      };
    });
    setTasks((current) => [...current, ...created]);
    if (!currentTask) setSelectedTaskId(created[0]?.taskId ?? null);
    setTaskDialogOpen(false);
  };

  const selectTask = (taskId: string) => {
    if (taskLocked) return;
    setSelectedTaskId(taskId);
  };

  const startTask = () => {
    if (
      !currentTask ||
      currentTask.status !== "待执行" ||
      !captureReady ||
      !heightThresholdValid ||
      !mountHeightValid
    ) return;
    setTasks((current) => current.map((task) =>
      task.taskId === currentTask.taskId ? { ...task, status: "采集中", lane: operationLane } : task,
    ));
  };

  const togglePauseTask = () => {
    if (!currentTask || (currentTask.status !== "采集中" && currentTask.status !== "已暂停")) return;
    const nextStatus: CollectionTaskStatus = currentTask.status === "采集中" ? "已暂停" : "采集中";
    setTasks((current) => current.map((task) =>
      task.taskId === currentTask.taskId ? { ...task, status: nextStatus } : task,
    ));
  };

  const stopTask = () => {
    if (!currentTask || (currentTask.status !== "采集中" && currentTask.status !== "已暂停")) return;
    if (!window.confirm(`确认停止任务 ${formatTaskSequence(currentTask.sequence)}？当前任务控制仍为前端原型，不会创建可回放记录。`)) return;
    setTasks((current) => current.map((task) =>
      task.taskId === currentTask.taskId ? { ...task, status: "已停止" } : task,
    ));
    setSelectedTaskId(currentTask.taskId);
  };

  return (
    <div className="page-stack dashboard-page">
      <section className="dashboard-health-bar" aria-label="系统状态栏">
        <div className="health-overview">
          <div className="health-overview__summary">
            <div className={`health-overview__icon health-overview__icon--${aggregateState}`} aria-hidden="true">
              <svg viewBox="0 0 48 48" focusable="false">
                <rect x="7" y="9" width="34" height="25" rx="5" />
                <path d="M13 24h6l3-7 5 13 3-6h5" />
                <path d="M18 40h12M24 34v6" />
              </svg>
            </div>
            <div className="health-overview__copy">
              <span>系统状态</span>
              <strong className={`device-state device-state--${aggregateState}`}>{aggregateText}</strong>
            </div>
          </div>

          <div className="health-kpi-grid" aria-label="实时测量摘要">
            <article className="health-kpi-card health-kpi-card--primary">
              <span>净空高度</span>
              <strong>{currentHeightText}<small>m</small></strong>
            </article>
            <article className="health-kpi-card health-kpi-card--coordinate">
              <span>当前坐标</span>
              <strong className="health-kpi-coordinate">
                <span><small>经度</small>{longitudeText}</span>
                <span><small>纬度</small>{latitudeText}</span>
              </strong>
            </article>
            <article className="health-kpi-card health-kpi-card--placeholder">
              <span>预留指标二</span>
              <strong>--</strong>
            </article>
          </div>
        </div>

        <div className="health-device-grid">
          <article className={`health-device-card${lidarConnected ? " health-device-card--connected" : ""}`}>
            <div className="health-device-card__identity">
              <span className="health-device-card__icon">LDR</span>
              <div><b>雷达</b><small>Odin1 Lite</small></div>
            </div>
            <div className="health-device-card__value health-device-card__value--empty" aria-hidden="true" />
            <i className="health-device-card__lamp" aria-label={lidarConnected ? "已连接" : "未连接"} />
          </article>

          <article className={`health-device-card${rtkConnected ? " health-device-card--connected" : ""}`}>
            <div className="health-device-card__identity">
              <span className="health-device-card__icon">RTK</span>
              <div><b>RTK</b><small>卫星定位</small></div>
            </div>
            <div className="health-device-card__value">
              <span>定位状态</span>
              <strong>{rtkCardValue}</strong>
            </div>
            <i className="health-device-card__lamp" aria-label={rtkConnected ? "已连接" : "未连接"} />
          </article>

          <article className={`health-device-card${controllerConnected ? " health-device-card--connected" : ""}`}>
            <div className="health-device-card__identity">
              <span className="health-device-card__icon">RK</span>
              <div><b>主控板</b><small>RK3588</small></div>
            </div>
            <div className="health-device-card__value">
              <span>CPU 占用</span>
              <strong>{controllerCpuText}</strong>
            </div>
            <i className="health-device-card__lamp" aria-label={controllerConnected ? "已连接" : "未连接"} />
          </article>

          <article className={`health-device-card${storageConnected ? " health-device-card--connected" : ""}`}>
            <div className="health-device-card__identity">
              <span className="health-device-card__icon">SSD</span>
              <div><b>数据存储</b><small>采集磁盘</small></div>
            </div>
            <div className="health-device-card__value health-device-card__value--storage">
              <span>可用容量</span>
              <strong>{storageText}</strong>
            </div>
            <i className="health-device-card__lamp" aria-label={storageConnected ? "已连接" : "未连接"} />
          </article>
        </div>
      </section>

      {expandedVisual && (
        <button
          type="button"
          className="visual-panel-backdrop"
          aria-label="退出放大显示"
          onClick={() => setExpandedVisual(null)}
        />
      )}

      <section className="dashboard-layout">
        <div className="dashboard-main">
          <section className="dashboard-visual-grid" aria-label="点云与实时地图">
            <article className={`panel cloud-panel dashboard-cloud-panel${expandedVisual === "cloud" ? " visual-panel--expanded" : ""}`}>
              <div className="cloud-toolbar">
                <div>
                  <h2>点云实时预览</h2>
                </div>
                <div className="cloud-toolbar__actions">
                  <span className="panel-tag panel-tag--muted">三维视图</span>
                  <ExpandButton
                    expanded={expandedVisual === "cloud"}
                    label="点云实时预览"
                    onClick={() => setExpandedVisual((current) => current === "cloud" ? null : "cloud")}
                  />
                </div>
              </div>
              <PointCloudViewer />
            </article>

            <RealtimeAmap
              snapshot={rtkSnapshot}
              hasFix={hasFix && rmcCharacter !== "V"}
              connectionDetail={rtk.detail}
              expanded={expandedVisual === "map"}
              onToggleExpanded={() => setExpandedVisual((current) => current === "map" ? null : "map")}
            />
          </section>

          <article className="panel dashboard-clearance-panel">
            <PanelHead
              title="实时净空高度曲线"
              description="最近120帧有效高度与无效帧断点"
              trailing={
                <div className="legend">
                  <span><i />实时净空</span>
                </div>
              }
            />
            <LiveClearanceChart
              snapshot={clearanceSnapshot}
              streaming={clearanceStreaming}
              detail={clearance.detail}
            />
          </article>
        </div>

        <aside className="dashboard-side-stack" aria-label="定位与任务控制">
          <article className="panel rtk-control-panel">
            <PanelHead
              title="RTK 定位"
              description="卫星数、精度因子与高度"
              trailing={<StatusPill tone={rtkTone}>{rtkDeviceText}</StatusPill>}
            />

            <section className="rtk-metric-grid" aria-label="RTK质量指标">
              <article>
                <span>卫星数</span>
                <strong>{rtkSnapshot?.satellite_count ?? "--"}</strong>
              </article>
              <article>
                <span>HDOP / PDOP</span>
                <strong>{formatMetric(rtkSnapshot?.hdop)} / {formatMetric(rtkSnapshot?.pdop)}</strong>
              </article>
              <article>
                <span>高度</span>
                <strong>{altitudeText}</strong>
              </article>
            </section>

          </article>

          <article className="panel task-operation-panel">
            <PanelHead
              title="任务控制"
              description="作业参数、当前任务与采集控制"
              trailing={
                <div className="task-operation-head-actions">
                  <button type="button" onClick={() => setTaskDialogOpen(true)}>创建任务</button>
                </div>
              }
            />

            <div className="task-operation-body">
              <section className={`task-parameter-strip${taskLocked ? " task-parameter-strip--locked" : ""}`} aria-label="共享作业参数">
                <header className="task-section-heading">
                  <div>
                    <h3>作业参数</h3>
                    <span>所有任务共用</span>
                  </div>
                  <small>{taskLocked ? "采集中已锁定" : "可直接修改"}</small>
                </header>

                <div className="task-parameter-grid">
                  <label className={mountHeightValid ? "" : "is-invalid"}>
                    <span>雷达安装高度</span>
                    <div>
                      <input
                        type="number"
                        min="0.01"
                        step="0.01"
                        value={mountHeight}
                        disabled={taskLocked}
                        aria-invalid={!mountHeightValid}
                        onChange={(event) => setMountHeight(event.target.value)}
                      />
                      <small>m</small>
                    </div>
                  </label>

                  <label>
                    <span>作业车道</span>
                    <select
                      value={operationLane}
                      disabled={taskLocked}
                      onChange={(event) => setOperationLane(event.target.value as CollectionTaskLane)}
                      aria-label="设置作业车道"
                    >
                      <option value="左车道">左车道</option>
                      <option value="右车道">右车道</option>
                    </select>
                  </label>

                  <label className={heightThresholdValid ? "" : "is-invalid"}>
                    <span>高度阈值</span>
                    <div>
                      <input
                        type="number"
                        min="0.01"
                        step="0.01"
                        value={heightThreshold}
                        disabled={taskLocked}
                        aria-invalid={!heightThresholdValid}
                        onChange={(event) => setHeightThreshold(event.target.value)}
                      />
                      <small>m</small>
                    </div>
                  </label>
                </div>
              </section>

              <section className={`task-current-card task-current-card--${currentTask ? taskTone(currentTaskStatus) : "idle"}`} aria-label="当前任务">
                <div className="task-current-card__head">
                  <span>当前任务</span>
                  <StatusPill tone={currentTask ? taskTone(currentTaskStatus) : "idle"}>
                    {currentTask ? currentTaskStatus : "无任务"}
                  </StatusPill>
                </div>

                {currentTask ? (
                  <div className="task-current-card__content">
                    <div className="task-current-card__identity">
                      <strong>任务 {formatTaskSequence(currentTask.sequence)}</strong>
                      <small>{currentTask.tunnelCode} · {currentTask.tunnelName} · {currentTask.lane ?? operationLane}</small>
                    </div>
                    <div className="task-current-card__actions">
                      <button
                        type="button"
                        disabled={taskLocked || tasks.length < 2}
                        onClick={() => setTaskSwitchOpen(true)}
                      >
                        切换任务 <span aria-hidden="true">›</span>
                      </button>
                      {currentTask.status === "已停止" && (
                        <>
                          <button type="button" onClick={() => { setSelectedTaskId(currentTask.taskId); onNavigate("playback"); }}>查看数据</button>
                          <button type="button" onClick={() => { setSelectedTaskId(currentTask.taskId); onNavigate("report"); }}>报告检查</button>
                        </>
                      )}
                    </div>
                  </div>
                ) : (
                  <div className="task-current-card__empty">创建任务后可在此选择并开始采集</div>
                )}
              </section>

              <section className="task-queue-section" aria-label="待测任务">
                <div className="task-section-heading task-queue-section__head">
                  <div>
                    <h3>待测任务</h3>
                    <span>按当前队列顺序显示</span>
                  </div>
                  <small>{pendingTasks.length} 项</small>
                </div>

                <div className="task-queue-list">
                  {pendingTasks.length === 0 ? (
                    <div className="task-queue-list__empty">暂无其他待测任务</div>
                  ) : pendingTasks.map((task) => (
                    <button
                      type="button"
                      key={task.taskId}
                      disabled={taskLocked}
                      onClick={() => selectTask(task.taskId)}
                    >
                      <span className="task-queue-list__index">{formatTaskSequence(task.sequence)}</span>
                      <div>
                        <strong>任务 {formatTaskSequence(task.sequence)}</strong>
                        <small>{task.tunnelCode} · {task.tunnelName} · {task.lane ?? operationLane}</small>
                      </div>
                      <span className="task-queue-list__arrow" aria-hidden="true">›</span>
                    </button>
                  ))}
                </div>
              </section>
            </div>

            <footer className="task-operation-actions" aria-label="采集控制">
              {taskRunning ? (
                <div className={`task-running-state${currentTask?.status === "已暂停" ? " task-running-state--paused" : ""}`}>
                  <i />
                  <span>{currentTask?.status === "已暂停" ? "采集已暂停" : "正在采集"}</span>
                </div>
              ) : (
                <button
                  type="button"
                  className="button button--green task-start-button"
                  disabled={
                    !currentTask ||
                    currentTask.status !== "待执行" ||
                    !captureReady ||
                    !heightThresholdValid ||
                    !mountHeightValid
                  }
                  title={!currentTask
                    ? "请先创建任务"
                    : !heightThresholdValid
                      ? "请输入有效的高度阈值"
                      : !mountHeightValid
                        ? "请输入有效的雷达安装高度"
                        : !captureReady
                          ? "请确认雷达、RTK 和存储均已连接"
                          : "开始采集"}
                  onClick={startTask}
                >开始采集</button>
              )}
              <button
                type="button"
                className="button task-pause-button"
                disabled={!currentTask || (currentTask.status !== "采集中" && currentTask.status !== "已暂停")}
                onClick={togglePauseTask}
              >{currentTask?.status === "已暂停" ? "继续" : "暂停"}</button>
              <button
                type="button"
                className="button task-stop-button"
                disabled={!currentTask || (currentTask.status !== "采集中" && currentTask.status !== "已暂停")}
                onClick={stopTask}
              >停止</button>
            </footer>
          </article>
        </aside>
      </section>

      {taskDialogOpen && (
        <TaskCreateDialog
          startingSequence={nextTaskSequence}
          onClose={() => setTaskDialogOpen(false)}
          onCreate={createTasks}
        />
      )}

      {taskSwitchOpen && (
        <TaskSwitchDialog
          tasks={tasks}
          currentTaskId={currentTask?.taskId ?? null}
          onClose={() => setTaskSwitchOpen(false)}
          onSelect={(taskId) => {
            selectTask(taskId);
            setTaskSwitchOpen(false);
          }}
        />
      )}
    </div>
  );
}

export default function Home() {
  const [activePage, setActivePage] = useState<PageId>("dashboard");
  const [tasks, setTasks] = useState<CollectionTask[]>([]);
  const [selectedTaskId, setSelectedTaskId] = useState<string | null>(null);
  const selectedTask = tasks.find((task) => task.taskId === selectedTaskId) ?? null;

  const navigateTo = (page: PageId) => setActivePage(page);

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brand__mark"><span>T</span></div>
          <div><strong>三维采集系统</strong><span>隧道净空测量终端</span></div>
        </div>
        <div className="nav-label">工作台</div>
        <nav aria-label="主导航">
          {navigation.map((item) => (
            <button
              type="button"
              key={item.id}
              className={activePage === item.id ? "active" : ""}
              onClick={() => navigateTo(item.id)}
              aria-current={activePage === item.id ? "page" : undefined}
            >
              <span>{item.index}</span>{item.label}<i>›</i>
            </button>
          ))}
        </nav>
        <div className="sidebar__bottom">
          <div className="device-card">
            <div className="device-card__icon">RK</div>
            <div><strong>车载主控终端</strong><span>RK3588 · 本地运行</span></div>
            <i />
          </div>
          <div className="version">CAPTURE SYSTEM · V1.0</div>
        </div>
      </aside>
      <main className={activePage === "dashboard" ? "main--dashboard" : undefined}>
        {activePage !== "dashboard" && <Header page={activePage} task={selectedTask} />}
        <div className="page-content">
          {activePage === "dashboard" && (
            <Dashboard
              tasks={tasks}
              setTasks={setTasks}
              selectedTaskId={selectedTaskId}
              setSelectedTaskId={setSelectedTaskId}
              onNavigate={navigateTo}
            />
          )}
          {activePage === "playback" && (
            <PlaybackWorkspace
              tasks={tasks}
              selectedTaskId={selectedTaskId}
              onSelectTask={setSelectedTaskId}
              onNavigate={navigateTo}
            />
          )}
          {activePage === "report" && (
            <ReportWorkspace
              tasks={tasks}
              selectedTaskId={selectedTaskId}
              onSelectTask={setSelectedTaskId}
              onNavigate={navigateTo}
            />
          )}
        </div>
      </main>
    </div>
  );
}
