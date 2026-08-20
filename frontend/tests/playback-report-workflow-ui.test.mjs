import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");
const playback = await readFile(new URL("../components/playback/PlaybackWorkspace.tsx", import.meta.url), "utf8");
const interactiveChart = await readFile(new URL("../components/playback/InteractiveClearanceChart.tsx", import.meta.url), "utf8");
const playbackWindow = await readFile(new URL("../components/playback/playbackSeriesWindow.ts", import.meta.url), "utf8");
const playbackCache = await readFile(new URL("../components/playback/playbackSeriesCache.ts", import.meta.url), "utf8");
const playbackCoordinator = await readFile(new URL("../components/playback/playbackLoadCoordinator.ts", import.meta.url), "utf8");
const report = await readFile(new URL("../components/report/ReportWorkspace.tsx", import.meta.url), "utf8");
const taskBrowser = await readFile(new URL("../components/workflow/TaskBrowser.tsx", import.meta.url), "utf8");
const taskModel = await readFile(new URL("../components/workflow/taskModel.ts", import.meta.url), "utf8");
const taskApi = await readFile(new URL("../components/workflow/taskApi.ts", import.meta.url), "utf8");
const reportApi = await readFile(new URL("../components/report/reportExportApi.ts", import.meta.url), "utf8");
const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");
const {
  fitChartYRange,
  normalizeChartView,
  panChartView,
  panChartYRange,
  zoomChartView,
  zoomChartYRange,
} = await import("../components/playback/clearanceChartViewport.ts");

test("capture, playback and report share persisted tasks and one browser selection", () => {
  assert.match(page, /const \[tasks, setTasks\] = useState<CollectionTask\[]>\(\[\]\)/);
  assert.match(page, /const \[selectedTaskId, setSelectedTaskId\] = useState<string \| null>\(null\)/);
  assert.match(page, /await listTasks\(\)/);
  assert.doesNotMatch(page, /selectedBatchId|listBatches\(/);
  assert.match(page, /<PlaybackWorkspace[\s\S]*selectedTaskId=\{selectedTaskId\}/);
  assert.match(page, /<ReportWorkspace[\s\S]*selectedTaskId=\{selectedTaskId\}/);
});

test("time identifiers replace visible operation-batch task numbering", () => {
  assert.match(taskModel, /displayId:\s*string/);
  assert.match(taskModel, /taskDateKey/);
  assert.match(page, /任务编号由设备端按创建时间生成/);
  assert.match(page, /task\.displayId/);
  assert.doesNotMatch(page, /batchMode|selectedBatchId|BatchSelector|formatTaskSequence/);
  assert.doesNotMatch(playback, /BatchSelector|purgeBatch/);
  assert.doesNotMatch(report, /BatchSelector|archiveBatch|completeBatch/);
});

test("task model uses UUID internally and creation-time display identifiers without task-name fields", () => {
  assert.match(taskModel, /taskId:\s*string/);
  assert.match(taskModel, /displayId:\s*string/);
  assert.doesNotMatch(taskModel, /taskName|batchId|batchSequence|globalSequence/);
  assert.doesNotMatch(`${page}${taskBrowser}${playback}${report}`, /task\.taskName|selectedTask\.taskName/);
});

test("stopped tasks expose direct playback and report navigation", () => {
  assert.match(page, /currentTask\.status === "已停止"/);
  assert.match(page, />查看数据<\/button>/);
  assert.match(page, />报告检查<\/button>/);
  assert.match(page, /onNavigate\("playback"\)/);
  assert.match(page, /onNavigate\("report"\)/);
});

test("playback task browser follows task creation order", () => {
  assert.match(playback, /heading="选择回放任务"[\s\S]*sortOrder="asc"/);
  assert.match(taskBrowser, /sortOrder\?: "asc" \| "desc"/);
  assert.match(taskBrowser, /const direction=sortOrder==="asc"\?1:-1/);
});

test("playback keeps one task curve workspace and supports filtered multi-task deletion", () => {
  assert.match(playback, /选择回放任务/);
  assert.match(playback, /净空高度曲线/);
  assert.match(playback, /50 Hz 测量序列/);
  assert.match(playback, /统计与数据质量/);
  assert.match(playback, /deleteSelectedTasks\(deleteIds\)/);
  assert.match(playback, /删除所选任务/);
  assert.match(playback, /showSelectAll/);
  assert.match(taskBrowser, /取消全选|全选/);
  assert.match(taskBrowser, /formatTaskDateKey\(dateKey\)/);
  assert.match(taskBrowser, /选择当日/);
  assert.doesNotMatch(playback, /playback-control-panel|playback-scrubber|点云回放|地图轨迹|PlaybackView/);
});

test("interactive clearance chart supports pan, zoom, reset and hover without mock data", () => {
  assert.match(interactiveChart, /addEventListener\("wheel", handleWheel, \{ passive: false \}\)/);
  assert.match(interactiveChart, /event\.preventDefault\(\)/);
  assert.match(interactiveChart, /event\.stopPropagation\(\)/);
  assert.doesNotMatch(interactiveChart, /onWheel=/);
  assert.match(interactiveChart, /onPointerDown=\{handlePointerDown\}/);
  assert.match(interactiveChart, /setPointerCapture/);
  assert.match(interactiveChart, /onDoubleClick=\{resetView\}/);
  assert.match(interactiveChart, /拖拽：左右、上下平移/);
  assert.match(interactiveChart, /滚轮：横向缩放/);
  assert.match(interactiveChart, /Shift\+滚轮：纵向缩放/);
  assert.match(interactiveChart, /zoomVertically/);
  assert.match(interactiveChart, /纵向放大曲线/);
  assert.match(interactiveChart, /纵向缩小曲线/);
  assert.match(interactiveChart, /纵轴已锁定/);
  assert.doesNotMatch(interactiveChart, /适配当前窗口|fitVerticalRange/);
  assert.match(interactiveChart, /pendingDragYRangeRef/);
  assert.match(interactiveChart, /outOfRange\.above/);
  assert.match(interactiveChart, /outOfRange\.below/);
  assert.match(interactiveChart, /requestAnimationFrame/);
  assert.doesNotMatch(interactiveChart, /\[verticalZoom, visibleSamples\]/);
  assert.match(interactiveChart, /回到开头/);
  assert.match(interactiveChart, /onKeyDown=\{handleKeyDown\}/);
  assert.match(interactiveChart, /heightM:\s*number \| null/);
  assert.match(interactiveChart, /sampleIndex:\s*number/);
  assert.match(interactiveChart, /sample\.timestampMs - windowStart/);
  assert.match(interactiveChart, /Math\.abs\(sample\.timestampMs - targetTime\)/);
  assert.doesNotMatch(interactiveChart, /xToPercent\(sample\.sampleIndex\)/);
  assert.match(playback, /startTimestampMs: request\.startTimestampMs/);
  assert.match(playback, /endTimestampMs: request\.endTimestampMs/);
  assert.match(interactiveChart, /!sample\.valid \|\| sample\.heightM === null/);
  assert.doesNotMatch(interactiveChart, /mock|demo|Math\.random|模拟曲线/);
  assert.match(playback, /loadMeasurementPrefix\(selectedTaskId/);
  assert.match(playback, /loadMeasurementSummary\(selectedTaskId/);
  assert.match(playback, /loadMeasurementSeries\(selectedTaskId/);
  assert.match(playback, /mergeSeriesIntoCache\(seriesCacheRef\.current, nextSeries\)/);
  assert.match(playback, /isSeriesWindowCached/);
  assert.match(playback, /series cache hit/);
  assert.match(playbackCache, /samplesByIndex/);
  assert.match(playbackCache, /targetResolutionMs/);
  assert.match(playback, /INITIAL_PREFIX_SAMPLES = 2000/);
  assert.match(playback, /SUMMARY_LOAD_DELAY_MS = 100/);
  assert.match(playback, /DETAIL_SERIES_POINTS = 6000/);
  assert.match(playback, /setTimeout/);
  assert.match(playback, /samples=\{series\?\.samples \?\? \[]\}/);
  assert.match(playback, /保留局部最低值、最高值和无效断点/);
  assert.match(playback, /首屏只读取任务最前面的固定样本段/);
  assert.match(interactiveChart, /initialView \?\? \{ start: 0, end: 1 \}/);
  assert.match(interactiveChart, /onViewWindowChange\?\.\(view\)/);
  assert.match(interactiveChart, /reportedInitialViewRef/);
  assert.match(playback, /userViewRevisionRef\.current === 0/);
  assert.match(playback, /\[selectedTaskId, selectedTask\?\.hasMeasurements\]/);
  assert.match(playbackWindow, /if \(userViewRevision <= 0\) return null/);
  assert.match(playback, /prefixTimer = window\.setTimeout\(beginInitialLoad, 0\)/);
  assert.match(playback, /SUMMARY_RETRY_DELAY_MS = 500/);
  assert.match(playback, /isSupersededError\(error\)/);
  assert.match(interactiveChart, /const lineSegments = useMemo/);
  assert.doesNotMatch(playbackCoordinator, /loadMeasurementSeries|\/measurements(?:["'`])/);
  assert.doesNotMatch(playback, /\}, \[selectedTask\]\)/);
});

test("playback removes physical cleanup from customer UI and deletes selected tasks logically", () => {
  assert.match(playback, /确定删除所选 \${deleteIds\.length} 个任务吗？/);
  assert.match(playback, /deleteSelectedTasks\(deleteIds\)/);
  assert.match(taskApi, /\/api\/v1\/tasks\/delete-selected/);
  assert.doesNotMatch(playback, /清理所选数据|purgeTaskData|任务索引|measurements\.db/);
  assert.doesNotMatch(taskApi, /\/api\/v1\/tasks\/purge-data/);
  assert.match(taskBrowser, /disabledTaskIds/);
  assert.match(css, /\.button--danger-outline/);
});

test("playback distinguishes loading, ready, empty, cleaned and failed history states", () => {
  assert.match(playback, /type HistoryState = "idle" \| "loading" \| "empty" \| "ready" \| "error"/);
  assert.match(playback, /界面测试数据/);
  assert.match(playback, /实际平均频率/);
  assert.match(playback, /有效采样比例/);
  assert.match(playback, /本地测量数据已清理/);
  assert.match(playback, /异常中断或未完整结束/);
});

test("clearance chart viewport math keeps zoom and pan inside the full task range", () => {
  const assertView = (actual, expected) => {
    assert.ok(Math.abs(actual.start - expected.start) < 1e-12);
    assert.ok(Math.abs(actual.end - expected.end) < 1e-12);
  };
  assertView(normalizeChartView(-0.2, 0.4), { start: 0, end: 0.6 });
  assertView(normalizeChartView(0.8, 1.4), { start: 0.4, end: 1 });
  const zoomed = zoomChartView({ start: 0, end: 1 }, 0.5, 0.5);
  assertView(zoomed, { start: 0.25, end: 0.75 });
  assertView(panChartView(zoomed, -2), { start: 0, end: 0.5 });
  assertView(panChartView(zoomed, 2), { start: 0.5, end: 1 });
});

test("clearance chart keeps a stable fitted y range until the user changes it", () => {
  const fitted = fitChartYRange([4.2, 4.4]);
  assert.deepEqual(fitted, { minimum: 4.176, maximum: 4.424 });
  const zoomed = zoomChartYRange(fitted, 2);
  assert.ok(Math.abs(zoomed.minimum - 4.238) < 1e-12);
  assert.ok(Math.abs(zoomed.maximum - 4.362) < 1e-12);
  assert.equal(fitChartYRange([Number.NaN, Number.POSITIVE_INFINITY]), null);
  assert.deepEqual(panChartYRange({ minimum: 4, maximum: 5 }, 0.5), {
    minimum: 4.5,
    maximum: 5.5,
  });
  assert.deepEqual(panChartYRange({ minimum: 0.1, maximum: 1.1 }, -0.5), {
    minimum: 0,
    maximum: 1,
  });
  assert.match(interactiveChart, /const \[yRange, setYRange\] = useState/);
  assert.match(interactiveChart, /setYRange\(initialYRangeRef\.current\)/);
});

test("report removes task-name fields and aggregates only user-selected tasks", () => {
  assert.match(report, /50 Hz 测量明细/);
  assert.match(report, /隧道净空检测汇总/);
  assert.match(report, /任务编号/);
  assert.match(report, /隧道编号/);
  assert.match(report, /检测车道/);
  assert.match(report, /最低高度/);
  assert.match(report, /隧道入口 RTK/);
  assert.match(report, /隧道出口 RTK/);
  assert.match(report, /checkedTaskIds=\{checked\}/);
  assert.match(report, /heading="选择导出任务" sortOrder="asc"/);
  assert.match(report, /generateSummaryPdf\(selectedIds\)/);
  assert.doesNotMatch(report, /任务名称|taskName|batchId|selectedBatch/);
});

test("report enables formal exports only for eligible recorded selected data", () => {
  assert.match(report, /loadReportPreview\(selectedIds\)/);
  assert.match(report, /generateTaskTxt\(selectedTask\.taskId\)/);
  assert.match(report, /generateSummaryPdf\(selectedIds\)/);
  assert.match(report, /downloadGeneratedFile/);
  assert.match(report, /disabled=\{!txtReady\|\|txtState==="generating"\}/);
  assert.match(report, /disabled=\{!pdfReady\|\|pdfState==="generating"\}/);
  assert.match(report, /pdfExportableTasks/);
  assert.match(report, /normalMinimumHeightM/);
  assert.match(report, /低于高度下限阈值、超过高度上限阈值/);
  assert.match(report, /异常中断、测试数据和无有效高度的任务不会进入正式 PDF/);
  assert.match(reportApi, /task_ids:taskIds/);
});

test("task browser searches time identifiers and tunnel metadata and groups by date", () => {
  assert.match(taskBrowser, /时间编号、隧道编号或名称/);
  assert.match(taskBrowser, /task\.displayId\.toLowerCase/);
  assert.match(taskBrowser, /task\.tunnelCode\.toLowerCase/);
  assert.match(taskBrowser, /task\.tunnelName\.toLowerCase/);
  assert.match(taskBrowser, /taskDateKey\(task\)/);
  assert.match(taskBrowser, /formatTaskDateKey\(dateKey\)/);
  assert.match(taskBrowser, /任务编号由设备端创建时间生成/);
  assert.doesNotMatch(taskBrowser, /formatTaskSequence|task\.taskName/);
});

test("playback main cards share one row height and y-axis title has normal chart orientation", () => {
  assert.match(css, /\.playback-layout > \.task-browser, \.playback-clearance-main, \.playback-inspector \{ height: 100%; \}/);
  assert.match(css, /\.playback-clearance-panel \{[^}]*height: 100%;/);
  assert.match(css, /\.interactive-clearance-chart__y-axis strong \{[^}]*writing-mode: horizontal-tb;[^}]*transform: rotate\(-90deg\);/);
  assert.doesNotMatch(css, /interactive-clearance-chart__y-axis strong[^}]*vertical-rl/);
});

test("workflow layout keeps the clearance curve prominent on wide and notebook screens", () => {
  assert.match(css, /\.playback-layout\s*\{[^}]*grid-template-columns:\s*320px minmax\(680px, 1fr\) 360px/i);
  assert.match(css, /\.interactive-clearance-chart\s*\{[^}]*min-height:\s*560px/i);
  assert.match(css, /\.report-simple-layout\s*\{[^}]*grid-template-columns:\s*320px minmax\(0, 1fr\)/i);
  assert.match(css, /\.report-export-grid\s*\{[^}]*grid-template-columns:\s*repeat\(2, minmax\(0, 1fr\)\)/i);
  assert.match(css, /@media \(max-width:\s*1700px\)[\s\S]*\.report-export-grid \{ grid-template-columns:\s*1fr/i);
  assert.match(css, /@media \(max-width:\s*1500px\) and \(min-width:\s*761px\)[\s\S]*\.playback-layout \{ grid-template-columns:\s*300px minmax\(0, 1fr\)/i);
  assert.match(css, /@media \(max-width:\s*1320px\)[\s\S]*\.report-simple-layout \{ grid-template-columns:\s*1fr/i);
});

test("playback and report retain readable notebook text and controls", () => {
  assert.match(css, /\.workflow-context-bar__identity > strong \{[^}]*font-size:\s*19px/i);
  assert.match(css, /\.task-browser__filters button \{[^}]*min-height:\s*36px[^}]*font-size:\s*12px/i);
  assert.match(css, /\.interactive-clearance-chart-toolbar__actions button \{[^}]*min-width:\s*40px[^}]*min-height:\s*38px[^}]*font-size:\s*13px/i);
  assert.match(css, /\.playback-clearance-panel__head h2 \{[^}]*font-size:\s*20px/i);
  assert.match(css, /\.inspector-section dt \{[^}]*font-size:\s*12px/i);
  assert.match(css, /\.report-export-card__head h2 \{[^}]*font-size:\s*19px/i);
  assert.match(css, /\.report-export-card__footer \.button \{[^}]*min-height:\s*42px[^}]*font-size:\s*13px/i);
  assert.match(css, /\.task-browser-row__main\s*\{[^}]*min-height:\s*70px/i);
});
