"use client";

import { useCallback, useEffect, useMemo, useState } from "react";

import { useClearanceSocket } from "@/components/clearance/useClearanceSocket";
import PointCloudViewer from "@/components/point-cloud/PointCloudViewer";
import { useRtkSocket } from "@/components/rtk/useRtkSocket";
import { getTaskControlReadiness, type TaskControlReadiness } from "@/components/workflow/taskControlApi";

import {
  captureRtkSnapshot,
  deleteDevRecording,
  getDevOverview,
  getDevParameters,
  getDevRecordingStatus,
  listDevRecordings,
  setDevParameter,
  startDevRecording,
  stopDevRecording,
  type DevOverview,
  type DevParameter,
  type DevRecording,
  type DevRecordingStatus,
  type DevRecordingRouteProfile,
  type DevTopicTelemetry,
} from "./devtoolsApi";

type DevTab = "overview" | "lidar" | "motion" | "rtk" | "clearance" | "recording" | "parameters";

const tabs: Array<{ id: DevTab; label: string }> = [
  { id: "overview", label: "概览" },
  { id: "lidar", label: "激光雷达" },
  { id: "motion", label: "运动补偿" },
  { id: "rtk", label: "RTK" },
  { id: "clearance", label: "净空" },
  { id: "recording", label: "任务与记录" },
  { id: "parameters", label: "参数" },
];

const bytesText = (value: number | null | undefined) => {
  const bytes = Math.max(0, value ?? 0);
  if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(2)} GiB`;
  if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${bytes} B`;
};

const fixed = (value: unknown, digits = 2) => typeof value === "number" && Number.isFinite(value) ? value.toFixed(digits) : "--";
const nsTime = (value: number | null | undefined) => value ? new Date(value / 1_000_000).toLocaleTimeString("zh-CN", { hour12: false }) : "--";
const stateLabel = (state: DevTopicTelemetry["state"]) => state === "streaming" ? "实时" : state === "stale" ? "超时" : "等待";

function Metric({ label, value, detail }: { label: string; value: React.ReactNode; detail?: React.ReactNode }) {
  return <div className="dev-metric"><span>{label}</span><strong>{value}</strong>{detail && <small>{detail}</small>}</div>;
}

function TopicTable({ topics }: { topics: DevTopicTelemetry[] }) {
  return (
    <div className="dev-table-wrap">
      <table className="dev-table">
        <thead><tr><th>数据源</th><th>状态</th><th>频率</th><th>数据年龄</th><th>最后接收</th><th>累计</th></tr></thead>
        <tbody>{topics.map((topic) => (
          <tr key={topic.key}>
            <td><strong>{topic.topic}</strong><small>{topic.message_type}</small></td>
            <td><span className={`dev-state dev-state--${topic.state}`}>{stateLabel(topic.state)}</span></td>
            <td>{fixed(topic.rate_hz, 1)} Hz</td>
            <td>{topic.age_ms === null ? "--" : `${fixed(topic.age_ms, 0)} ms`}</td>
            <td>{nsTime(topic.last_received_ns)}</td>
            <td>{topic.received_count}</td>
          </tr>
        ))}</tbody>
      </table>
    </div>
  );
}

function useDevOverview() {
  const [overview, setOverview] = useState<DevOverview | null>(null);
  const [error, setError] = useState<string | null>(null);
  const refresh = useCallback(async () => {
    try {
      setOverview(await getDevOverview());
      setError(null);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "开发诊断接口读取失败");
    }
  }, []);
  useEffect(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), 1000);
    return () => window.clearInterval(timer);
  }, [refresh]);
  return { overview, error, refresh };
}

function OverviewPanel({ overview }: { overview: DevOverview | null }) {
  const topics = overview ? Object.values(overview.telemetry.topics) : [];
  const storage = overview?.storage;
  return <div className="dev-section-stack">
    <section className="panel dev-panel">
      <div className="panel-head"><div><h2>开发诊断概览</h2><p>以下状态来自真实接口和ROS 2数据接收统计，不生成模拟设备数据。</p></div></div>
      <div className="dev-metric-grid dev-metric-grid--4">
        <Metric label="构建类型" value="development" detail="客户版不包含本页面" />
        <Metric label="软件版本" value={overview?.version ?? "--"} />
        <Metric label="CPU" value={overview?.system.cpu_percent == null ? "--" : `${overview.system.cpu_percent.toFixed(1)}%`} detail={overview?.system.load_1m == null ? undefined : `1分钟负载 ${overview.system.load_1m}`} />
        <Metric label="内存" value={overview?.system.memory_used_percent == null ? "--" : `${overview.system.memory_used_percent.toFixed(1)}%`} detail={overview ? `可用 ${bytesText(overview.system.memory_available_bytes)}` : undefined} />
        <Metric label="SoC温度" value={overview?.system.soc_temperature_c == null ? "--" : `${overview.system.soc_temperature_c.toFixed(1)} °C`} detail={overview?.system.uptime_seconds == null ? undefined : `运行 ${Math.floor(overview.system.uptime_seconds / 60)} min`} />
        <Metric label="数据目录" value={overview?.data_root ?? "--"} />
        <Metric label="可用空间" value={storage ? bytesText(storage.free_bytes) : "--"} detail={storage ? `总容量 ${bytesText(storage.total_bytes)}` : undefined} />
        <Metric label="诊断ROS桥" value={overview?.telemetry.bridge_available ? "正常" : "不可用"} detail={overview?.telemetry.bridge_error ?? ""} />
      </div>
    </section>
    <section className="panel dev-panel">
      <div className="panel-head"><div><h2>实时数据链路</h2><p>频率按最近消息窗口计算，数据年龄按FastAPI接收时间计算。</p></div></div>
      <TopicTable topics={topics} />
    </section>
  </div>;
}

function LidarPanel({ overview }: { overview: DevOverview | null }) {
  const [preview, setPreview] = useState<"raw" | "compensated">("raw");
  const recordingController = useDevRecordingController(true);
  const raw = overview?.telemetry.topics.raw_cloud;
  const compensated = overview?.telemetry.topics.compensated_cloud;
  return <div className="dev-section-stack">
    <section className="panel dev-panel dev-pointcloud-panel">
      <div className="panel-head"><div><h2>点云预览</h2><p>原始点云开发预览由FastAPI抽取XYZ并限流降采样，算法输入仍使用完整ROS点云。</p></div><div className="dev-segmented"><button className={preview === "raw" ? "active" : ""} onClick={() => setPreview("raw")}>原始点云</button><button className={preview === "compensated" ? "active" : ""} onClick={() => setPreview("compensated")}>补偿后点云</button></div></div>
      <div className="dev-pointcloud-stage" key={preview}>
        <PointCloudViewer
          socketPath={preview === "raw" ? "/ws/dev/raw-cloud-preview" : "/ws/v1/cloud-preview"}
          liveLabel={preview === "raw" ? "原始点云" : "补偿后点云"}
          axisMode={preview === "raw" ? "sensor" : "enu"}
        />
      </div>
    </section>
    <section className="panel dev-panel">
      <div className="dev-metric-grid dev-metric-grid--4">
        <Metric label="原始帧率" value={`${fixed(raw?.rate_hz, 1)} Hz`} detail={`点数 ${raw?.point_count ?? "--"}`} />
        <Metric label="原始数据年龄" value={raw?.age_ms == null ? "--" : `${fixed(raw.age_ms, 0)} ms`} detail={`帧 ${raw?.frame_id ?? "--"}`} />
        <Metric label="补偿帧率" value={`${fixed(compensated?.rate_hz, 1)} Hz`} detail={`点数 ${compensated?.point_count ?? "--"}`} />
        <Metric label="补偿数据年龄" value={compensated?.age_ms == null ? "--" : `${fixed(compensated.age_ms, 0)} ms`} detail={`帧 ${compensated?.frame_id ?? "--"}`} />
      </div>
    </section>
    <RecordingControl controller={recordingController} compact profile="raw-cloud" title="保存原始点云" description="保存 /capture/lidar/points_raw 的完整 PointCloud2 为MCAP。浏览器预览的降采样点不会写入录制文件。" />
  </div>;
}

function MotionPanel({ overview }: { overview: DevOverview | null }) {
  const odom = overview?.telemetry.topics.odometry;
  const raw = overview?.telemetry.topics.raw_cloud;
  const compensated = overview?.telemetry.topics.compensated_cloud;
  return <div className="dev-section-stack">
    <section className="panel dev-panel"><div className="panel-head"><div><h2>运动补偿数据流</h2><p>当前节点没有发布逐帧姿态覆盖率诊断，因此页面只显示可从真实Topic确认的数据和实际参数。</p></div></div>
      <div className="dev-metric-grid dev-metric-grid--4">
        <Metric label="高频里程计" value={`${fixed(odom?.rate_hz, 1)} Hz`} detail={odom?.age_ms == null ? "等待数据" : `${fixed(odom.age_ms, 0)} ms前`} />
        <Metric label="原始点云" value={`${fixed(raw?.rate_hz, 1)} Hz`} detail={`${raw?.received_count ?? 0} 帧`} />
        <Metric label="补偿点云" value={`${fixed(compensated?.rate_hz, 1)} Hz`} detail={`${compensated?.received_count ?? 0} 帧`} />
        <Metric label="输出帧" value={String(compensated?.frame_id ?? "--")} detail="预期 lidar_local_enu" />
      </div>
    </section>
    <ParameterSubset prefix="motion." />
  </div>;
}

function RtkPanel({ overview }: { overview: DevOverview | null }) {
  const rtk = useRtkSocket();
  const [captureResult, setCaptureResult] = useState<{ confirmed: boolean; detail: string; snapshot: Record<string, unknown> | null } | null>(null);
  const [captureError, setCaptureError] = useState<string | null>(null);
  const fix = overview?.telemetry.topics.rtk_fix;
  const status = overview?.telemetry.topics.rtk_status;
  const snapshot = rtk.snapshot;
  const capture = async () => {
    try {
      const result = await captureRtkSnapshot();
      setCaptureResult(result);
      setCaptureError(null);
    } catch (error) {
      setCaptureError(error instanceof Error ? error.message : "RTK快照失败");
    }
  };
  return <div className="dev-section-stack">
    <section className="panel dev-panel"><div className="panel-head"><div><h2>RTK实时状态</h2><p>{rtk.detail}</p></div><button className="button" onClick={() => void capture()}>记录当前RTK快照</button></div>
      <div className="dev-metric-grid dev-metric-grid--4">
        <Metric label="纬度" value={fixed(snapshot?.latitude, 8)} />
        <Metric label="经度" value={fixed(snapshot?.longitude, 8)} />
        <Metric label="高程" value={snapshot?.altitude == null ? "--" : `${fixed(snapshot.altitude, 3)} m`} />
        <Metric label="定位状态" value={snapshot?.fix_status ?? "--"} detail={`卫星 ${snapshot?.satellite_count ?? "--"}`} />
        <Metric label="Fix频率" value={`${fixed(fix?.rate_hz, 1)} Hz`} detail={fix?.age_ms == null ? "--" : `${fixed(fix.age_ms, 0)} ms前`} />
        <Metric label="Status频率" value={`${fixed(status?.rate_hz, 1)} Hz`} detail={`HDOP ${fixed(status?.hdop, 2)}`} />
        <Metric label="串口诊断" value={snapshot?.serial_connected === true ? "已连接" : snapshot?.serial_connected === false ? "异常" : "等待"} detail={snapshot?.serial_message ?? "--"} />
        <Metric label="数据时间" value={nsTime(snapshot?.fix_stamp_ns)} />
      </div>
      {(captureResult || captureError) && <div className={`dev-message ${captureError ? "dev-message--error" : captureResult?.confirmed ? "dev-message--ok" : "dev-message--warn"}`}><strong>{captureError ?? captureResult?.detail}</strong>{captureResult?.snapshot && <span>{String(captureResult.snapshot.latitude ?? "--")}, {String(captureResult.snapshot.longitude ?? "--")}</span>}</div>}
    </section>
  </div>;
}

function ClearancePanel({ overview }: { overview: DevOverview | null }) {
  const clearance = useClearanceSocket();
  const source = overview?.telemetry.topics.clearance;
  const recording = overview?.telemetry.topics.recording_status;
  const snapshot = clearance.snapshot;
  return <div className="dev-section-stack">
    <section className="panel dev-panel"><div className="panel-head"><div><h2>净空实时结果</h2><p>{clearance.detail}</p></div></div>
      <div className="dev-metric-grid dev-metric-grid--4">
        <Metric label="当前算法值" value={snapshot?.lidar_to_top_m == null ? "--" : `${fixed(snapshot.lidar_to_top_m, 3)} m`} detail={snapshot?.valid ? "有效" : snapshot ? snapshot.invalid_reason : "等待数据"} />
        <Metric label="源帧频率" value={`${fixed(source?.rate_hz, 1)} Hz`} detail={source?.age_ms == null ? "--" : `${fixed(source.age_ms, 0)} ms前`} />
        <Metric label="候选平面" value={source?.candidate_count ?? "--"} detail={`内点 ${source?.selected_inlier_count ?? "--"}`} />
        <Metric label="处理时间" value={source?.processing_time_ms == null ? "--" : `${fixed(source.processing_time_ms, 2)} ms`} detail={`有效点 ${source?.valid_point_ratio == null ? "--" : `${(Number(source.valid_point_ratio) * 100).toFixed(1)}%`}`} />
        <Metric label="平面面积" value={source?.selected_area_m2 == null ? "--" : `${fixed(source.selected_area_m2, 3)} m²`} />
        <Metric label="平面倾角" value={source?.selected_tilt_deg == null ? "--" : `${fixed(source.selected_tilt_deg, 2)}°`} />
        <Metric label="残差P95" value={source?.residual_p95_m == null ? "--" : `${fixed(source.residual_p95_m, 4)} m`} />
        <Metric label="50 Hz记录器" value={String(recording?.status ?? "等待")} detail={`总样本 ${recording?.total_samples ?? 0} · 有效 ${recording?.valid_samples ?? 0} · 无效 ${recording?.invalid_samples ?? 0}`} />
      </div>
      <div className="dev-note">50 Hz记录序列采用最近源帧保持。源帧超过250 ms后记录无效样本，不把最后有效值无限延长。</div>
    </section>
    <ParameterSubset prefix="clearance." />
  </div>;
}

const storedProfileName: Record<DevRecordingRouteProfile, DevRecording["profile"]> = {
  "raw-cloud": "raw_cloud",
  diagnostic: "diagnostic",
  "raw-sensor": "raw_sensor",
  "algorithm-debug": "algorithm_debug",
  "full-debug": "full_debug",
};

type RecordingController = {
  status: DevRecordingStatus | null;
  records: DevRecording[];
  error: string | null;
  busy: boolean;
  start: (profile: DevRecordingRouteProfile, duration: 5 | 10 | 30 | null) => Promise<void>;
  stop: () => Promise<void>;
  deleteRecording: (recordingId: string) => Promise<void>;
};

function useDevRecordingController(includeRecords: boolean): RecordingController {
  const [status, setStatus] = useState<DevRecordingStatus | null>(null);
  const [records, setRecords] = useState<DevRecording[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const refreshStatus = useCallback(async () => {
    try { setStatus(await getDevRecordingStatus()); setError(null); } catch (reason) { setError(reason instanceof Error ? reason.message : "录制状态读取失败"); }
  }, []);
  const refreshRecords = useCallback(async () => {
    if (!includeRecords) return;
    try { setRecords(await listDevRecordings()); } catch (reason) { setError(reason instanceof Error ? reason.message : "录制历史读取失败"); }
  }, [includeRecords]);
  useEffect(() => {
    void refreshStatus();
    void refreshRecords();
    const timer = window.setInterval(() => void refreshStatus(), 1000);
    return () => window.clearInterval(timer);
  }, [refreshRecords, refreshStatus]);
  const startRecording = async (profile: DevRecordingRouteProfile, duration: 5 | 10 | 30 | null) => {
    if (busy || status?.active) return;
    setBusy(true); setError(null);
    try { setStatus(await startDevRecording(profile, duration)); } catch (reason) { setError(reason instanceof Error ? reason.message : "录制启动失败"); } finally { setBusy(false); }
  };
  const stopRecording = async () => {
    if (busy || !status?.active) return;
    setBusy(true); setError(null);
    try { setStatus(await stopDevRecording()); await refreshRecords(); } catch (reason) { setError(reason instanceof Error ? reason.message : "停止录制失败"); } finally { setBusy(false); }
  };
  const deleteRecording = async (recordingId: string) => {
    if (busy) return;
    if (!window.confirm("确定删除该开发录制吗？此操作不可恢复。")) return;
    setBusy(true); setError(null);
    try { await deleteDevRecording(recordingId); await refreshRecords(); } catch (reason) { setError(reason instanceof Error ? reason.message : "删除失败"); } finally { setBusy(false); }
  };
  return { status, records, error, busy, start: startRecording, stop: stopRecording, deleteRecording };
}

function RecordingControl({ controller, profile, title, description, compact = false }: { controller: RecordingController; profile: DevRecordingRouteProfile; title: string; description: string; compact?: boolean }) {
  const { status, records, error, busy } = controller;
  const ownRecords = records.filter((record) => record.profile === storedProfileName[profile]);
  const visibleRecords = compact ? ownRecords : ownRecords.slice(0, 10);
  const recordingBusy = busy || status?.active === true;
  return <section className={`panel dev-panel ${compact ? "dev-recording-compact" : ""}`}>
    <div className="panel-head"><div><h2>{title}</h2><p>{description}</p></div><div className={`dev-state ${status?.active ? "dev-state--streaming" : "dev-state--waiting"}`}>{busy ? "处理中" : status?.active ? `录制中 ${status.elapsed_seconds.toFixed(1)} s` : "空闲"}</div></div>
    <div className="dev-record-actions"><button className="button" disabled={recordingBusy} onClick={() => void controller.start(profile, 5)}>记录 5 秒</button><button className="button" disabled={recordingBusy} onClick={() => void controller.start(profile, 10)}>记录 10 秒</button><button className="button" disabled={recordingBusy} onClick={() => void controller.start(profile, 30)}>记录 30 秒</button>{profile !== "diagnostic" && <button className="button" disabled={recordingBusy} onClick={() => void controller.start(profile, null)}>连续记录</button>}<button className="button button--danger-outline" disabled={busy || !status?.active} onClick={() => void controller.stop()}>停止</button></div>
    <div className="dev-metric-grid dev-metric-grid--4"><Metric label="当前文件" value={status?.recording_id ?? "--"} detail={status?.path ?? ""} /><Metric label="已写入" value={bytesText(status?.bytes)} /><Metric label="剩余空间" value={bytesText(status?.free_bytes)} /><Metric label="参数快照" value={status?.parameter_snapshot_complete === true ? "完整" : status?.parameter_snapshot_complete === false ? "有缺项" : "写入中"} detail="录制启动不等待参数快照，快照使用常驻ROS参数桥的最近真实值" /></div>
    {error && <div className="dev-message dev-message--error"><strong>{error}</strong></div>}
    {visibleRecords.length > 0 && <div className={`dev-record-list${compact ? " dev-record-list--compact" : ""}`}>{visibleRecords.map((record) => <div key={record.recording_id}><div><strong>{record.recording_id}</strong><span>{bytesText(record.bytes)} · {new Date(record.modified_at_ns / 1_000_000).toLocaleString("zh-CN")} · 参数快照 {record.parameter_snapshot_complete === true ? "完整" : record.parameter_snapshot_complete === false ? "有缺项" : "未记录"}</span></div><button className="button button--quiet" disabled={record.active || busy} onClick={() => void controller.deleteRecording(record.recording_id)}>删除</button></div>)}</div>}
  </section>;
}

function RecordingPanel({ overview }: { overview: DevOverview | null }) {
  const recordingController = useDevRecordingController(true);
  const [control, setControl] = useState<TaskControlReadiness | null>(null);
  const [error, setError] = useState<string | null>(null);
  const refreshControl = useCallback(async () => { try { setControl(await getTaskControlReadiness()); setError(null); } catch (reason) { setError(reason instanceof Error ? reason.message : "任务控制状态读取失败"); } }, []);
  useEffect(() => { void refreshControl(); const timer = window.setInterval(() => void refreshControl(), 1000); return () => window.clearInterval(timer); }, [refreshControl]);
  const task = overview?.telemetry.topics.task_status;
  const recording = overview?.telemetry.topics.recording_status;
  const services = control?.services;
  return <div className="dev-section-stack">
    <section className="panel dev-panel"><div className="panel-head"><div><h2>任务控制链路</h2><p>{control?.detail ?? error ?? "正在读取"}</p></div><button className="button" onClick={() => void refreshControl()}>刷新Service状态</button></div>
      <div className="dev-service-grid">{(["start", "pause", "resume", "stop", "recover"] as const).map((name) => <div key={name}><span>{name}</span><strong className={services?.[name] ? "is-ready" : "is-blocked"}>{services?.[name] ? "可用" : "不可用"}</strong></div>)}</div>
      <div className="dev-metric-grid dev-metric-grid--4"><Metric label="活动任务" value={control?.activeTaskId ?? task?.task_id ?? "--"} /><Metric label="内部阶段" value={control?.activePhase ?? task?.operation_phase ?? "--"} /><Metric label="状态版本" value={task?.status_revision ?? "--"} /><Metric label="记录器" value={recording?.status ?? "--"} detail={String(recording?.message ?? "")} /></div>
    </section>
    <RecordingControl controller={recordingController} profile="raw-sensor" title="原始传感器记录" description="按ROS原始发布频率记录点云、IMU、原始高频里程计、SLAM里程计和雷达上下线事件。当前不录制视觉和传感器内部温度。" />
    <RecordingControl controller={recordingController} profile="algorithm-debug" title="算法诊断记录" description="记录时间适配里程计、运动补偿点云、净空、RTK、任务、记录器和系统诊断，不创建正式任务。" />
    <RecordingControl controller={recordingController} profile="full-debug" title="完整开发记录" description="同时记录当前原始传感器链和算法处理链，ROS Topic不经浏览器且不降频。" />
  </div>;
}

function ParameterSubset({ prefix }: { prefix: string }) {
  return <ParameterPanel embeddedPrefix={prefix} />;
}

function ParameterPanel({ embeddedPrefix }: { embeddedPrefix?: string }) {
  const [parameters, setParameters] = useState<DevParameter[]>([]);
  const [editing, setEditing] = useState<Record<string, string>>({});
  const [error, setError] = useState<string | null>(null);
  const refresh = useCallback(async () => { try { setParameters(await getDevParameters()); setError(null); } catch (reason) { setError(reason instanceof Error ? reason.message : "参数读取失败"); } }, []);
  useEffect(() => { void refresh(); }, [refresh]);
  const visible = parameters.filter((parameter) => !embeddedPrefix || parameter.key.startsWith(embeddedPrefix));
  const apply = async (parameter: DevParameter) => {
    const raw = editing[parameter.key] ?? String(parameter.value ?? "");
    const value = parameter.kind === "bool" ? raw === "true" : parameter.kind === "int" ? Number.parseInt(raw, 10) : Number.parseFloat(raw);
    try { await setDevParameter(parameter.key, value); setEditing((current) => ({ ...current, [parameter.key]: "" })); await refresh(); } catch (reason) { setError(reason instanceof Error ? reason.message : "参数设置失败"); }
  };
  const formatValue = (value: DevParameter["value"], unit: string) => value === null || value === undefined ? "--" : `${String(value)}${unit ? ` ${unit}` : ""}`;
  return <section className="panel dev-panel"><div className="panel-head"><div><h2>{embeddedPrefix ? "当前实际参数" : "核心参数装订"}</h2><p>正式配置值来自所属YAML，运行值由常驻ROS 2参数桥后台批量读取并缓存。节点不可用时不以配置值代替运行值；可写项只修改当前ROS节点，重启后恢复YAML值。</p></div><button className="button" onClick={() => void refresh()}>重新读取</button></div>
    {error && <div className="dev-message dev-message--error"><strong>{error}</strong></div>}
    <div className="dev-parameter-list">{visible.map((parameter) => <div key={parameter.key} className={!parameter.available ? "is-unavailable" : ""}><div><strong>{parameter.label}</strong><span>{parameter.node} · {parameter.parameter}</span><small>{parameter.note}{parameter.source_config ? ` · ${parameter.source_config}` : ""}</small></div><div className="dev-parameter-values"><div><small>配置值</small><strong>{parameter.config_available ? formatValue(parameter.configured_value, parameter.unit) : "--"}</strong>{!parameter.config_available && <span title={parameter.config_detail}>配置不可用</span>}</div><div><small>运行值</small>{parameter.writable ? <div className="dev-parameter-control"><input disabled={!parameter.available} value={editing[parameter.key] ?? String(parameter.value ?? "")} placeholder="--" onChange={(event) => setEditing((current) => ({ ...current, [parameter.key]: event.target.value }))} /><span>{parameter.unit}</span><button className="button button--quiet" disabled={!parameter.available} onClick={() => void apply(parameter)}>应用</button></div> : <strong>{parameter.available ? formatValue(parameter.value, parameter.unit) : "--"}</strong>}{!parameter.available && <span title={parameter.detail}>节点不可用</span>}</div></div></div>)}</div>
  </section>;
}

export default function DevToolsWorkspace() {
  const [tab, setTab] = useState<DevTab>("overview");
  const { overview, error } = useDevOverview();
  const content = useMemo(() => {
    if (tab === "overview") return <OverviewPanel overview={overview} />;
    if (tab === "lidar") return <LidarPanel overview={overview} />;
    if (tab === "motion") return <MotionPanel overview={overview} />;
    if (tab === "rtk") return <RtkPanel overview={overview} />;
    if (tab === "clearance") return <ClearancePanel overview={overview} />;
    if (tab === "recording") return <RecordingPanel overview={overview} />;
    return <ParameterPanel />;
  }, [tab, overview]);
  return <div className="devtools-workspace">
    <section className="dev-banner"><div><strong>开发测试版本</strong><span>测试功能只存在于 development 构建，客户版本不注册开发接口。</span></div><span>{overview?.version ?? "--"}</span></section>
    <div className="dev-tabs" role="tablist">{tabs.map((item) => <button key={item.id} type="button" role="tab" aria-selected={tab === item.id} className={tab === item.id ? "active" : ""} onClick={() => setTab(item.id)}>{item.label}</button>)}</div>
    {error && <div className="dev-message dev-message--error"><strong>{error}</strong><span>确认使用 development 构建并启动FastAPI开发接口。</span></div>}
    {content}
  </div>;
}
