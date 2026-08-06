"use client";

import { useEffect, useMemo, useState } from "react";

import {
  downloadGeneratedFile,
  generateSummaryPdf,
  generateTaskTxt,
  loadReportPreview,
  type GeneratedExportFile,
  type ReportPreview,
  type ReportRtkEndpoint,
  type ReportTaskPreview,
} from "@/components/report/reportExportApi";
import TaskBrowser from "@/components/workflow/TaskBrowser";
import { formatTaskSequence } from "@/components/workflow/taskModel";
import type { CollectionTask, WorkflowPageId } from "@/components/workflow/taskModel";


type ReportWorkspaceProps = {
  tasks: CollectionTask[];
  selectedTaskId: string | null;
  onSelectTask: (taskId: string) => void;
  onNavigate: (page: WorkflowPageId) => void;
};

type PreviewState = "loading" | "ready" | "error";
type ExportState = "idle" | "generating" | "done" | "error";

const txtFields = [
  { name: "记录时间", detail: "数据源时间和设备写入时间，精确到毫秒" },
  { name: "隧道编号", detail: "当前任务对应的隧道业务编号" },
  { name: "检测车道", detail: "任务开始时冻结的左车道或右车道" },
  { name: "实时高度", detail: "有效样本的净空高度，无效样本保持空值" },
  { name: "最低高度", detail: "当前任务全部有效样本中的最低高度" },
  { name: "隧道入口 RTK", detail: "入口经纬度和高程，无有效定位时标记未记录" },
  { name: "隧道出口 RTK", detail: "出口经纬度和高程，无有效定位时标记未记录" },
];

const pdfColumns = [
  "任务编号",
  "隧道编号",
  "检测车道",
  "最低高度",
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

const formatHeight = (value: number | null) => value === null ? "--" : `${value.toFixed(3)} m`;

const formatTime = (value: string | null) => {
  if (!value) return "--";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "--";
  return date.toLocaleString("zh-CN", { hour12: false });
};

const formatRtk = (endpoint: ReportRtkEndpoint | null) => {
  if (!endpoint || !endpoint.valid) return "未记录";
  return `${endpoint.latitudeDeg.toFixed(7)}, ${endpoint.longitudeDeg.toFixed(7)}`;
};

const errorText = (error: unknown) => error instanceof Error ? error.message : "导出操作失败";

export default function ReportWorkspace({
  tasks,
  selectedTaskId,
  onSelectTask,
  onNavigate,
}: ReportWorkspaceProps) {
  const [previewState, setPreviewState] = useState<PreviewState>("loading");
  const [preview, setPreview] = useState<ReportPreview | null>(null);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const [txtState, setTxtState] = useState<ExportState>("idle");
  const [pdfState, setPdfState] = useState<ExportState>("idle");
  const [txtMessage, setTxtMessage] = useState<string | null>(null);
  const [pdfMessage, setPdfMessage] = useState<string | null>(null);

  const taskRevision = useMemo(
    () => tasks.map((task) => `${task.taskId}:${task.updatedAt}:${task.hasMeasurements}`).join("|"),
    [tasks],
  );

  useEffect(() => {
    let cancelled = false;
    setPreviewState("loading");
    setPreviewError(null);
    loadReportPreview()
      .then((result) => {
        if (cancelled) return;
        setPreview(result);
        setPreviewState("ready");
      })
      .catch((error) => {
        if (cancelled) return;
        setPreview(null);
        setPreviewError(errorText(error));
        setPreviewState("error");
      });
    return () => {
      cancelled = true;
    };
  }, [taskRevision]);

  const selectedTask = tasks.find((task) => task.taskId === selectedTaskId) ?? null;
  const selectedPreview = preview?.tasks.find((task) => task.taskId === selectedTaskId) ?? null;
  const exportableTasks = preview?.tasks.filter((task) => task.exportable) ?? [];
  const selectedTaskReady = selectedPreview?.exportable === true;
  const pdfReady = exportableTasks.length > 0;
  const taskNumber = selectedTask ? formatTaskSequence(selectedTask.sequence) : "--";
  const selectedStatus = selectedPreview ? statusText[selectedPreview.status] : selectedTask?.status ?? "待选择";
  const blockerText = !selectedTask
    ? "请先选择需要导出 50 Hz 明细的任务"
    : previewState === "loading"
      ? "正在核对任务测量记录"
      : previewState === "error"
        ? previewError ?? "导出预览读取失败"
        : selectedPreview?.blockedReason ?? "当前任务满足正式 TXT 导出条件";

  const runDownload = async (
    kind: "txt" | "pdf",
    generator: () => Promise<GeneratedExportFile>,
  ) => {
    const setState = kind === "txt" ? setTxtState : setPdfState;
    const setMessage = kind === "txt" ? setTxtMessage : setPdfMessage;
    setState("generating");
    setMessage(null);
    try {
      const file = await generator();
      await downloadGeneratedFile(file);
      setState("done");
      setMessage(`${file.fileName} 已生成并开始下载`);
    } catch (error) {
      setState("error");
      setMessage(errorText(error));
    }
  };

  return (
    <div className="page-stack report-page report-page--simple">
      <section className="panel workflow-context-bar">
        <div className="workflow-context-bar__identity">
          <span>当前导出对象</span>
          <strong>{selectedTask ? `任务 ${taskNumber}` : "尚未选择任务"}</strong>
          <small>{selectedTask
            ? `${selectedTask.tunnelCode} · ${selectedTask.tunnelName}`
            : "TXT 按当前任务导出，PDF 汇总全部满足正式导出条件的隧道记录"}</small>
        </div>
        <div className="workflow-context-bar__states" aria-label="导出状态">
          <div><span>当前任务</span><strong className={selectedTaskReady ? "is-ready" : "is-pending"}>{selectedStatus}</strong></div>
          <div><span>可汇总任务</span><strong className={pdfReady ? "is-ready" : "is-pending"}>{previewState === "ready" ? `${exportableTasks.length}/${preview?.taskCount ?? 0}` : "--"}</strong></div>
          <div><span>导出接口</span><strong className={previewState === "ready" ? "is-ready" : "is-blocked"}>{previewState === "ready" ? "可用" : previewState === "loading" ? "检查中" : "异常"}</strong></div>
        </div>
        <div className="workflow-context-bar__actions">
          <button type="button" className="button" onClick={() => onNavigate("playback")}>返回数据回放</button>
          <button type="button" className="button button--primary" onClick={() => onNavigate("dashboard")}>返回采集首页</button>
        </div>
      </section>

      <section className="report-simple-layout">
        <TaskBrowser
          tasks={tasks}
          selectedTaskId={selectedTaskId}
          onSelectTask={onSelectTask}
          heading="选择导出任务"
        />

        <main className="report-simple-main">
          <article className="panel report-task-summary">
            <header className="report-simple-heading">
              <div>
                <span>当前任务</span>
                <h2>导出对象</h2>
              </div>
              <strong className={selectedTaskReady ? "is-ready" : "is-pending"}>{selectedStatus}</strong>
            </header>

            <dl className="report-task-summary__grid">
              <div><dt>任务编号</dt><dd>{selectedTask ? taskNumber : "--"}</dd></div>
              <div><dt>隧道编号</dt><dd>{selectedTask?.tunnelCode ?? "--"}</dd></div>
              <div><dt>隧道名称</dt><dd>{selectedTask?.tunnelName ?? "--"}</dd></div>
              <div><dt>检测车道</dt><dd>{selectedPreview?.lane ?? selectedTask?.lane ?? "未记录"}</dd></div>
            </dl>

            <p className={`report-export-blocker${selectedTaskReady ? " is-ready" : ""}`} role="status">
              <strong>{selectedTaskReady ? "可以导出" : "当前不能导出"}</strong>
              <span>{blockerText}</span>
            </p>
          </article>

          <section className="report-export-grid" aria-label="导出格式">
            <article className="panel report-export-card report-export-card--txt">
              <header className="report-export-card__head">
                <span className="report-export-card__type" aria-hidden="true">TXT</span>
                <div>
                  <h2>50 Hz 测量明细</h2>
                  <p>当前选中任务每个采样周期输出一行，无效样本保留空高度和无效原因。</p>
                </div>
                <strong>50 Hz</strong>
              </header>

              <div className="report-field-list" aria-label="TXT字段">
                {txtFields.map((field, index) => (
                  <div key={field.name}>
                    <span>{String(index + 1).padStart(2, "0")}</span>
                    <div><strong>{field.name}</strong><small>{field.detail}</small></div>
                  </div>
                ))}
              </div>

              <footer className="report-export-card__footer">
                <div>
                  <span>文件名称</span>
                  <strong>{selectedTask ? `任务${taskNumber}_${selectedTask.tunnelCode}_50Hz测量明细.txt` : "任务编号_隧道编号_50Hz测量明细.txt"}</strong>
                  {txtMessage && <small className={txtState === "error" ? "is-error" : "is-success"}>{txtMessage}</small>}
                </div>
                <button
                  type="button"
                  className="button button--primary"
                  disabled={!selectedTaskReady || txtState === "generating"}
                  onClick={() => selectedTaskId && runDownload("txt", () => generateTaskTxt(selectedTaskId))}
                >
                  {txtState === "generating" ? "正在生成" : "导出 TXT"}
                </button>
              </footer>
            </article>

            <article className="panel report-export-card report-export-card--pdf">
              <header className="report-export-card__head">
                <span className="report-export-card__type" aria-hidden="true">PDF</span>
                <div>
                  <h2>隧道净空检测汇总</h2>
                  <p>按任务序号汇总正常完成、来源为正式记录且包含有效净空样本的任务。</p>
                </div>
                <strong>{exportableTasks.length} 项</strong>
              </header>

              <section className="report-pdf-outline" aria-label="PDF内容预览">
                <header>
                  <span>报告标题</span>
                  <strong>隧道净空检测汇总报告</strong>
                </header>
                <div className="report-pdf-table" role="table" aria-label="隧道汇总字段">
                  <div className="report-pdf-table__row report-pdf-table__row--head" role="row">
                    {pdfColumns.map((column) => <span key={column} role="columnheader">{column}</span>)}
                  </div>
                  {exportableTasks.length === 0 ? (
                    <div className="report-pdf-table__row" role="row">
                      {pdfColumns.map((column) => <span key={column} role="cell">--</span>)}
                    </div>
                  ) : exportableTasks.map((task) => (
                    <div className="report-pdf-table__row" role="row" key={task.taskId}>
                      <span role="cell">{formatTaskSequence(task.sequence)}</span>
                      <span role="cell">{task.tunnelCode}</span>
                      <span role="cell">{task.lane ?? "未记录"}</span>
                      <span role="cell">{formatHeight(task.minimumHeightM)}</span>
                      <span role="cell">{formatTime(task.startedAt)}</span>
                      <span role="cell">{formatRtk(task.entryRtk)}</span>
                      <span role="cell">{formatRtk(task.exitRtk)}</span>
                    </div>
                  ))}
                </div>
                <p>无有效 RTK 端点时字段标记为未记录。界面测试数据、异常中断记录和无有效高度的任务不会进入正式 PDF。</p>
              </section>

              <footer className="report-export-card__footer">
                <div>
                  <span>文件名称</span>
                  <strong>隧道净空检测汇总报告.pdf</strong>
                  {pdfMessage && <small className={pdfState === "error" ? "is-error" : "is-success"}>{pdfMessage}</small>}
                </div>
                <button
                  type="button"
                  className="button button--primary"
                  disabled={!pdfReady || pdfState === "generating"}
                  onClick={() => runDownload("pdf", generateSummaryPdf)}
                >
                  {pdfState === "generating" ? "正在生成" : "导出 PDF"}
                </button>
              </footer>
            </article>
          </section>
        </main>
      </section>
    </div>
  );
}
