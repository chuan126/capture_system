"use client";

import { useCallback, useEffect, useMemo, useState } from "react";

import { useClearanceSocket, type ClearanceConnectionState } from "@/components/clearance/useClearanceSocket";
import { rtkSolutionLabel } from "@/components/rtk/localizationView";
import { useRtkSocket, type RtkConnectionState } from "@/components/rtk/useRtkSocket";

import {
  deleteDevRecording,
  getDevOfflineReplayStatus,
  getDevParameters,
  getDevRecordingStatus,
  listDevRecordings,
  setDevParameter,
  startDevOfflineReplay,
  startDevRecording,
  stopDevOfflineReplay,
  stopDevRecording,
  type DevOfflineReplayStatus,
  type DevParameter,
  type DevRecording,
  type DevRecordingStatus,
} from "./devtoolsApi";

const bytesText = (value: number | null | undefined) => {
  const bytes = Math.max(0, value ?? 0);
  if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(2)} GiB`;
  if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${bytes} B`;
};

const fixed = (value: unknown, digits = 2) =>
  typeof value === "number" && Number.isFinite(value) ? value.toFixed(digits) : "--";

function Metric({ label, value, detail }: { label: string; value: React.ReactNode; detail?: React.ReactNode }) {
  return <div className="dev-metric"><span>{label}</span><strong>{value}</strong>{detail && <small>{detail}</small>}</div>;
}

function StatusChip({ label, value, tone = "idle" }: { label: string; value: string; tone?: "ok" | "warn" | "danger" | "idle" }) {
  return <div className={`dev-status-chip dev-status-chip--${tone}`}><span>{label}</span><strong>{value}</strong></div>;
}

function useRawCloudRecording() {
  const [status, setStatus] = useState<DevRecordingStatus | null>(null);
  const [records, setRecords] = useState<DevRecording[]>([]);
  const [pollError, setPollError] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const refreshRecords = useCallback(async () => {
    try {
      const items = await listDevRecordings();
      setRecords(items.filter((record) => record.profile === "raw_cloud"));
      setPollError(null);
    } catch (reason) {
      setPollError(reason instanceof Error ? reason.message : "综合测试样本读取失败");
    }
  }, []);

  const refreshStatus = useCallback(async () => {
    try {
      setStatus(await getDevRecordingStatus());
      setPollError(null);
    } catch (reason) {
      setPollError(reason instanceof Error ? reason.message : "录制状态读取失败");
    }
  }, []);

  useEffect(() => {
    void refreshStatus();
    void refreshRecords();
    const timer = window.setInterval(() => void refreshStatus(), 1000);
    return () => window.clearInterval(timer);
  }, [refreshRecords, refreshStatus]);

  const start = async () => {
    if (busy || status?.active) return;
    setBusy(true);
    setActionError(null);
    setMessage("正在启动完整测试数据保存…");
    try {
      const next = await startDevRecording("raw-cloud", null);
      setStatus(next);
      setMessage(next.active ? `已开始保存：${next.recording_id ?? "新样本"}` : "录制进程未进入活动状态");
    } catch (reason) {
      setActionError(reason instanceof Error ? reason.message : "综合测试数据保存启动失败");
      setMessage(null);
    } finally {
      setBusy(false);
    }
  };

  const stop = async () => {
    if (busy || !status?.active) return;
    setBusy(true);
    setActionError(null);
    setMessage("正在停止并整理分组文件…");
    try {
      const next = await stopDevRecording();
      setStatus(next);
      await refreshRecords();
      setMessage("保存完成，分组MCAP已写入综合测试样本");
    } catch (reason) {
      setActionError(reason instanceof Error ? reason.message : "停止保存失败");
      setMessage(null);
    } finally {
      setBusy(false);
    }
  };

  const remove = async (recordingId: string) => {
    if (busy) return;
    setBusy(true);
    setActionError(null);
    setMessage(null);
    try {
      await deleteDevRecording(recordingId);
      await refreshRecords();
      setMessage(`已删除综合测试样本：${recordingId}`);
    } catch (reason) {
      setActionError(reason instanceof Error ? reason.message : "删除综合测试样本失败");
    } finally {
      setBusy(false);
    }
  };

  return {
    status,
    records,
    error: actionError ?? pollError ?? status?.last_error ?? null,
    message,
    busy,
    start,
    stop,
    remove,
    refreshRecords,
  };
}

function useOfflineReplay() {
  const [status, setStatus] = useState<DevOfflineReplayStatus | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const refresh = useCallback(async () => {
    try {
      setStatus(await getDevOfflineReplayStatus());
      setError(null);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "离线检测状态读取失败");
    }
  }, []);

  useEffect(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), 750);
    return () => window.clearInterval(timer);
  }, [refresh]);

  const start = async (recordingId: string) => {
    if (busy || status?.active) return;
    setBusy(true);
    setError(null);
    try {
      setStatus(await startDevOfflineReplay(recordingId));
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "离线算法检测启动失败");
    } finally {
      setBusy(false);
    }
  };

  const stop = async () => {
    if (busy || !status?.active) return;
    setBusy(true);
    setError(null);
    try {
      setStatus(await stopDevOfflineReplay());
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "停止离线算法检测失败");
    } finally {
      setBusy(false);
    }
  };

  return { status, error, busy, start, stop };
}

function PositionPanel({ rtk }: { rtk: RtkConnectionState }) {
  const snapshot = rtk.snapshot;
  const streamAvailable = rtk.connection === "connected" && rtk.streamState === "streaming";
  const rawFix = rtkSolutionLabel(snapshot?.gps_state);
  const rtkTone = !streamAvailable ? "idle" : snapshot?.gps_state === 4 ? "ok" : snapshot?.gps_state === 5 ? "warn" : "danger";
  const hasPosition = streamAvailable && snapshot?.fix_status !== -1 &&
    snapshot?.rmc_validity !== "V".charCodeAt(0) &&
    typeof snapshot?.latitude === "number" && Number.isFinite(snapshot.latitude) &&
    typeof snapshot.longitude === "number" && Number.isFinite(snapshot.longitude);

  return <section className="panel dev-dashboard-card">
    <div className="panel-head"><div><h2>RTK定位</h2><p>显示RTK接收机原始解算状态与坐标，不使用里程计融合回退。</p></div><span className={`status-pill status-pill--${rtkTone}`}>{rawFix}</span></div>
    <div className="dev-metric-grid dev-metric-grid--4">
      <Metric label="卫星" value={snapshot?.satellite_count ?? "--"} />
      <Metric label="HDOP" value={fixed(snapshot?.hdop, 2)} detail={`PDOP ${fixed(snapshot?.pdop, 2)}`} />
      <Metric label="串口" value={snapshot?.serial_connected === true ? "正常" : snapshot?.serial_connected === false ? "异常" : "等待"} detail={snapshot?.serial_message ?? rtk.detail} />
      <Metric label="航向" value={hasPosition && typeof snapshot?.track_degrees === "number" ? `${fixed(snapshot.track_degrees, 2)}°` : "--"} />
    </div>
    <div className="dev-card-divider" />
    <div className="dev-card-subhead"><strong>原始坐标</strong><span>{hasPosition ? "有效" : "等待有效定位"}</span></div>
    <div className="dev-position-grid">
      <div><span>纬度</span><strong>{hasPosition ? fixed(snapshot?.latitude, 7) : "--"}</strong></div>
      <div><span>经度</span><strong>{hasPosition ? fixed(snapshot?.longitude, 7) : "--"}</strong></div>
      <div><span>高度</span><strong>{hasPosition ? `${fixed(snapshot?.altitude, 2)} m` : "--"}</strong></div>
    </div>
    <div className="dev-card-footnote">
      <span>速度 {hasPosition && typeof snapshot?.speed_knots === "number" ? `${fixed(snapshot.speed_knots * 0.514444, 2)} m/s` : "--"}</span>
      <span>RMC {snapshot?.rmc_validity ? String.fromCharCode(snapshot.rmc_validity) : "--"}</span>
    </div>
  </section>;
}

function ClearancePanel({ clearance }: { clearance: ClearanceConnectionState }) {
  const snapshot = clearance.snapshot;
  const streaming = clearance.connection === "connected" && clearance.streamState === "streaming";
  const valid = streaming && snapshot?.valid === true && snapshot.lidar_to_top_m !== null;
  const tone = valid ? "ok" : snapshot && streaming ? "danger" : "idle";
  const status = valid ? "有效" : snapshot && streaming ? "无效" : "等待";
  const reason = !streaming ? "等待净空数据" : valid ? "当前帧通过质量检查" : snapshot?.invalid_reason || "当前帧无效";

  return <section className="panel dev-dashboard-card dev-clearance-card">
    <div className="panel-head"><div><h2>净空算法</h2><p>显示原始雷达本体系最低可信簇距离和当前帧质量，不叠加任务安装高度。</p></div><span className={`status-pill status-pill--${tone}`}>{status}</span></div>
    <div className="dev-clearance-primary">
      <span>当前雷达到顶距离</span>
      <strong>{snapshot?.lidar_to_top_m == null ? "--" : `${fixed(snapshot.lidar_to_top_m, 3)} m`}</strong>
      <small className="dev-clearance-reason" title={reason}>{reason}</small>
    </div>
    <div className="dev-metric-grid dev-metric-grid--3">
      <Metric label="可信点簇" value={snapshot?.candidate_count ?? "--"} />
      <Metric label="最低簇点数" value={snapshot?.selected_inlier_count ?? "--"} />
      <Metric label="有效点比例" value={snapshot?.valid_point_ratio == null ? "--" : `${fixed(snapshot.valid_point_ratio * 100, 1)}%`} />
      <Metric label="处理时间" value={snapshot?.processing_time_ms == null ? "--" : `${fixed(snapshot.processing_time_ms, 2)} ms`} />
      <Metric label="原始帧" value={snapshot?.frame_id || "--"} />
      <Metric label="算法状态" value={snapshot?.valid ? "可信" : "无效"} />
    </div>
    <div className="dev-clearance-footnote" title={snapshot?.frame_id || "--"}>源帧 {snapshot?.frame_id || "--"}</div>
  </section>;
}

const offlineStateText = (state: DevOfflineReplayStatus["state"] | undefined) => {
  switch (state) {
    case "starting": return "启动中";
    case "running": return "检测中";
    case "stopping": return "停止中";
    case "completed": return "已完成";
    case "stopped": return "已停止";
    case "failed": return "失败";
    default: return "空闲";
  }
};

function RawCloudPanel({
  controller,
  offline,
  selectedId,
  onSelect,
}: {
  controller: ReturnType<typeof useRawCloudRecording>;
  offline: ReturnType<typeof useOfflineReplay>;
  selectedId: string | null;
  onSelect: (recordingId: string) => void;
}) {
  const { status, records, error, message, busy } = controller;
  const recordingActive = status?.active === true;
  const activeRawCloud = recordingActive && status.profile === "raw_cloud";
  const blockedByOtherProfile = recordingActive && status.profile !== "raw_cloud";
  const selected = records.find((record) => record.recording_id === selectedId) ?? null;
  const offlineActive = offline.status?.active === true;

  const removeSelected = async () => {
    if (!selected || busy || offlineActive) return;
    if (!window.confirm(`确认删除综合测试样本 ${selected.recording_id}？删除后不可恢复。`)) return;
    await controller.remove(selected.recording_id);
  };

  return <section className="panel dev-dashboard-card dev-raw-cloud-card">
    <div className="panel-head"><div><h2>保存完整测试数据</h2><p>每次保存生成一个时间目录，原始点云、400 Hz雷达状态、RTK、算法结果和诊断按频率/职责拆分为独立MCAP，并保留消息时间戳。</p></div><span className={`dev-inline-state ${activeRawCloud ? "dev-inline-state--ok" : blockedByOtherProfile ? "dev-inline-state--warn" : ""}`}>{busy ? "处理中" : activeRawCloud ? `保存中 ${status?.elapsed_seconds.toFixed(1)} s` : blockedByOtherProfile ? "其他开发录制占用" : "空闲"}</span></div>
    <div className="dev-record-actions">
      <button type="button" className="button" disabled={busy || status?.active === true || offlineActive} onClick={() => void controller.start()}>保存</button>
      <button type="button" className="button button--danger-outline" disabled={busy || !activeRawCloud} onClick={() => void controller.stop()}>停止</button>
      <button type="button" className="button button--quiet" disabled={busy || !selected || activeRawCloud || offlineActive} onClick={() => void removeSelected()}>删除</button>
    </div>
    {message && <div className="dev-message dev-message--ok" role="status"><strong>{message}</strong></div>}
    {error && <div className="dev-message dev-message--error" role="alert"><strong>{error}</strong></div>}
    <div className="dev-metric-grid dev-metric-grid--3">
      <Metric label="当前文件" value={activeRawCloud ? status?.recording_id ?? "--" : "--"} detail={activeRawCloud ? status?.path ?? "" : undefined} />
      <Metric label="已写入" value={activeRawCloud ? bytesText(status?.bytes) : "--"} />
      <Metric label="剩余空间" value={status ? bytesText(status.free_bytes) : "--"} />
    </div>
    <div className="dev-card-divider" />
    <div className="dev-card-subhead"><strong>综合测试样本</strong><span>{records.length} 个</span></div>
    <div className="dev-sample-list" role="listbox" aria-label="综合测试样本">
      {records.length === 0 && <div className="dev-sample-empty">暂无已保存样本</div>}
      {records.map((record) => <button
        type="button"
        role="option"
        aria-selected={record.recording_id === selectedId}
        key={record.recording_id}
        className={`dev-sample-row${record.recording_id === selectedId ? " is-selected" : ""}`}
        onClick={() => onSelect(record.recording_id)}
      >
        <span><strong>{record.recording_id}</strong><small>{new Date(record.modified_at_ns / 1_000_000).toLocaleString("zh-CN")} · {bytesText(record.bytes)} · {record.layout === "frequency_split_mcap" ? `${record.file_count}个分组文件` : "旧版单MCAP"}</small></span>
        <span className={`dev-sample-ready${record.replay_ready ? " is-ready" : ""}`}>{record.replay_ready ? "可离线检测" : "旧样本缺辅助里程计"}</span>
      </button>)}
    </div>
  </section>;
}

function OfflineReplayPanel({
  controller,
  offline,
  selected,
}: {
  controller: ReturnType<typeof useRawCloudRecording>;
  offline: ReturnType<typeof useOfflineReplay>;
  selected: DevRecording | null;
}) {
  const recordingActive = controller.status?.active === true;
  const offlineActive = offline.status?.active === true;
  const progress = offline.status?.progress == null ? "--" : `${Math.round(offline.status.progress * 100)}%`;
  const replayState = offlineStateText(offline.status?.state);
  const replayTone = offline.status?.state === "completed" ? "ok" : offline.status?.state === "failed" ? "danger" : offlineActive ? "warn" : "";
  const lastClearanceDetail = offline.status?.latest_result_valid === false
    ? `最后有效值 · 当前帧无效${offline.status.invalid_reason ? ` · ${offline.status.invalid_reason}` : ""}`
    : offline.status?.invalid_reason && offline.status.lidar_to_top_last_m == null
      ? offline.status.invalid_reason
      : undefined;

  return <section className="panel dev-dashboard-card dev-offline-card">
    <div className="panel-head"><div><h2>离线算法调试</h2><p>选中右侧保存的样本，以原记录时序直接回放原始点云并运行正式净空算法，全部 Topic 使用 `/capture/dev/offline/*` 隔离。</p></div><span className={`dev-inline-state${replayTone ? ` dev-inline-state--${replayTone}` : ""}`}>{replayState}</span></div>
    <div className="dev-record-actions">
      <button className="button" disabled={offline.busy || offlineActive || recordingActive || !selected?.replay_ready} onClick={() => selected && void offline.start(selected.recording_id)}>开始检测</button>
      <button className="button button--danger-outline" disabled={offline.busy || !offlineActive} onClick={() => void offline.stop()}>停止检测</button>
    </div>
    <div className="dev-metric-grid dev-metric-grid--3">
      <Metric label="进度" value={progress} detail={offline.status?.recording_id ?? selected?.recording_id ?? "--"} />
      <Metric label="处理帧" value={offline.status?.processed_frames ?? 0} detail={`有效 ${offline.status?.valid_frames ?? 0} · 无效 ${offline.status?.invalid_frames ?? 0}`} />
      <Metric label="最低簇点数" value={offline.status?.cluster_point_count_last ?? "--"} detail={offline.status?.cluster_point_count_mean == null ? "均值 --" : `均值 ${offline.status.cluster_point_count_mean.toFixed(1)} · 最大 ${offline.status.cluster_point_count_max ?? "--"}`} />
      <Metric label="雷达到顶距离" value={offline.status?.lidar_to_top_last_m == null ? "--" : `${fixed(offline.status.lidar_to_top_last_m, 3)} m`} detail={lastClearanceDetail} />
      <Metric label="单帧处理时间" value={offline.status?.processing_time_ms_last == null ? "--" : `${fixed(offline.status.processing_time_ms_last, 2)} ms`} />
      <Metric label="参数来源" value={offline.status?.parameter_fallback_keys?.length ? "部分回退YAML" : offline.status?.recording_id ? "当前运行值" : "--"} detail={offline.status?.parameter_fallback_keys?.length ? offline.status.parameter_fallback_keys.join(", ") : undefined} />
    </div>
    {offline.status?.state === "completed" && <div className="dev-card-footnote"><span>最低 {offline.status.lidar_to_top_min_m == null ? "--" : `${fixed(offline.status.lidar_to_top_min_m, 3)} m`}</span><span>平均 {offline.status.lidar_to_top_mean_m == null ? "--" : `${fixed(offline.status.lidar_to_top_mean_m, 3)} m`}</span><span>最高 {offline.status.lidar_to_top_max_m == null ? "--" : `${fixed(offline.status.lidar_to_top_max_m, 3)} m`}</span></div>}
    {offline.error && <div className="dev-message dev-message--error"><strong>{offline.error}</strong></div>}
    {offline.status?.last_error && <div className="dev-message dev-message--error"><strong>{offline.status.last_error}</strong></div>}
  </section>;
}

const CLEARANCE_PARAMETER_KEYS = [
  "clearance.min_detection_x_m",
  "clearance.detection_radius_m",
  "clearance.support_height_band_m",
  "clearance.min_support_points",
  "clearance.spatial_grid_size_m",
  "clearance.min_occupied_cells",
  "clearance.min_spatial_span_m",
] as const;

const formatParameterValue = (parameter: DevParameter, value: DevParameter["value"]) =>
  value === null || value === undefined ? "--" : `${String(value)}${parameter.unit ? ` ${parameter.unit}` : ""}`;

const parameterMismatch = (parameter: DevParameter) => {
  if (!parameter.available || !parameter.config_available) return false;
  if (typeof parameter.value === "number" && typeof parameter.configured_value === "number") {
    return Math.abs(parameter.value - parameter.configured_value) > 1e-9;
  }
  return parameter.value !== parameter.configured_value;
};

function ParameterRow({ parameter, onOpen }: { parameter: DevParameter; onOpen: (parameter: DevParameter) => void }) {
  const mismatch = parameterMismatch(parameter);
  const stateText = !parameter.available ? "节点不可用" : mismatch ? "运行值不一致" : parameter.writable ? null : "只读";
  return <button type="button" className={`dev-config-row${!parameter.available ? " is-unavailable" : ""}${mismatch ? " is-mismatch" : ""}`} onClick={() => onOpen(parameter)}>
    <span><strong>{parameter.label}</strong><small>{parameter.parameter}</small></span>
    <span className="dev-config-values"><small>配置</small><strong>{parameter.config_available ? formatParameterValue(parameter, parameter.configured_value) : "--"}</strong></span>
    <span className="dev-config-values"><small>运行</small><strong>{parameter.available ? formatParameterValue(parameter, parameter.value) : "--"}</strong></span>
    {stateText && <span className={`dev-config-state${mismatch ? " is-mismatch" : ""}`}>{stateText}</span>}
  </button>;
}

function ParameterDialog({ parameter, onClose, onApplied }: { parameter: DevParameter; onClose: () => void; onApplied: () => Promise<void> }) {
  const [value, setValue] = useState(String(parameter.value ?? parameter.configured_value ?? ""));
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const apply = async () => {
    if (!parameter.writable || !parameter.available || busy) return;
    let parsed: number | boolean;
    if (parameter.kind === "bool") {
      if (value !== "true" && value !== "false") {
        setError("布尔参数只能填写 true 或 false");
        return;
      }
      parsed = value === "true";
    } else {
      const numeric = Number(value);
      if (!Number.isFinite(numeric) || (parameter.kind === "int" && !Number.isInteger(numeric))) {
        setError(parameter.kind === "int" ? "请输入有效整数" : "请输入有效数值");
        return;
      }
      if (parameter.minimum !== null && numeric < parameter.minimum) {
        setError(`参数不能小于 ${parameter.minimum}`);
        return;
      }
      if (parameter.maximum !== null && numeric > parameter.maximum) {
        setError(`参数不能大于 ${parameter.maximum}`);
        return;
      }
      parsed = numeric;
    }
    setBusy(true);
    setError(null);
    try {
      await setDevParameter(parameter.key, parsed);
      await onApplied();
      onClose();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "参数设置失败");
    } finally {
      setBusy(false);
    }
  };

  return <div className="dev-config-modal" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose(); }}>
    <section role="dialog" aria-modal="true" aria-labelledby="dev-parameter-title" className="panel dev-config-dialog">
      <div className="panel-head"><div><h2 id="dev-parameter-title">{parameter.label}</h2><p>{parameter.node} · {parameter.parameter}</p></div><button className="button button--quiet" onClick={onClose}>关闭</button></div>
      <div className="dev-config-detail-grid">
        <Metric label="正式配置值" value={parameter.config_available ? formatParameterValue(parameter, parameter.configured_value) : "--"} detail={parameter.source_config || parameter.config_detail} />
        <Metric label="当前运行值" value={parameter.available ? formatParameterValue(parameter, parameter.value) : "--"} detail={parameter.available ? "ROS 2参数桥最近读取值" : parameter.detail} />
        <Metric label="允许范围" value={parameter.minimum == null && parameter.maximum == null ? "--" : `${parameter.minimum ?? "-∞"} ～ ${parameter.maximum ?? "+∞"}${parameter.unit ? ` ${parameter.unit}` : ""}`} />
        <Metric label="生效方式" value={parameter.writable ? "运行时动态修改" : "启动时读取"} detail={parameter.note} />
      </div>
      {parameter.writable && <div className="dev-config-editor"><label><span>设置当前运行值</span><input disabled={!parameter.available || busy} value={value} onChange={(event) => setValue(event.target.value)} /></label><span>{parameter.unit}</span><button className="button" disabled={!parameter.available || busy} onClick={() => void apply()}>{busy ? "应用中" : "应用"}</button></div>}
      {error && <div className="dev-message dev-message--error"><strong>{error}</strong></div>}
    </section>
  </div>;
}

function ConfigPanel() {
  const [parameters, setParameters] = useState<DevParameter[]>([]);
  const [selected, setSelected] = useState<DevParameter | null>(null);
  const [error, setError] = useState<string | null>(null);
  const refresh = useCallback(async () => {
    try {
      setParameters(await getDevParameters());
      setError(null);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "核心参数读取失败");
    }
  }, []);
  useEffect(() => { void refresh(); }, [refresh]);
  const byKey = useMemo(() => new Map(parameters.map((parameter) => [parameter.key, parameter])), [parameters]);
  const clearance = CLEARANCE_PARAMETER_KEYS.map((key) => byKey.get(key)).filter((item): item is DevParameter => Boolean(item));

  return <section className="panel dev-dashboard-card dev-config-card">
    <div className="panel-head"><div><h2>核心配置</h2><p>主界面只读显示正式YAML值和ROS运行值。运行值与配置不一致时单独标记。</p></div><button className="button button--quiet" onClick={() => void refresh()}>重新读取</button></div>
    {error && <div className="dev-message dev-message--error"><strong>{error}</strong></div>}
    <div className="dev-config-group"><h3>净空算法</h3>{clearance.map((parameter) => <ParameterRow key={parameter.key} parameter={parameter} onOpen={setSelected} />)}</div>
    {selected && <ParameterDialog parameter={selected} onClose={() => setSelected(null)} onApplied={refresh} />}
  </section>;
}

export default function DevToolsWorkspace() {
  const rtk = useRtkSocket();
  const clearance = useClearanceSocket();
  const recording = useRawCloudRecording();
  const offline = useOfflineReplay();
  const clearanceStreaming = clearance.connection === "connected" && clearance.streamState === "streaming";
  const clearanceValid = clearanceStreaming && clearance.snapshot?.valid === true && clearance.snapshot.lidar_to_top_m !== null;
  const rawRecording = recording.status?.active === true && recording.status.profile === "raw_cloud";
  const [selectedRecordingId, setSelectedRecordingId] = useState<string | null>(null);
  useEffect(() => {
    if (selectedRecordingId && recording.records.some((record) => record.recording_id === selectedRecordingId)) return;
    setSelectedRecordingId(recording.records[0]?.recording_id ?? null);
  }, [recording.records, selectedRecordingId]);
  const selectedRecording = recording.records.find((record) => record.recording_id === selectedRecordingId) ?? null;

  return <div className="devtools-workspace dev-dashboard">
    <section className="dev-banner"><div><strong>开发测试</strong><span>单页显示现场调试核心状态。客户版本不注册本页面和开发接口。</span></div></section>
    <section className="dev-status-strip" aria-label="开发测试核心状态">
      <StatusChip label="RTK" value={rtkSolutionLabel(rtk.snapshot?.gps_state)} tone={rtk.streamState !== "streaming" ? "idle" : rtk.snapshot?.gps_state === 4 ? "ok" : rtk.snapshot?.gps_state === 5 ? "warn" : "danger"} />
      <StatusChip label="净空" value={clearanceValid ? "有效" : clearanceStreaming ? "无效" : "等待"} tone={clearanceValid ? "ok" : clearanceStreaming ? "danger" : "idle"} />
      <StatusChip label="原始点云录制" value={rawRecording ? `录制中 ${recording.status?.elapsed_seconds.toFixed(1)} s` : recording.status?.active ? "其他录制占用" : "空闲"} tone={rawRecording ? "ok" : recording.status?.active ? "warn" : "idle"} />
    </section>
    <div className="dev-dashboard-grid">
      <div className="dev-dashboard-left">
        <ConfigPanel />
        <OfflineReplayPanel controller={recording} offline={offline} selected={selectedRecording} />
      </div>
      <div className="dev-dashboard-right">
        <ClearancePanel clearance={clearance} />
        <PositionPanel rtk={rtk} />
        <RawCloudPanel controller={recording} offline={offline} selectedId={selectedRecordingId} onSelect={setSelectedRecordingId} />
      </div>
    </div>
  </div>;
}
