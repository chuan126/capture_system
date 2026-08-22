"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import InteractiveClearanceChart from "@/components/playback/InteractiveClearanceChart";
import {
  normalizeChartView,
  type NormalizedViewWindow,
} from "@/components/playback/clearanceChartViewport";
import {
  loadMeasurementPrefix,
  loadMeasurementSeries,
  loadMeasurementSummary,
  type MeasurementRtkEndpoint,
  type MeasurementSeries,
  type MeasurementSummary,
} from "@/components/playback/measurementHistoryApi";
import { loadInitialPlayback } from "@/components/playback/playbackLoadCoordinator";
import { markPlaybackTiming } from "@/components/playback/playbackPerformance";
import {
  createPlaybackSeriesCache,
  isSeriesWindowCached,
  mergeSeriesIntoCache,
} from "@/components/playback/playbackSeriesCache";
import { getUserSeriesWindowRequest } from "@/components/playback/playbackSeriesWindow";
import TaskBrowser from "@/components/workflow/TaskBrowser";
import { deleteSelectedTasks, TaskApiError } from "@/components/workflow/taskApi";
import type { CollectionTask, WorkflowPageId } from "@/components/workflow/taskModel";

const INITIAL_PREFIX_SAMPLES = 2000;
const DETAIL_SERIES_POINTS = 6000;
const VIEW_RELOAD_DELAY_MS = 160;
const SUMMARY_LOAD_DELAY_MS = 100;
const SUMMARY_RETRY_DELAY_MS = 500;

type Props = {
  tasks: CollectionTask[];
  selectedTaskId: string | null;
  onSelectTask: (id: string) => void;
  onDataChanged: () => Promise<void>;
  onNavigate: (page: WorkflowPageId) => void;
};

type HistoryState = "idle" | "loading" | "empty" | "ready" | "error";

const formatHeight = (value: number | null) => value === null ? "-- m" : `${value.toFixed(3)} m`;
const formatDuration = (value: number | null) => {
  if (value === null) return "--:--.---";
  const milliseconds = Math.max(0, Math.round(value));
  return `${String(Math.floor(milliseconds / 60000)).padStart(2, "0")}:${String(Math.floor((milliseconds % 60000) / 1000)).padStart(2, "0")}.${String(milliseconds % 1000).padStart(3, "0")}`;
};
const formatRate = (value: number | null) => value === null ? "-- Hz" : `${value.toFixed(2)} Hz`;
const formatRtk = (endpoint: MeasurementRtkEndpoint | null) => !endpoint || !endpoint.valid
  ? "--"
  : `${endpoint.latitudeDeg.toFixed(7)}, ${endpoint.longitudeDeg.toFixed(7)}`;
const canDeleteTask = (task: CollectionTask) => task.status !== "采集中" && task.status !== "已暂停";
const isAbortError = (error: unknown) => (
  error instanceof DOMException && error.name === "AbortError"
) || (
  error instanceof TaskApiError && error.status === 499
);
const isSupersededError = (error: unknown) => (
  error instanceof TaskApiError && error.status === 499
);

export default function PlaybackWorkspace({
  tasks,
  selectedTaskId,
  onSelectTask,
  onDataChanged,
  onNavigate,
}: Props) {
  const selectedTask = tasks.find((task) => task.taskId === selectedTaskId) ?? null;
  const [historyState, setHistoryState] = useState<HistoryState>("idle");
  const [summary, setSummary] = useState<MeasurementSummary | null>(null);
  const [series, setSeries] = useState<MeasurementSeries | null>(null);
  const [historyError, setHistoryError] = useState<string | null>(null);
  const [viewWindow, setViewWindow] = useState<NormalizedViewWindow>({ start: 0, end: 1 });
  const [initialViewWindow, setInitialViewWindow] = useState<NormalizedViewWindow | null>(null);
  const [seriesRefreshing, setSeriesRefreshing] = useState(false);
  const [checked, setChecked] = useState<Set<string>>(new Set());
  const [deleting, setDeleting] = useState(false);
  const [deleteMessage, setDeleteMessage] = useState<string | null>(null);
  const [deleteError, setDeleteError] = useState(false);
  const userViewRevisionRef = useRef(0);
  const seriesCacheRef = useRef(createPlaybackSeriesCache());

  const disabledTaskIds = useMemo(
    () => new Set(tasks.filter((task) => !canDeleteTask(task)).map((task) => task.taskId)),
    [tasks],
  );

  useEffect(() => {
    const controller = new AbortController();
    let prefixTimer: number | null = null;
    let summaryTimer: number | null = null;
    const beginInitialLoad = () => {
      userViewRevisionRef.current = 0;
      seriesCacheRef.current = createPlaybackSeriesCache();
      setSummary(null);
      setSeries(null);
      setHistoryError(null);
      setViewWindow({ start: 0, end: 1 });
      setInitialViewWindow(null);
      setSeriesRefreshing(false);

      if (!selectedTaskId) {
        setHistoryState("idle");
        return;
      }
      if (!selectedTask?.hasMeasurements) {
        setHistoryState("empty");
        return;
      }

      setHistoryState("loading");
      markPlaybackTiming(selectedTaskId, "task selected");
      markPlaybackTiming(selectedTaskId, "prefix request start");
      const loadSummary = () => {
        markPlaybackTiming(selectedTaskId, "summary request start");
        return loadMeasurementSummary(selectedTaskId, controller.signal);
      };
      const commitSummary = (nextSummary: MeasurementSummary) => {
        if (controller.signal.aborted) return;
        markPlaybackTiming(selectedTaskId, "summary response received");
        setSummary(nextSummary);
      };
      const handleSummaryError = (error: unknown) => {
        if (isSupersededError(error) && !controller.signal.aborted) {
          summaryTimer = window.setTimeout(() => {
            void loadSummary().then(commitSummary).catch(handleSummaryError);
          }, SUMMARY_RETRY_DELAY_MS);
          return;
        }
        if (isAbortError(error)) return;
        setHistoryError(error instanceof Error ? error.message : "历史统计读取失败");
      };
      void loadInitialPlayback({
        loadPrefix: () => loadMeasurementPrefix(selectedTaskId, {
          maxSamples: INITIAL_PREFIX_SAMPLES,
          signal: controller.signal,
        }),
        loadSummary,
        scheduleSummary: (load) => {
          summaryTimer = window.setTimeout(load, SUMMARY_LOAD_DELAY_MS);
        },
        onPrefix: (nextSeries) => {
          if (controller.signal.aborted) return;
          markPlaybackTiming(selectedTaskId, "prefix response received", {
            returnedSamples: nextSeries.returnedSampleCount,
          });
          const domainSpan = Math.max(1, nextSeries.domainEndTimestampMs - nextSeries.domainStartTimestampMs);
          const prefixEnd = Math.max(
            0.0001,
            (nextSeries.requestedEndTimestampMs - nextSeries.domainStartTimestampMs) / domainSpan,
          );
          const nextInitialView = normalizeChartView(0, Math.min(1, prefixEnd), 0.0001);
          const cachedSeries = mergeSeriesIntoCache(seriesCacheRef.current, nextSeries);
          setSeries(cachedSeries);
          setInitialViewWindow(nextInitialView);
          setViewWindow(nextInitialView);
          setHistoryState(nextSeries.sourceSampleCount > 1 ? "ready" : "empty");
        },
        onSummary: commitSummary,
        onPrefixError: (error) => {
          if (isAbortError(error)) return;
          setHistoryState("error");
          setHistoryError(error instanceof Error ? error.message : "历史曲线首段读取失败");
        },
        onSummaryError: handleSummaryError,
      });
    };
    // development 的 StrictMode 会执行一次 setup/cleanup 探测；零延时调度使被清理的
    // 探测 effect 不会真的发出 HTTP 请求，正式首屏仍在下一轮事件循环立即开始。
    prefixTimer = window.setTimeout(beginInitialLoad, 0);

    return () => {
      if (prefixTimer !== null) window.clearTimeout(prefixTimer);
      if (summaryTimer !== null) window.clearTimeout(summaryTimer);
      controller.abort();
    };
  }, [selectedTaskId, selectedTask?.hasMeasurements]);

  useEffect(() => {
    if (!selectedTaskId || !series || historyState !== "ready" || userViewRevisionRef.current === 0) {
      return undefined;
    }

    const request = getUserSeriesWindowRequest(
      series,
      viewWindow,
      userViewRevisionRef.current,
    );
    if (!request) return undefined;

    if (isSeriesWindowCached(
      seriesCacheRef.current,
      request.startTimestampMs,
      request.endTimestampMs,
      DETAIL_SERIES_POINTS,
    )) {
      markPlaybackTiming(selectedTaskId, "series cache hit", {
        startTimestampMs: request.startTimestampMs,
        endTimestampMs: request.endTimestampMs,
      });
      return undefined;
    }

    const controller = new AbortController();
    const timer = window.setTimeout(() => {
      setSeriesRefreshing(true);
      markPlaybackTiming(selectedTaskId, "series request start", {
        startTimestampMs: request.startTimestampMs,
        endTimestampMs: request.endTimestampMs,
      });
      loadMeasurementSeries(selectedTaskId, {
        startTimestampMs: request.startTimestampMs,
        endTimestampMs: request.endTimestampMs,
        maxPoints: DETAIL_SERIES_POINTS,
        signal: controller.signal,
      }).then((nextSeries) => {
        if (controller.signal.aborted) return;
        markPlaybackTiming(selectedTaskId, "series response received", {
          sourceSamples: nextSeries.sourceSampleCount,
          returnedSamples: nextSeries.returnedSampleCount,
        });
        setSeries(mergeSeriesIntoCache(seriesCacheRef.current, nextSeries));
        setSeriesRefreshing(false);
      }).catch((error) => {
        if (isAbortError(error)) return;
        setSeriesRefreshing(false);
        setHistoryError(error instanceof Error ? error.message : "局部曲线读取失败");
      });
    }, VIEW_RELOAD_DELAY_MS);

    return () => {
      window.clearTimeout(timer);
      controller.abort();
    };
  }, [historyState, selectedTaskId, series, viewWindow]);

  useEffect(() => {
    const timer = window.setTimeout(() => {
      setChecked((current) => new Set(
        [...current].filter((id) => tasks.some((task) => task.taskId === id) && !disabledTaskIds.has(id)),
      ));
    }, 0);
    return () => window.clearTimeout(timer);
  }, [tasks, disabledTaskIds]);

  const handleViewWindowChange = useCallback((nextView: NormalizedViewWindow) => {
    userViewRevisionRef.current += 1;
    setViewWindow(nextView);
  }, []);

  useEffect(() => {
    if (historyState !== "ready" || !series || !selectedTaskId) return;
    markPlaybackTiming(selectedTaskId, "prefix state committed", {
      returnedSamples: series.returnedSampleCount,
    });
  }, [historyState, selectedTaskId, series]);

  const stats = summary?.statistics ?? null;
  const validRate = stats && stats.totalSamples > 0
    ? `${((stats.validSamples / stats.totalSamples) * 100).toFixed(1)}%`
    : "--";
  const taskEnded = selectedTask?.status === "已停止"
    || selectedTask?.status === "异常中断"
    || selectedTask?.status === "失败";
  const measurementRows = useMemo(() => [
    ["最低净空高度", formatHeight(stats?.minimumHeightM ?? null)],
    ["平均净空高度", formatHeight(stats?.averageHeightM ?? null)],
    ["最高净空高度", formatHeight(stats?.maximumHeightM ?? null)],
    ["有效采样数量", stats ? `${stats.validSamples} / ${stats.totalSamples}` : "--"],
    ["记录时长", formatDuration(stats?.durationMs ?? null)],
    ["实际平均频率", formatRate(stats?.actualAverageSampleRateHz ?? null)],
  ], [stats]);
  const qualityRows = useMemo(() => [
    ["数据来源", summary?.dataOrigin === "test_fixture" ? "界面测试数据" : summary ? "设备记录" : "--"],
    ["标称采样频率", stats ? `${stats.nominalSampleRateHz.toFixed(0)} Hz` : "--"],
    ["有效采样比例", validRate],
    ["无效采样数量", stats ? String(stats.invalidSamples) : "--"],
    ["暂停区间", summary ? `${summary.pauseIntervalCount} 段` : "--"],
    ["记录完整性", summary ? summary.complete ? "完整" : "异常中断或未完整结束" : "--"],
  ], [summary, stats, validRate]);

  const toggle = (id: string) => {
    if (disabledTaskIds.has(id)) return;
    setChecked((current) => {
      const next = new Set(current);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  };
  const toggleGroup = (_: string, ids: string[]) => setChecked((current) => {
    const next = new Set(current);
    const eligible = ids.filter((id) => !disabledTaskIds.has(id));
    const allChecked = eligible.length > 0 && eligible.every((id) => next.has(id));
    eligible.forEach((id) => {
      if (allChecked) next.delete(id);
      else next.add(id);
    });
    return next;
  });
  const toggleAll = (ids: string[]) => toggleGroup("all", ids);
  const deleteIds = [...checked];
  const deleteSelected = async () => {
    if (deleteIds.length === 0 || deleting) return;
    const confirmed = window.confirm(`确定删除所选 ${deleteIds.length} 个任务吗？`);
    if (!confirmed) return;
    setDeleting(true);
    setDeleteMessage(null);
    setDeleteError(false);
    try {
      const result = await deleteSelectedTasks(deleteIds);
      setChecked(new Set());
      setDeleteMessage(`已删除 ${result.deletedTaskCount} 个任务`);
      await onDataChanged();
    } catch (error) {
      setDeleteError(true);
      setDeleteMessage(error instanceof Error ? error.message : "任务删除失败");
    } finally {
      setDeleting(false);
    }
  };

  const recordState = historyState === "loading"
    ? "读取中"
    : historyState === "ready"
      ? stats ? `${stats.validSamples} 条有效记录` : "曲线已就绪"
      : historyState === "error"
        ? "读取失败"
        : selectedTask?.localDataPurgedAt
          ? "本地数据已清理"
          : selectedTask?.hasMeasurements
            ? "记录为空"
            : selectedTask
              ? "无记录"
              : "--";
  const curveDetail = series
    ? `${series.samples.length.toLocaleString()} 个已缓存显示点 · 当前窗口源样本 ${series.sourceSampleCount.toLocaleString()}${series.downsampled ? " · 当前窗口保留极值" : " · 当前窗口完整分辨率"}${seriesRefreshing ? " · 更新中" : ""}`
    : `首屏读取前 ${INITIAL_PREFIX_SAMPLES.toLocaleString()} 条样本`;

  return (
    <div className="page-stack playback-page playback-page--clearance">
      <section className="panel workflow-context-bar">
        <div className="workflow-context-bar__identity">
          <span>当前任务</span>
          <strong>{selectedTask?.displayId ?? "尚未选择任务"}</strong>
          <small>{selectedTask ? `${selectedTask.tunnelCode} · ${selectedTask.tunnelName} · ${selectedTask.status}` : "任务按创建日期分组，可选择单个、多个或当前筛选结果后删除"}</small>
        </div>
        <div className="workflow-context-bar__states">
          <div><span>任务状态</span><strong>{selectedTask?.status ?? "--"}</strong></div>
          <div><span>高度记录</span><strong>{recordState}</strong></div>
          <div><span>已选删除</span><strong>{checked.size} 项</strong></div>
          <div><span>本地数据</span><strong>{selectedTask?.localDataPurgedAt ? "已清理" : selectedTask?.hasMeasurements ? "存在" : "无"}</strong></div>
        </div>
        <div className="workflow-context-bar__actions">
          <button type="button" className="button" onClick={() => onNavigate("dashboard")}>返回采集首页</button>
          <button type="button" className="button button--primary" disabled={!selectedTask} onClick={() => onNavigate("report")}>进入报告导出</button>
          <button type="button" className="button button--danger-outline" disabled={deleteIds.length === 0 || deleting} onClick={() => void deleteSelected()}>{deleting ? "正在删除" : `删除所选任务${deleteIds.length ? ` (${deleteIds.length})` : ""}`}</button>
        </div>
      </section>

      {deleteMessage && <p className={`batch-operation-message${deleteError ? " is-error" : ""}`}>{deleteMessage}</p>}

      <section className="playback-layout playback-layout--clearance">
        <TaskBrowser
          tasks={tasks}
          selectedTaskId={selectedTaskId}
          onSelectTask={onSelectTask}
          heading="选择回放任务"
          sortOrder="asc"
          selectable
          showSelectAll
          checkedTaskIds={checked}
          disabledTaskIds={disabledTaskIds}
          onToggleChecked={toggle}
          onToggleDateChecked={toggleGroup}
          onToggleAllChecked={toggleAll}
        />
        <main className="playback-clearance-main">
          <article className="panel playback-clearance-panel">
            <header className="playback-clearance-panel__head">
              <div>
                <span>10 Hz 测量序列</span>
                <h2>{selectedTask ? `${selectedTask.displayId} 净空高度曲线` : "净空高度曲线"}</h2>
                <p>首屏只读取任务最前面的固定样本段。已加载窗口保留在当前任务缓存中，回拖时直接复用；只有未加载或分辨率不足的区域才再次请求，降采样仍保留局部最低值、最高值和无效断点。</p>
                <small>{curveDetail}</small>
              </div>
              <span className={`workflow-status ${summary?.dataOrigin === "test_fixture" ? "workflow-status--warn" : taskEnded ? "workflow-status--ok" : "workflow-status--idle"}`}>{summary?.dataOrigin === "test_fixture" ? "界面测试数据" : taskEnded ? "任务已结束" : selectedTask ? "任务未结束" : "待选择"}</span>
            </header>
            <InteractiveClearanceChart
              key={`${selectedTask?.taskId ?? "no-task"}:${initialViewWindow ? "prefix" : "empty"}`}
              samples={series?.samples ?? []}
              domainStartTimestampMs={series?.domainStartTimestampMs ?? 0}
              domainEndTimestampMs={series?.domainEndTimestampMs ?? 1}
              initialView={initialViewWindow ?? undefined}
              onViewWindowChange={handleViewWindowChange}
              timingTaskId={selectedTaskId ?? undefined}
              emptyTitle={historyState === "loading" ? "正在读取任务前段高度记录" : historyState === "error" ? "任务高度记录读取失败" : selectedTask?.localDataPurgedAt ? "本地测量数据已清理" : selectedTask ? "当前任务没有可显示的高度记录" : "请选择任务查看净空高度曲线"}
              emptyDescription={historyState === "error" ? historyError ?? "请检查后端日志。" : selectedTask?.localDataPurgedAt ? "本地测量数据已清理，无法回放。" : "页面不会使用模拟曲线替代缺失记录。"}
            />
          </article>
        </main>
        <aside className="panel playback-inspector playback-inspector--clearance">
          <header><div><span>任务记录</span><strong>统计与数据质量</strong></div></header>
          <section className="inspector-section">
            <h3>隧道信息</h3>
            <dl>
              <div><dt>任务编号</dt><dd>{selectedTask?.displayId ?? "--"}</dd></div>
              <div><dt>隧道编号</dt><dd>{selectedTask?.tunnelCode ?? "--"}</dd></div>
              <div><dt>隧道名称</dt><dd>{selectedTask?.tunnelName ?? "--"}</dd></div>
              <div><dt>检测车道</dt><dd>{summary?.lane ?? selectedTask?.lane ?? "未记录"}</dd></div>
            </dl>
          </section>
          <section className="inspector-section"><h3>测量统计</h3><dl>{measurementRows.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl></section>
          <section className="inspector-section"><h3>隧道端点</h3><dl><div><dt>入口 RTK</dt><dd>{formatRtk(summary?.entryRtk ?? null)}</dd></div><div><dt>出口 RTK</dt><dd>{formatRtk(summary?.exitRtk ?? null)}</dd></div></dl></section>
          <section className="inspector-section"><h3>数据质量</h3><dl>{qualityRows.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl></section>
        </aside>
      </section>
    </div>
  );
}
