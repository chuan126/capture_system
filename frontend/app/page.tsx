"use client";

import { useEffect, useMemo, useState } from "react";

import { useClearanceSocket } from "@/components/clearance/useClearanceSocket";
import type { ClearanceSnapshot } from "@/components/clearance/clearanceProtocol";
import RealtimeAmap from "@/components/map/RealtimeAmap";
import PointCloudViewer from "@/components/point-cloud/PointCloudViewer";
import type { PointCloudViewMode } from "@/components/point-cloud/PointCloudViewer";
import { useRtkSocket } from "@/components/rtk/useRtkSocket";
import { useSystemStatusSocket } from "@/components/system-status/useSystemStatusSocket";
import type { DeviceStatus, HealthState } from "@/components/system-status/systemStatusProtocol";

type PageId = "dashboard" | "tasks" | "playback" | "report";
type PlaybackTab = "result" | "history" | "logs";
type TaskTab = "pending" | "completed" | "abnormal";
type ViewMode = "3d" | "top" | "section";
type ReportLoadState = "idle" | "loading" | "ready" | "error";

type ReportTestData = {
  task_id: string;
  task_name: string;
  tunnel_name: string;
  lane: string;
  inspection_time: string;
  distance_m: number;
  minimum_clearance_m: number;
  valid_points: number;
  quality_status: string;
  file_name: string;
  file_size_bytes: number;
  download_url: string;
  clearance_points: Array<{ distance_m: number; clearance_m: number }>;
};

const navigation: Array<{ id: PageId; label: string; index: string }> = [
  { id: "dashboard", label: "采集首页", index: "01" },
  { id: "tasks", label: "任务管理", index: "02" },
  { id: "playback", label: "数据回放", index: "03" },
  { id: "report", label: "报告导出", index: "04" },
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

function Header({ page }: { page: PageId }) {
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
          <StatusPill>等待接入</StatusPill>
        </div>
        <div className="clock">
          <strong>--:--</strong>
          <span>设备时间</span>
        </div>
      </div>
    </header>
  );
}

function Dashboard() {
  const [viewMode, setViewMode] = useState<ViewMode>("3d");
  const rtk = useRtkSocket();
  const rtkSnapshot = rtk.snapshot;
  const clearance = useClearanceSocket();
  const clearanceSnapshot = clearance.snapshot;
  const systemStatus = useSystemStatusSocket();
  const systemSnapshot = systemStatus.snapshot;
  const modes: Array<{ id: ViewMode; label: string; disabled?: boolean }> = [
    { id: "3d", label: "三维视图" },
    { id: "top", label: "沿X轴俯视" },
    { id: "section", label: "断面视图", disabled: true },
  ];

  const systemStreamAvailable = systemStatus.connection === "connected" && systemStatus.streamState === "streaming";
  const rtkDeviceText = systemStreamAvailable ? systemSnapshot?.rtk.message ?? "检查中" : "系统检查中";
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
  const solutionText = gpsState === null || gpsState === undefined
    ? "等待RTK数据"
    : `${solutionLabels[gpsState] ?? "状态码"}（${gpsState}）`;
  const rmcCharacter = rtkSnapshot?.rmc_validity
    ? String.fromCharCode(rtkSnapshot.rmc_validity)
    : null;
  const rmcText = rmcCharacter === "A"
    ? "RMC有效（A）"
    : rmcCharacter === "V"
      ? "RMC无效（V）"
      : rmcCharacter
        ? `RMC（${rmcCharacter}）`
        : "RMC --";
  const formatMetric = (value: number | null | undefined, digits = 2) =>
    value === null || value === undefined ? "--" : value.toFixed(digits);
  const hasFix = rtkSnapshot?.fix_status !== null &&
    rtkSnapshot?.fix_status !== undefined && rtkSnapshot.fix_status !== -1;
  const coordinateText = hasFix &&
    rtkSnapshot?.latitude !== null && rtkSnapshot?.latitude !== undefined &&
    rtkSnapshot?.longitude !== null && rtkSnapshot?.longitude !== undefined
    ? `${rtkSnapshot.latitude.toFixed(8)}°, ${rtkSnapshot.longitude.toFixed(8)}°`
    : "--";
  const altitudeText = hasFix ? `${formatMetric(rtkSnapshot?.altitude)} m` : "--";
  const monitorUnavailable = !systemStreamAvailable;
  const statusText = (device: DeviceStatus | undefined, fallback: string) =>
    monitorUnavailable ? fallback : device?.message ?? "检查中";
  const statusState = (device: DeviceStatus | undefined): HealthState =>
    monitorUnavailable ? "unknown" : device?.state ?? "unknown";
  const availableBytes = Number(systemSnapshot?.storage.values?.available_bytes);
  const storageText = monitorUnavailable
    ? "检查中"
    : Number.isFinite(availableBytes)
      ? `${(availableBytes / 1024 ** 3).toFixed(1)} GiB 可用`
      : systemSnapshot?.storage.message ?? "检查中";
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
  const aggregateText = {ok: "系统正常", warn: "系统有告警", error: "系统异常", stale: "系统异常", unknown: "系统检查中"}[aggregateState];
  const clearanceStreaming = clearance.connection === "connected" && clearance.streamState === "streaming";
  const clearanceValid = clearanceStreaming && clearanceSnapshot?.valid === true &&
    clearanceSnapshot.lidar_to_top_m !== null;
  const currentHeightText = clearanceValid
    ? clearanceSnapshot.lidar_to_top_m!.toFixed(3)
    : "--";

  return (
    <div className="page-stack dashboard-page">
      <section className="dashboard-health-bar" aria-label="系统状态与当前告警">
        <div className="health-alert">
          <span>…</span>
          <div>
            <strong className={`device-state device-state--${aggregateState}`}>{aggregateText}</strong>
          </div>
        </div>
        <div className="health-devices">
          <span><b>雷达</b><strong className={`device-state device-state--${statusState(systemSnapshot?.lidar)}`} title={systemSnapshot?.lidar.message}>{statusText(systemSnapshot?.lidar, "检查中")}</strong></span>
          <span><b>RTK</b><strong className={`device-state device-state--${statusState(systemSnapshot?.rtk)}`} title={systemSnapshot?.rtk.message}>{statusText(systemSnapshot?.rtk, "检查中")}</strong></span>
          <span><b>控制器</b><strong className={`device-state device-state--${statusState(systemSnapshot?.controller)}`} title={systemSnapshot?.controller.message}>{statusText(systemSnapshot?.controller, "检查中")}</strong></span>
          <span><b>存储</b><strong className={`device-state device-state--${statusState(systemSnapshot?.storage)}`} title={systemSnapshot?.storage.message}>{storageText}</strong></span>
        </div>
      </section>

      <section className="dashboard-layout">
        <div className="dashboard-main">
          <section className="dashboard-visual-grid" aria-label="点云与实时地图">
            <article className="panel cloud-panel dashboard-cloud-panel">
              <div className="cloud-toolbar">
                <div>
                  <h2>点云实时预览</h2>
                </div>
                <div className="segmented segmented--small" aria-label="点云视角">
                  {modes.map((mode) => (
                    <button
                      type="button"
                      key={mode.id}
                      className={viewMode === mode.id ? "active" : ""}
                      disabled={mode.disabled}
                      title={mode.disabled ? "等待真实断面接口接入" : undefined}
                      onClick={() => setViewMode(mode.id)}
                    >
                      {mode.label}
                    </button>
                  ))}
                </div>
                <span className="panel-tag panel-tag--muted">预览链路</span>
              </div>
              <PointCloudViewer viewMode={viewMode as PointCloudViewMode} />
            </article>

            <RealtimeAmap
              snapshot={rtkSnapshot}
              hasFix={hasFix && rmcCharacter !== "V"}
              connectionDetail={rtk.detail}
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

        <aside className="panel measurement-panel measurement-control-panel">
          <PanelHead
            title="测量与任务控制"
            description="实时顶面高度、RTK质量和当前任务操作"
          />
          <section className="measurement-primary">
            <span>雷达到当前最低顶面</span>
            <strong>{currentHeightText} <small>m</small></strong>
          </section>
          <section className="measurement-details" aria-label="RTK定位质量">
            <div className="measurement-details__head">
              <h3>RTK 定位质量</h3>
              <StatusPill tone={rtkTone}>{rtkDeviceText}</StatusPill>
            </div>
            <div><span>解状态</span><strong>{solutionText} · {rmcText}</strong></div>
            <div>
              <span>卫星数 / HDOP / PDOP</span>
              <strong>
                {rtkSnapshot?.satellite_count ?? "--"} / {formatMetric(rtkSnapshot?.hdop)} / {formatMetric(rtkSnapshot?.pdop)}
              </strong>
            </div>
            <div><span>当前坐标</span><strong className="rtk-coordinate">{coordinateText}</strong></div>
            <div><span>高度</span><strong>{altitudeText}</strong></div>
            <div><span>入口坐标</span><strong>未标记</strong></div>
            <div><span>出口坐标</span><strong>未标记</strong></div>
          </section>
          <section className="task-control-section" aria-label="当前任务与采集控制">
            <div className="task-control-section__head">
              <h3>当前任务</h3>
            </div>
            <div className="task-control-current">
              <div><span>任务名称</span><strong>尚未选择任务</strong></div>
              <StatusPill>待机</StatusPill>
            </div>
            <div className="task-information-grid" aria-label="任务重要信息">
              <div><span>任务编号</span><strong>--</strong></div>
              <div><span>隧道名称</span><strong>--</strong></div>
              <div><span>检测车道</span><strong>--</strong></div>
              <div><span>参数方案</span><strong>--</strong></div>
              <div><span>计划日期</span><strong>--</strong></div>
              <div><span>采集时长</span><strong>--:--</strong></div>
            </div>
            <div className="task-control-actions">
              <button type="button" className="button button--green" disabled title="请选择任务并完成采集前检查">开始</button>
              <button type="button" className="button button--warning" disabled>暂停</button>
              <button type="button" className="button button--danger" disabled>停止</button>
            </div>
          </section>
          <p className="measurement-footnote">浏览器断开不会终止RK3588上的采集与计算任务。</p>
        </aside>
      </section>
    </div>
  );
}

function Tasks() {
  const [taskTab, setTaskTab] = useState<TaskTab>("pending");
  const tabs: Array<{ id: TaskTab; label: string }> = [
    { id: "pending", label: "待检测任务" },
    { id: "completed", label: "已完成任务" },
    { id: "abnormal", label: "异常任务" },
  ];

  return (
    <div className="page-stack">
      <section className="task-overview">
        <article><i>待</i><div><span>待检测任务</span><strong>--</strong></div></article>
        <article><i>完</i><div><span>已完成任务</span><strong>--</strong></div></article>
        <article><i>异</i><div><span>异常任务</span><strong>--</strong></div></article>
      </section>

      <section className="task-page-grid">
        <article className="panel task-create-panel">
          <PanelHead
            title="批量创建检测任务"
            description="按隧道与车道建立待检测任务"
            trailing={<span className="panel-tag">新建任务</span>}
          />
          <div className="task-form">
            <label>
              <span>隧道名称</span>
              <input placeholder="请输入隧道名称" />
            </label>
            <label>
              <span>隧道编号</span>
              <input placeholder="请输入隧道编号" />
            </label>
            <fieldset>
              <legend>车道选择</legend>
              <label className="check-card"><input type="checkbox" /><span><i>01</i><strong>左侧车道</strong></span></label>
              <label className="check-card"><input type="checkbox" /><span><i>02</i><strong>右侧车道</strong></span></label>
            </fieldset>
            <label>
              <span>任务编号规则</span>
              <input placeholder="例如：隧道编号-车道-序号" />
            </label>
            <label>
              <span>计划检测日期</span>
              <input type="date" />
            </label>
          </div>
          <div className="task-form-actions">
            <button type="button" className="button button--soft">清空内容</button>
            <button type="button" className="button button--primary">批量创建任务</button>
          </div>
          <div className="info-box">当前为静态界面，任务内容不会保存或提交。</div>
        </article>

        <article className="panel task-queue-panel">
          <PanelHead title="任务编排预览" description="确认任务顺序与自动切换关系" />
          <div className="queue-empty">
            <EmptyState icon="≡" title="尚未添加任务" description="填写左侧信息后在此预览任务顺序" />
          </div>
          <div className="queue-rule">
            <div><span>自动切换规则</span><StatusPill>未设置</StatusPill></div>
            <p>当前任务完成 → 加载下一任务 → 更新任务状态</p>
          </div>
        </article>
      </section>

      <article className="panel table-panel">
        <div className="table-toolbar">
          <div className="segmented">
            {tabs.map((tab) => (
              <button
                type="button"
                key={tab.id}
                className={taskTab === tab.id ? "active" : ""}
                onClick={() => setTaskTab(tab.id)}
              >
                {tab.label}<span>0</span>
              </button>
            ))}
          </div>
          <div className="toolbar-actions">
            <label className="search-box"><span>⌕</span><input placeholder="搜索任务编号或隧道名称" /></label>
            <select defaultValue=""><option value="">全部车道</option></select>
          </div>
        </div>
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>任务编号</th><th>隧道名称</th><th>隧道编号</th>
                <th>检测车道</th><th>计划日期</th><th>任务状态</th><th>操作</th>
              </tr>
            </thead>
            <tbody>
              <tr className="empty-row">
                <td colSpan={7}>暂无{tabs.find((tab) => tab.id === taskTab)?.label}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </article>
    </div>
  );
}

function Playback() {
  const [tab, setTab] = useState<PlaybackTab>("result");
  const tabs: Array<{ id: PlaybackTab; label: string }> = [
    { id: "result", label: "测量结果" },
    { id: "history", label: "历史数据" },
    { id: "logs", label: "运行日志" },
  ];

  return (
    <div className="page-stack">
      <article className="panel playback-filter">
        <div>
          <span>历史数据回放</span>
          <strong>选择检测任务，查看测量结果与历史记录</strong>
        </div>
        <label><span>检测任务</span><select defaultValue=""><option value="">请选择任务</option></select></label>
        <label><span>回放区间</span><select defaultValue="all"><option value="all">全部数据</option></select></label>
        <button type="button" className="button button--primary">载入任务</button>
      </article>

      <div className="section-tabs">
        {tabs.map((item) => (
          <button
            type="button"
            key={item.id}
            className={tab === item.id ? "active" : ""}
            onClick={() => setTab(item.id)}
          >
            {item.label}
          </button>
        ))}
      </div>

      {tab === "result" && (
        <>
          <section className="playback-summary">
            <article><span>最低净空</span><strong>-- <small>m</small></strong></article>
            <article><span>有效测量点</span><strong>-- <small>点</small></strong></article>
            <article><span>采集时长</span><strong>--:--</strong></article>
            <article><span>异常点数量</span><strong>-- <small>点</small></strong></article>
          </section>
          <article className="panel">
            <PanelHead
              title="净空变化曲线"
              description="按采集序号查看历史净空结果"
              trailing={<span className="panel-tag panel-tag--muted">暂无任务</span>}
            />
            <EmptyChart historical />
          </article>
          <MeasurementTable message="请选择检测任务查看测量结果" />
        </>
      )}

      {tab === "history" && (
        <article className="panel data-management">
          <PanelHead title="历史测量数据" description="测量数据保存与历史任务管理" />
          <div className="data-actions">
            <button type="button"><i>数</i><span><strong>测量数据保存</strong><small>查看结构化测量结果</small></span><b>›</b></button>
            <button type="button"><i>史</i><span><strong>历史测量数据</strong><small>按任务查询历史记录</small></span><b>›</b></button>
            <button type="button"><i>任</i><span><strong>历史任务管理</strong><small>查看已完成与异常任务</small></span><b>›</b></button>
          </div>
          <div className="data-empty"><EmptyState icon="▤" title="尚未选择历史任务" description="载入任务后显示数据文件与记录" /></div>
        </article>
      )}

      {tab === "logs" && (
        <article className="panel log-panel">
          <PanelHead
            title="运行日志"
            description="查看任务采集与设备运行记录"
            trailing={<button type="button" className="button button--soft">导出日志</button>}
          />
          <div className="log-toolbar">
            <select defaultValue="all"><option value="all">全部级别</option><option value="info">信息</option><option value="warn">警告</option></select>
            <input placeholder="搜索日志内容" />
          </div>
          <div className="log-empty"><EmptyState icon="≡" title="暂无日志记录" description="选择任务后显示对应运行日志" /></div>
        </article>
      )}
    </div>
  );
}

function MeasurementTable({ message }: { message: string }) {
  return (
    <article className="panel table-panel">
      <PanelHead title="测量数据明细" description="净空高度、最低点位置和质量状态" />
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>序号</th><th>采集时间</th><th>相对里程</th><th>净空高度</th>
              <th>最低点横向位置</th><th>测量质量</th><th>标记</th>
            </tr>
          </thead>
          <tbody><tr className="empty-row"><td colSpan={7}>{message}</td></tr></tbody>
        </table>
      </div>
    </article>
  );
}

function Report() {
  const [reportData, setReportData] = useState<ReportTestData | null>(null);
  const [loadState, setLoadState] = useState<ReportLoadState>("idle");

  const selectReportTask = async (taskId: string) => {
    if (!taskId) {
      setReportData(null);
      setLoadState("idle");
      return;
    }

    setLoadState("loading");
    try {
      const response = await fetch("/api/v1/report-export-test", { cache: "no-store" });
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }
      setReportData(await response.json() as ReportTestData);
      setLoadState("ready");
    } catch {
      setReportData(null);
      setLoadState("error");
    }
  };

  const formatFileSize = (bytes: number) => `${Math.max(1, Math.ceil(bytes / 1024))} KB`;
  const chartPoints = reportData?.clearance_points.map((point) => {
    const x = reportData.distance_m > 0 ? point.distance_m / reportData.distance_m * 1000 : 0;
    const y = Math.max(0, Math.min(300, (6 - point.clearance_m) / 1.5 * 300));
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(" ");
  const minimumPoint = reportData && reportData.clearance_points.length > 0
    ? reportData.clearance_points.reduce((minimum, point) =>
        point.clearance_m < minimum.clearance_m ? point : minimum
      )
    : null;
  const minimumPointPosition = minimumPoint && reportData
    ? {
        x: reportData.distance_m > 0 ? minimumPoint.distance_m / reportData.distance_m * 1000 : 0,
        y: Math.max(0, Math.min(300, (6 - minimumPoint.clearance_m) / 1.5 * 300)),
      }
    : null;

  return (
    <div className="page-stack report-page">
      <article className="panel report-taskbar">
        <label className="report-task-select">
          <span>检测任务</span>
          <select defaultValue="" onChange={(event) => void selectReportTask(event.target.value)}>
            <option value="">请选择已完成任务</option>
            <option value="browser-download-test">浏览器下载测试任务（模拟数据）</option>
          </select>
        </label>
        <div className="report-key-metric">
          <span>最低净空</span>
          <strong>{reportData ? reportData.minimum_clearance_m.toFixed(2) : "--"} <small>m</small></strong>
        </div>
        <div className="report-key-metric">
          <span>有效测点</span>
          <strong>{reportData?.valid_points ?? "--"}</strong>
        </div>
        <div className="report-key-metric">
          <span>质量状态</span>
          <strong className={reportData ? "report-key-metric__test" : "report-key-metric__muted"}>
            {loadState === "loading" ? "正在载入" : reportData?.quality_status ?? "待选择任务"}
          </strong>
        </div>
      </article>

      <section className="report-layout">
        <aside className="panel report-export-panel">
          <PanelHead title="文件导出" description="将当前任务检测结果导出到本地" />
          <div className="report-file-card">
            <div className="report-file-icon">TXT</div>
            <div>
              <strong>检测结果文件</strong>
              <small>文本文件 · TXT</small>
            </div>
            <span className="report-file-tag">测试文件</span>
          </div>
          <div className="report-file-name">
            <span>导出文件名</span>
            <strong>{reportData ? `${reportData.file_name} · ${formatFileSize(reportData.file_size_bytes)}` : "选择任务后显示文件"}</strong>
          </div>
          <div className="report-export-hint">
            <i>i</i>
            <p>当前下载的是固定测试文件，仅用于验证电脑浏览器到设备端的文件下载链路。</p>
          </div>
          <div className="report-export-footer">
            <div className="report-export-state">
              <i className={loadState === "ready" ? "ready" : loadState === "error" ? "error" : ""} />
              <span>
                <strong>{loadState === "ready" ? "测试文件已就绪" : loadState === "error" ? "测试文件载入失败" : loadState === "loading" ? "正在检查测试文件" : "等待选择任务"}</strong>
                <small>{loadState === "error" ? "请确认FastAPI服务和设备端测试文件可用" : loadState === "ready" ? "可下载到当前电脑的浏览器默认目录" : "请选择浏览器下载测试任务"}</small>
              </span>
            </div>
            {reportData ? (
              <a className="button button--primary report-download-button" href={reportData.download_url} download={reportData.file_name}>下载测试 TXT</a>
            ) : (
              <button type="button" className="button button--primary" disabled>下载测试 TXT</button>
            )}
          </div>
        </aside>

        <article className="panel report-overview">
          <PanelHead
            title="任务数据概览"
            description="查看所选任务的基本信息与净空高度变化"
            trailing={<span className={`panel-tag${reportData ? " panel-tag--test" : " panel-tag--muted"}`}>{reportData ? "模拟数据" : "未选择"}</span>}
          />
          <div className="report-basic-info">
            <div><span>隧道名称</span><strong>{reportData?.tunnel_name ?? "--"}</strong></div>
            <div><span>任务编号</span><strong>{reportData?.task_id ?? "--"}</strong></div>
            <div><span>检测车道</span><strong>{reportData?.lane ?? "--"}</strong></div>
            <div><span>检测时间</span><strong>{reportData?.inspection_time ?? "--"}</strong></div>
            <div><span>检测里程</span><strong>{reportData ? `${reportData.distance_m.toFixed(1)} m` : "-- m"}</strong></div>
            <div><span>有效测点</span><strong>{reportData?.valid_points ?? "--"}</strong></div>
          </div>
          <div className="report-chart-head">
            <div>
              <strong>净空高度曲线</strong>
              <span>随车辆相对里程变化</span>
            </div>
            <div className="report-chart-legend"><i /><span>净空高度</span></div>
          </div>
          <div className="report-clearance-chart">
            <div className="report-chart-grid" />
            {chartPoints && (
              <svg className="report-chart-plot" viewBox="0 0 1000 300" preserveAspectRatio="none" aria-label="模拟净空高度曲线">
                <polyline points={chartPoints} />
                {minimumPointPosition && <circle cx={minimumPointPosition.x} cy={minimumPointPosition.y} r="7" />}
              </svg>
            )}
            <div className="report-chart-y-title">净空高度（m）</div>
            <div className="report-chart-y-axis">
              <span>6.0</span><span>5.5</span><span>5.0</span><span>4.5</span>
            </div>
            <div className="report-chart-x-axis">
              <span>0</span><span>25</span><span>50</span><span>75</span><span>100</span>
            </div>
            <div className="report-chart-x-title">相对里程（m）</div>
            {!reportData && (
              <EmptyState
                compact
                icon={loadState === "error" ? "!" : "⌁"}
                title={loadState === "error" ? "测试数据载入失败" : "暂无净空曲线"}
                description={loadState === "error" ? "请检查FastAPI和测试文件" : "选择测试任务后显示模拟曲线"}
              />
            )}
          </div>
        </article>
      </section>
    </div>
  );
}

export default function Home() {
  const [activePage, setActivePage] = useState<PageId>("dashboard");

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
              onClick={() => setActivePage(item.id)}
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
        {activePage !== "dashboard" && <Header page={activePage} />}
        <div className="page-content">
          {activePage === "dashboard" && <Dashboard />}
          {activePage === "tasks" && <Tasks />}
          {activePage === "playback" && <Playback />}
          {activePage === "report" && <Report />}
        </div>
      </main>
    </div>
  );
}
