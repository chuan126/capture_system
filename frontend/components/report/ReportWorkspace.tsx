"use client";

import { useEffect, useMemo, useState } from "react";

import {
  cancelExportJob,
  downloadExportJob,
  loadDeepSeekSettings,
  loadExportJob,
  loadReportPreview,
  saveDeepSeekSettings,
  startDeepSeekReportJob,
  startSummaryPdfJob,
  startTaskTxtJob,
  type DeepSeekSettings,
  type ExportJob,
  type ReportPreview,
  type ReportRtkEndpoint,
  type ReportTaskPreview,
} from "@/components/report/reportExportApi";
import TaskBrowser from "@/components/workflow/TaskBrowser";
import type { CollectionTask, WorkflowPageId } from "@/components/workflow/taskModel";

type Props = {
  tasks: CollectionTask[];
  selectedTaskId: string | null;
  onSelectTask: (id: string) => void;
  onNavigate: (page: WorkflowPageId) => void;
};
type PreviewState = "idle" | "loading" | "ready" | "error";
type ExportState = "idle" | "generating" | "done" | "error";
type ExportKind = "txt" | "pdf" | "deepseek";

const pdfColumns = [
  "任务编号",
  "隧道编号",
  "检测车道",
  "原始单帧最低 m",
  "记录时间",
  "隧道入口 RTK",
  "隧道出口 RTK",
];
const statusText: Record<ReportTaskPreview["status"], string> = {
  pending: "待执行",
  running: "采集中",
  paused: "已暂停",
  completed: "已停止",
  interrupted: "异常中断",
  failed: "失败",
};
const DEFAULT_DEEPSEEK_SETTINGS: DeepSeekSettings = {
  apiUrl: "https://api.deepseek.com/chat/completions",
  apiKey: "",
  model: "deepseek-v4-flash",
  skillPrompt: `你是隧道净空测量证据审计器。输入是RK3588从最多2 GiB measurements.db只读流式计算得到的capture-clearance-audit-v2 analysis_package，不是数据库全表。不得要求重新上传全库、不得臆造未提供的数据，也不得修改原始记录。

字段口径：n必须原样取source_frame_statistics.valid_frames，禁止把最低帧数量或候选数量写成n。raw必须取source_frame_statistics.raw_min_m；f取与raw对应的最低真实源帧。设备端已经按source_sequence去重并排除无效、0、NaN和Inf高度，clearance_samples中的重复保持记录不能当成独立障碍。

判定时以点云证据为主体：综合raw_min_m、median_m、mad_m、p01_m、p05_m、rolling3_median_min_m、rolling5_median_min_m、lowest_20_real_frames的重复低值、selected_inlier_count以及candidate_contexts前后连续性。只有同时满足“孤立、明显突降、前后立即恢复、无连续或周期结构支持”才判O。连续低值、风机、横梁或周期结构判V；证据确实矛盾或不足时判R。V或R时eff必须等于raw，只有O允许从输入已有滚动统计或候选值中选择eff。

可信度c表示“对V/R/O判定结论的可信程度”，不是净空精度、RTK有效率或数据字段完整率。R表示有充分理由需要复核，结论明确时c可以较高，不能因为状态是R就自动限制在0.5以下。

评分时遵守以下规则：
- 真实有效源帧不少于20且source_valid_ratio不低于0.95，是强基础证据；不少于100帧且频率稳定时可进一步提高可信度。
- 多个相近低值、连续低值、3帧或5帧滚动统计支持、较高inlier数量，均应提高结论可信度；单个最低帧inlier偏少只降低该帧权重，不得抹去其他低值帧证据。
- RTK无效、continuous_distance_m为null只影响空间定位，不直接否定点云高度，对c的合计影响不得超过0.05。
- selected_grid_area_m2、selected_residual_p95_m或最低点XYZ等辅助字段缺失只写入q；已有帧数、连续性和inlier证据可用时，对c的合计影响不得超过0.10。
- 同一缺项不得通过多个描述重复扣分。q用于披露数据问题，不要求每个q都降低c。
- 证据一致的V通常为0.80–0.98；证据清楚但仍需现场确认的R通常为0.65–0.85；证据充分的O通常为0.80–0.98。
- 只有有效真实源帧少于5、核心高度大量无效或证据严重冲突时，c才应低于0.55。

报告依据必须诚实、保守但不过度惩罚辅助数据缺项。数据库={DB_PATH}；范围={TASK_OR_TIME_RANGE}。`,
};

const formatHeight = (value: number | null) => value === null ? "--" : `${value.toFixed(3)} m`;
const formatTime = (value: string | null) => {
  if (!value) return "--";
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? "--" : date.toLocaleString("zh-CN", { hour12: false });
};
const formatRtk = (endpoint: ReportRtkEndpoint | null) => !endpoint || !endpoint.valid
  ? "未记录"
  : `${endpoint.latitudeDeg.toFixed(7)}, ${endpoint.longitudeDeg.toFixed(7)}`;
const errorText = (error: unknown) => error instanceof Error ? error.message : "导出操作失败";
const jobKey = (kind: ExportKind) => `capture-export-job-${kind}`;
const wait = (milliseconds: number) => new Promise((resolve) => window.setTimeout(resolve, milliseconds));

export default function ReportWorkspace({
  tasks,
  selectedTaskId,
  onSelectTask,
  onNavigate,
}: Props) {
  const [checked, setChecked] = useState<Set<string>>(() => new Set(selectedTaskId ? [selectedTaskId] : []));
  const [previewState, setPreviewState] = useState<PreviewState>("idle");
  const [preview, setPreview] = useState<ReportPreview | null>(null);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const [txtState, setTxtState] = useState<ExportState>("idle");
  const [pdfState, setPdfState] = useState<ExportState>("idle");
  const [deepseekState, setDeepSeekState] = useState<ExportState>("idle");
  const [txtMessage, setTxtMessage] = useState<string | null>(null);
  const [pdfMessage, setPdfMessage] = useState<string | null>(null);
  const [deepseekMessage, setDeepSeekMessage] = useState<string | null>(null);
  const [txtJob, setTxtJob] = useState<ExportJob | null>(null);
  const [pdfJob, setPdfJob] = useState<ExportJob | null>(null);
  const [deepseekJob, setDeepSeekJob] = useState<ExportJob | null>(null);
  const [deepseekSettings, setDeepSeekSettings] = useState<DeepSeekSettings>(DEFAULT_DEEPSEEK_SETTINGS);
  const [settingsLoaded, setSettingsLoaded] = useState(false);
  const [settingsMessage, setSettingsMessage] = useState<string | null>(null);

  const selectedTask = tasks.find((task) => task.taskId === selectedTaskId) ?? null;
  const selectedIds = useMemo(
    () => tasks.filter((task) => checked.has(task.taskId)).map((task) => task.taskId),
    [checked, tasks],
  );
  const previewIds = useMemo(
    () => selectedTaskId && !selectedIds.includes(selectedTaskId)
      ? [...selectedIds, selectedTaskId]
      : selectedIds,
    [selectedIds, selectedTaskId],
  );
  const revision = useMemo(
    () => tasks.map((task) => `${task.taskId}:${task.updatedAt}:${task.hasMeasurements}`).join("|"),
    [tasks],
  );

  useEffect(() => {
    setChecked((current) => {
      const next = new Set([...current].filter((id) => tasks.some((task) => task.taskId === id)));
      if (next.size === 0 && selectedTaskId && tasks.some((task) => task.taskId === selectedTaskId)) {
        next.add(selectedTaskId);
      }
      return next;
    });
  }, [revision, tasks, selectedTaskId]);

  useEffect(() => {
    const controller = new AbortController();
    loadDeepSeekSettings(controller.signal)
      .then((settings) => {
        setDeepSeekSettings(settings);
        setSettingsLoaded(true);
      })
      .catch((error) => {
        if (!controller.signal.aborted) {
          setSettingsLoaded(true);
          setSettingsMessage(errorText(error));
        }
      });
    return () => controller.abort();
  }, []);

  useEffect(() => {
    if (!settingsLoaded) return;
    const timer = window.setTimeout(() => {
      saveDeepSeekSettings(deepseekSettings)
        .then(() => setSettingsMessage("配置已保存到设备端"))
        .catch((error) => setSettingsMessage(errorText(error)));
    }, 700);
    return () => window.clearTimeout(timer);
  }, [deepseekSettings, settingsLoaded]);

  useEffect(() => {
    const controller = new AbortController();
    if (previewIds.length === 0) {
      setPreview(null);
      setPreviewState("idle");
      return () => controller.abort();
    }
    setPreviewState("loading");
    setPreviewError(null);
    loadReportPreview(previewIds, controller.signal)
      .then((result) => {
        setPreview(result);
        setPreviewState("ready");
      })
      .catch((error) => {
        if (!controller.signal.aborted) {
          setPreview(null);
          setPreviewState("error");
          setPreviewError(errorText(error));
        }
      });
    return () => controller.abort();
  }, [previewIds.join("|"), revision]);

  const selectedPreview = preview?.tasks.find((task) => task.taskId === selectedTaskId) ?? null;
  const pdfExportableTasks = preview?.tasks.filter(
    (task) => checked.has(task.taskId) && task.pdfExportable,
  ) ?? [];
  const txtReady = selectedPreview?.exportable === true;
  const pdfReady = pdfExportableTasks.length > 0;
  const deepseekReady = selectedPreview?.pdfExportable === true
    && deepseekSettings.apiUrl.trim().length > 0
    && deepseekSettings.apiKey.trim().length > 0
    && deepseekSettings.skillPrompt.trim().length > 0;

  const toggle = (id: string) => setChecked((current) => {
    const next = new Set(current);
    next.has(id) ? next.delete(id) : next.add(id);
    return next;
  });
  const toggleDate = (_: string, ids: string[]) => setChecked((current) => {
    const next = new Set(current);
    const all = ids.every((id) => next.has(id));
    ids.forEach((id) => all ? next.delete(id) : next.add(id));
    return next;
  });

  const followJob = async (kind: ExportKind, initial: ExportJob, signal?: AbortSignal) => {
    const setState = kind === "txt" ? setTxtState : kind === "pdf" ? setPdfState : setDeepSeekState;
    const setMessage = kind === "txt" ? setTxtMessage : kind === "pdf" ? setPdfMessage : setDeepSeekMessage;
    const setJob = kind === "txt" ? setTxtJob : kind === "pdf" ? setPdfJob : setDeepSeekJob;
    let current = initial;
    setJob(current);
    setState("generating");
    while (current.state === "queued" || current.state === "running") {
      if (signal?.aborted) return;
      if (kind !== "deepseek") {
        const percentage = Math.round(current.progress * 100);
        setMessage(`${current.phase} ${percentage}%`);
      }
      await wait(750);
      current = await loadExportJob(current.jobId, signal);
      setJob(current);
    }
    localStorage.removeItem(jobKey(kind));
    if (current.state === "completed") {
      downloadExportJob(current);
      setState("done");
      setMessage(`${current.fileName ?? "导出文件"} 已生成并开始下载`);
    } else if (current.state === "cancelled") {
      setState("idle");
      setMessage("导出任务已取消");
    } else {
      setState("error");
      setMessage(current.error ?? "导出任务失败");
    }
  };

  const run = async (kind: ExportKind, starter: () => Promise<ExportJob>) => {
    const setState = kind === "txt" ? setTxtState : kind === "pdf" ? setPdfState : setDeepSeekState;
    const setMessage = kind === "txt" ? setTxtMessage : kind === "pdf" ? setPdfMessage : setDeepSeekMessage;
    setState("generating");
    setMessage(kind === "deepseek" ? null : "正在提交导出任务");
    try {
      const created = await starter();
      localStorage.setItem(jobKey(kind), created.jobId);
      await followJob(kind, created);
    } catch (error) {
      setState("error");
      setMessage(errorText(error));
    }
  };

  const cancel = async (kind: ExportKind, current: ExportJob | null) => {
    if (!current) return;
    const setMessage = kind === "txt" ? setTxtMessage : kind === "pdf" ? setPdfMessage : setDeepSeekMessage;
    try {
      await cancelExportJob(current.jobId);
      localStorage.removeItem(jobKey(kind));
      (kind === "txt" ? setTxtState : kind === "pdf" ? setPdfState : setDeepSeekState)("idle");
      setMessage("导出任务已取消");
    } catch (error) {
      setMessage(errorText(error));
    }
  };

  useEffect(() => {
    const controller = new AbortController();
    (["txt", "pdf", "deepseek"] as const).forEach((kind) => {
      const id = localStorage.getItem(jobKey(kind));
      if (id) {
        loadExportJob(id, controller.signal)
          .then((job) => followJob(kind, job, controller.signal))
          .catch((error) => {
            if (!controller.signal.aborted) {
              (kind === "txt" ? setTxtMessage : kind === "pdf" ? setPdfMessage : setDeepSeekMessage)(errorText(error));
            }
          });
      }
    });
    return () => controller.abort();
  }, []);

  const blocker = !selectedTask
    ? "请先选择需要导出明细的任务"
    : selectedPreview?.blockedReason
      ?? (txtReady ? "当前任务满足正式 TXT 导出条件" : "请将当前任务加入 PDF 选择后核对导出条件");

  return (
    <div className="page-stack report-page report-page--simple">
      <section className="panel workflow-context-bar">
        <div className="workflow-context-bar__identity"><span>报告选择</span><strong>{checked.size} 个任务已选择</strong><small>TXT 和大模型报告面向当前任务，PDF 汇总完成异常分析且包含有效净空的勾选任务</small></div>
        <div className="workflow-context-bar__states"><div><span>当前任务</span><strong>{selectedTask?.displayId ?? "--"}</strong></div><div><span>已选择</span><strong>{checked.size} 项</strong></div><div><span>可汇总</span><strong className={pdfReady ? "is-ready" : "is-pending"}>{previewState === "ready" ? `${pdfExportableTasks.length}/${selectedIds.length}` : "--"}</strong></div><div><span>导出接口</span><strong className={previewState === "error" ? "is-blocked" : "is-ready"}>{previewState === "error" ? "异常" : "可用"}</strong></div></div>
        <div className="workflow-context-bar__actions"><button type="button" className="button" onClick={() => onNavigate("playback")}>返回数据回放</button><button type="button" className="button button--primary" onClick={() => onNavigate("dashboard")}>返回采集首页</button></div>
      </section>
      {previewError && <p className="batch-operation-message is-error" role="alert">{previewError}</p>}
      <section className="report-simple-layout">
        <TaskBrowser tasks={tasks} selectedTaskId={selectedTaskId} onSelectTask={onSelectTask} heading="选择导出任务" sortOrder="asc" selectable checkedTaskIds={checked} onToggleChecked={toggle} onToggleDateChecked={toggleDate} />
        <main className="report-simple-main">
          <article className="panel report-task-summary">
            <header className="report-simple-heading"><div><span>当前任务</span><h2>{selectedTask?.displayId ?? "导出对象"}</h2></div><strong className={txtReady ? "is-ready" : "is-pending"}>{selectedPreview ? statusText[selectedPreview.status] : selectedTask?.status ?? "待选择"}</strong></header>
            <dl className="report-task-summary__grid"><div><dt>任务编号</dt><dd>{selectedTask?.displayId ?? "--"}</dd></div><div><dt>创建时间</dt><dd>{formatTime(selectedTask?.createdAt ?? null)}</dd></div><div><dt>隧道编号</dt><dd>{selectedTask?.tunnelCode ?? "--"}</dd></div><div><dt>隧道名称</dt><dd>{selectedTask?.tunnelName ?? "--"}</dd></div><div><dt>检测车道</dt><dd>{selectedPreview?.lane ?? selectedTask?.lane ?? "未记录"}</dd></div></dl>
            <p className={`report-export-blocker${txtReady ? " is-ready" : ""}`}><strong>{txtReady ? "可以导出" : "当前不能导出"}</strong><span>{blocker}</span></p>
          </article>
          <section className="report-export-grid">
            <article className="panel report-export-card report-export-card--txt">
              <header className="report-export-card__head"><span className="report-export-card__type">TXT</span><div><h2>原始数据保存</h2><p>雷达、IMU、RTK数据保存</p></div></header>
              <footer className="report-export-card__footer"><div><span>文件名称</span><strong>{selectedTask ? `${selectedTask.displayId}_${selectedTask.tunnelCode}_原始数据保存.txt` : "时间编号_隧道编号_原始数据保存.txt"}</strong>{txtMessage && <small className={txtState === "error" ? "is-error" : "is-success"}>{txtMessage}</small>}</div><button type="button" className="button button--primary" disabled={!txtReady} onClick={() => txtState === "generating" ? cancel("txt", txtJob) : selectedTask && run("txt", () => startTaskTxtJob(selectedTask.taskId))}>{txtState === "generating" ? "取消导出" : "导出 TXT"}</button></footer>
            </article>
            <article className="panel report-export-card report-export-card--pdf">
              <header className="report-export-card__head"><span className="report-export-card__type">PDF</span><div><h2>隧道净空检测汇总</h2></div><strong>{pdfExportableTasks.length} 项</strong></header>
              <section className="report-pdf-outline">
                <header><span>报告标题</span><strong>隧道净空检测汇总报告</strong></header>
                <div className="report-pdf-table"><div className="report-pdf-table__row report-pdf-table__row--head">{pdfColumns.map((column) => <span key={column}>{column}</span>)}</div>{pdfExportableTasks.length === 0 ? <div className="report-pdf-table__row">{pdfColumns.map((column) => <span key={column}>--</span>)}</div> : pdfExportableTasks.map((task) => <div className="report-pdf-table__row" key={task.taskId}><span>{task.displayId}</span><span>{task.tunnelCode}</span><span>{task.lane ?? "未记录"}</span><span>{formatHeight(task.rawMinClearanceM)}</span><span>{formatTime(task.startedAt)}</span><span>{formatRtk(task.entryRtk)}</span><span>{formatRtk(task.exitRtk)}</span></div>)}</div>
              </section>
              <section className="deepseek-report-config">
                <header><div><strong>DeepSeek大模型辅助分析</strong></div><span>当前任务：{selectedTask?.displayId ?? "--"}</span></header>
                <div className="deepseek-report-config__fields">
                  <label className="deepseek-report-config__api"><span>DeepSeek API</span><input type="url" value={deepseekSettings.apiUrl} onChange={(event) => setDeepSeekSettings((current) => ({ ...current, apiUrl: event.target.value }))} /></label>
                  <label className="deepseek-report-config__key"><span>DeepSeek API Key</span><input type="password" autoComplete="off" value={deepseekSettings.apiKey} placeholder="请输入DeepSeek API Key" onChange={(event) => setDeepSeekSettings((current) => ({ ...current, apiKey: event.target.value }))} /></label>
                  <label className="deepseek-report-config__model"><span>分析模型</span><select value={deepseekSettings.model} onChange={(event) => setDeepSeekSettings((current) => ({ ...current, model: event.target.value as DeepSeekSettings["model"] }))}><option value="deepseek-v4-flash">deepseek-v4-flash</option><option value="deepseek-v4-pro">deepseek-v4-pro</option></select></label>
                  <label className="deepseek-report-config__skill"><span>大模型Skill</span><textarea value={deepseekSettings.skillPrompt} onChange={(event) => setDeepSeekSettings((current) => ({ ...current, skillPrompt: event.target.value }))} /></label>
                </div>
                {settingsMessage && <p className={settingsMessage.includes("保存到") ? "is-success" : "is-error"}>{settingsMessage}</p>}
                {deepseekState === "generating" && <p className="deepseek-thinking" role="status" aria-label="大模型thinking"><span>大模型thinking</span><span className="deepseek-thinking__dots" aria-hidden="true"><i>.</i><i>.</i><i>.</i></span></p>}
                {deepseekState !== "generating" && deepseekMessage && <p className={deepseekState === "error" ? "is-error" : "is-success"}>{deepseekMessage}</p>}
              </section>
              <footer className="report-export-card__footer report-export-card__footer--buttons">
                {pdfMessage && <small className={pdfState === "error" ? "is-error" : "is-success"}>{pdfMessage}</small>}
                <div className="report-export-card__actions"><button type="button" className="button" disabled={!pdfReady} onClick={() => pdfState === "generating" ? cancel("pdf", pdfJob) : run("pdf", () => startSummaryPdfJob(selectedIds))}>{pdfState === "generating" ? "取消本地生成" : "本地报告生成"}</button><button type="button" className="button button--primary" disabled={!deepseekReady} onClick={() => deepseekState === "generating" ? cancel("deepseek", deepseekJob) : selectedTask && run("deepseek", () => startDeepSeekReportJob(selectedTask.taskId, deepseekSettings))}>{deepseekState === "generating" ? "取消AI生成" : "AI报告生成"}</button></div>
              </footer>
            </article>
          </section>
        </main>
      </section>
    </div>
  );
}
