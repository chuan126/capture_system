"use client";

import { useEffect, useMemo, useState } from "react";

import InteractiveClearanceChart from "@/components/playback/InteractiveClearanceChart";
import {
  loadMeasurementHistory,
  type MeasurementHistory,
  type MeasurementRtkEndpoint,
} from "@/components/playback/measurementHistoryApi";
import TaskBrowser from "@/components/workflow/TaskBrowser";
import { formatTaskSequence } from "@/components/workflow/taskModel";
import type { CollectionTask, WorkflowPageId } from "@/components/workflow/taskModel";

type PlaybackWorkspaceProps = {
  tasks: CollectionTask[];
  selectedTaskId: string | null;
  onSelectTask: (taskId: string) => void;
  onDeleteTask: (taskId: string) => Promise<void>;
  onNavigate: (page: WorkflowPageId) => void;
};

type HistoryState = "idle" | "loading" | "empty" | "ready" | "error";

const formatHeight = (value: number | null) =>
  value === null ? "-- m" : `${value.toFixed(3)} m`;

const formatDuration = (durationMs: number | null) => {
  if (durationMs === null) return "--:--.---";
  const totalMilliseconds = Math.max(0, Math.round(durationMs));
  const minutes = Math.floor(totalMilliseconds / 60_000);
  const seconds = Math.floor((totalMilliseconds % 60_000) / 1_000);
  const milliseconds = totalMilliseconds % 1_000;
  return `${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}.${String(milliseconds).padStart(3, "0")}`;
};

const formatRate = (value: number | null) =>
  value === null ? "-- Hz" : `${value.toFixed(2)} Hz`;

const formatRtk = (endpoint: MeasurementRtkEndpoint | null) => {
  if (!endpoint || !endpoint.valid) return "--";
  return `${endpoint.latitudeDeg.toFixed(7)}, ${endpoint.longitudeDeg.toFixed(7)}`;
};

function DeleteTaskDialog({
  task,
  deleting,
  error,
  onCancel,
  onConfirm,
}: {
  task: CollectionTask;
  deleting: boolean;
  error: string | null;
  onCancel: () => void;
  onConfirm: () => void;
}) {
  return (
    <div
      className="task-dialog-mask"
      role="dialog"
      aria-modal="true"
      aria-labelledby="delete-task-dialog-title"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget && !deleting) onCancel();
      }}
    >
      <section className="task-dialog-panel delete-task-dialog">
        <header className="task-dialog-head">
          <div>
            <h2 id="delete-task-dialog-title">删除任务 {formatTaskSequence(task.sequence)}</h2>
            <p>任务将从采集、回放和报告列表中移除。测量文件暂时保留，不执行物理清理。</p>
          </div>
          <button type="button" disabled={deleting} onClick={onCancel} aria-label="关闭删除任务窗口">×</button>
        </header>

        <dl className="delete-task-dialog__summary">
          <div><dt>隧道编号</dt><dd>{task.tunnelCode}</dd></div>
          <div><dt>隧道名称</dt><dd>{task.tunnelName}</dd></div>
          <div><dt>任务状态</dt><dd>{task.status}</dd></div>
          <div><dt>测量记录</dt><dd>{task.hasMeasurements ? "存在，保留文件" : "无"}</dd></div>
        </dl>

        {error && <p className="task-dialog-error" role="alert">{error}</p>}

        <footer className="task-dialog-actions">
          <button type="button" className="button" disabled={deleting} onClick={onCancel}>取消</button>
          <button type="button" className="button button--danger" disabled={deleting} onClick={onConfirm}>
            {deleting ? "正在删除" : "确认删除"}
          </button>
        </footer>
      </section>
    </div>
  );
}

export default function PlaybackWorkspace({
  tasks,
  selectedTaskId,
  onSelectTask,
  onDeleteTask,
  onNavigate,
}: PlaybackWorkspaceProps) {
  const selectedTask = tasks.find((task) => task.taskId === selectedTaskId) ?? null;
  const [historyState, setHistoryState] = useState<HistoryState>("idle");
  const [history, setHistory] = useState<MeasurementHistory | null>(null);
  const [historyError, setHistoryError] = useState<string | null>(null);
  const [deleteDialogOpen, setDeleteDialogOpen] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [deleteError, setDeleteError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    setHistory(null);
    setHistoryError(null);

    if (!selectedTask) {
      setHistoryState("idle");
      return () => {
        cancelled = true;
      };
    }
    if (!selectedTask.hasMeasurements) {
      setHistoryState("empty");
      return () => {
        cancelled = true;
      };
    }

    setHistoryState("loading");
    void loadMeasurementHistory(selectedTask.taskId)
      .then((loaded) => {
        if (cancelled) return;
        if (loaded.taskId !== selectedTask.taskId) {
          throw new Error("历史记录与当前任务不一致");
        }
        setHistory(loaded);
        setHistoryState(loaded.samples.length > 1 ? "ready" : "empty");
      })
      .catch((error) => {
        if (cancelled) return;
        setHistoryError(error instanceof Error ? error.message : "历史记录读取失败");
        setHistoryState("error");
      });

    return () => {
      cancelled = true;
    };
  }, [selectedTask]);

  useEffect(() => {
    setDeleteDialogOpen(false);
    setDeleteError(null);
  }, [selectedTaskId]);

  const taskEnded = selectedTask?.status === "已停止" || selectedTask?.status === "异常中断" || selectedTask?.status === "失败";
  const taskNumber = selectedTask ? formatTaskSequence(selectedTask.sequence) : "--";
  const deleteBlocked = selectedTask?.status === "采集中" || selectedTask?.status === "已暂停";
  const statistics = history?.statistics ?? null;
  const validRate = statistics && statistics.totalSamples > 0
    ? `${((statistics.validSamples / statistics.totalSamples) * 100).toFixed(1)}%`
    : "--";

  const measurementRows = useMemo(() => [
    ["最低净空高度", formatHeight(statistics?.minimumHeightM ?? null)],
    ["平均净空高度", formatHeight(statistics?.averageHeightM ?? null)],
    ["最高净空高度", formatHeight(statistics?.maximumHeightM ?? null)],
    ["有效采样数量", statistics ? `${statistics.validSamples} / ${statistics.totalSamples}` : "--"],
    ["记录时长", formatDuration(statistics?.durationMs ?? null)],
    ["实际平均频率", formatRate(statistics?.actualAverageSampleRateHz ?? null)],
  ], [statistics]);

  const qualityRows = useMemo(() => [
    ["数据来源", history?.dataOrigin === "test_fixture" ? "界面测试数据" : history ? "设备记录" : "--"],
    ["标称采样频率", statistics ? `${statistics.nominalSampleRateHz.toFixed(0)} Hz` : "--"],
    ["有效采样比例", validRate],
    ["无效采样数量", statistics ? String(statistics.invalidSamples) : "--"],
    ["暂停区间", history ? `${history.pauseIntervalCount} 段` : "--"],
    ["记录完整性", history ? (history.complete ? "完整" : "异常中断或未完整结束") : "--"],
  ], [history, statistics, validRate]);

  const recordStateText = historyState === "loading"
    ? "读取中"
    : historyState === "ready"
      ? `${statistics?.validSamples ?? 0} 条有效记录`
      : historyState === "error"
        ? "读取失败"
        : selectedTask?.hasMeasurements
          ? "记录为空"
          : selectedTask
            ? "无记录"
            : "--";
  const curveStateText = historyState === "ready"
    ? "可查看"
    : historyState === "loading"
      ? "加载中"
      : historyState === "error"
        ? "不可用"
        : selectedTask
          ? "无数据"
          : "--";

  const confirmDelete = async () => {
    if (!selectedTask || deleteBlocked) return;
    setDeleting(true);
    setDeleteError(null);
    try {
      await onDeleteTask(selectedTask.taskId);
      setDeleteDialogOpen(false);
    } catch (error) {
      setDeleteError(error instanceof Error ? error.message : "任务删除失败");
    } finally {
      setDeleting(false);
    }
  };

  return (
    <div className="page-stack playback-page playback-page--clearance">
      <section className="panel workflow-context-bar">
        <div className="workflow-context-bar__identity">
          <span>当前任务</span>
          <strong>{selectedTask ? `任务 ${taskNumber}` : "尚未选择任务"}</strong>
          <small>{selectedTask
            ? `${selectedTask.tunnelCode} · ${selectedTask.tunnelName} · ${selectedTask.status}`
            : "从左侧任务列表选择需要查看的隧道记录"}</small>
        </div>
        <div className="workflow-context-bar__states" aria-label="回放数据状态">
          <div><span>任务状态</span><strong>{selectedTask?.status ?? "--"}</strong></div>
          <div><span>高度记录</span><strong className={historyState === "ready" ? "is-ready" : historyState === "error" ? "is-blocked" : "is-pending"}>{recordStateText}</strong></div>
          <div><span>曲线状态</span><strong className={historyState === "ready" ? "is-ready" : historyState === "error" ? "is-blocked" : "is-pending"}>{curveStateText}</strong></div>
        </div>
        <div className="workflow-context-bar__actions">
          <button type="button" className="button" onClick={() => onNavigate("dashboard")}>返回采集首页</button>
          <button type="button" className="button button--primary" disabled={!selectedTask} onClick={() => onNavigate("report")}>进入报告导出</button>
          <button
            type="button"
            className="button button--danger-outline"
            disabled={!selectedTask || deleteBlocked}
            title={deleteBlocked ? "采集中或已暂停的任务不能删除" : "删除当前选中的任务"}
            onClick={() => {
              setDeleteError(null);
              setDeleteDialogOpen(true);
            }}
          >删除任务</button>
        </div>
      </section>

      <section className="playback-layout playback-layout--clearance">
        <TaskBrowser
          tasks={tasks}
          selectedTaskId={selectedTaskId}
          onSelectTask={onSelectTask}
          heading="选择回放任务"
        />

        <main className="playback-clearance-main">
          <article className="panel playback-clearance-panel">
            <header className="playback-clearance-panel__head">
              <div>
                <span>50 Hz 测量序列</span>
                <h2>{selectedTask ? `任务 ${taskNumber} 完整净空高度曲线` : "完整净空高度曲线"}</h2>
                <p>曲线展示当前任务的完整记录。支持拖拽平移、滚轮缩放、按钮缩放和双击复位，无效采样保持断开。</p>
              </div>
              <span className={`workflow-status ${history?.dataOrigin === "test_fixture" ? "workflow-status--warn" : taskEnded ? "workflow-status--ok" : "workflow-status--idle"}`}>
                {history?.dataOrigin === "test_fixture" ? "界面测试数据" : taskEnded ? "任务已结束" : selectedTask ? "任务未结束" : "待选择"}
              </span>
            </header>

            <InteractiveClearanceChart
              key={selectedTask?.taskId ?? "no-task"}
              samples={history?.samples ?? []}
              emptyTitle={historyState === "loading"
                ? "正在读取任务高度记录"
                : historyState === "error"
                  ? "任务高度记录读取失败"
                  : selectedTask
                    ? "当前任务没有可显示的高度记录"
                    : "请选择任务查看完整净空高度曲线"}
              emptyDescription={historyState === "loading"
                ? "正在从 FastAPI 历史记录接口读取任务测量数据库。"
                : historyState === "error"
                  ? historyError ?? "请检查任务测量文件和后端日志。"
                  : selectedTask
                    ? "任务尚未形成测量文件，页面不会使用实时流或自动生成曲线替代。"
                    : "任务形成正式记录后，完整曲线将在此按源时间戳展示。"}
            />
          </article>
        </main>

        <aside className="panel playback-inspector playback-inspector--clearance">
          <header>
            <div>
              <span>任务记录</span>
              <strong>统计与数据质量</strong>
            </div>
            <span className={`workflow-status ${historyState === "ready" ? history?.dataOrigin === "test_fixture" ? "workflow-status--warn" : "workflow-status--ok" : historyState === "error" ? "workflow-status--danger" : "workflow-status--idle"}`}>
              {historyState === "ready" ? history?.dataOrigin === "test_fixture" ? "测试数据" : "记录可用" : historyState === "error" ? "读取失败" : "无数据"}
            </span>
          </header>

          <section className="inspector-section">
            <h3>隧道信息</h3>
            <dl>
              <div><dt>任务编号</dt><dd>{selectedTask ? taskNumber : "--"}</dd></div>
              <div><dt>隧道编号</dt><dd>{selectedTask?.tunnelCode ?? "--"}</dd></div>
              <div><dt>隧道名称</dt><dd>{selectedTask?.tunnelName ?? "--"}</dd></div>
              <div><dt>检测车道</dt><dd>{history?.lane ?? selectedTask?.lane ?? "未记录"}</dd></div>
            </dl>
          </section>

          <section className="inspector-section">
            <h3>测量统计</h3>
            <dl>
              {measurementRows.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}
            </dl>
          </section>

          <section className="inspector-section">
            <h3>隧道端点</h3>
            <dl>
              <div><dt>入口 RTK</dt><dd title={formatRtk(history?.entryRtk ?? null)}>{formatRtk(history?.entryRtk ?? null)}</dd></div>
              <div><dt>出口 RTK</dt><dd title={formatRtk(history?.exitRtk ?? null)}>{formatRtk(history?.exitRtk ?? null)}</dd></div>
            </dl>
          </section>

          <section className="inspector-section">
            <h3>数据质量</h3>
            <dl>
              {qualityRows.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}
            </dl>
          </section>
        </aside>
      </section>

      {deleteDialogOpen && selectedTask && (
        <DeleteTaskDialog
          task={selectedTask}
          deleting={deleting}
          error={deleteError}
          onCancel={() => {
            if (!deleting) setDeleteDialogOpen(false);
          }}
          onConfirm={() => void confirmDelete()}
        />
      )}
    </div>
  );
}
