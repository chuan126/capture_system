import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");
const playback = await readFile(new URL("../components/playback/PlaybackWorkspace.tsx", import.meta.url), "utf8");
const interactiveChart = await readFile(new URL("../components/playback/InteractiveClearanceChart.tsx", import.meta.url), "utf8");
const report = await readFile(new URL("../components/report/ReportWorkspace.tsx", import.meta.url), "utf8");
const taskBrowser = await readFile(new URL("../components/workflow/TaskBrowser.tsx", import.meta.url), "utf8");
const taskModel = await readFile(new URL("../components/workflow/taskModel.ts", import.meta.url), "utf8");
const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");
const { normalizeChartView, panChartView, zoomChartView } = await import(
  "../components/playback/clearanceChartViewport.ts"
);

test("capture, playback and report share one in-memory task selection", () => {
  assert.match(page, /const \[tasks, setTasks\] = useState<CollectionTask\[]>\(\[\]\)/);
  assert.match(page, /const \[selectedTaskId, setSelectedTaskId\] = useState<string \| null>\(null\)/);
  assert.match(page, /<Dashboard[\s\S]*tasks=\{tasks\}[\s\S]*setTasks=\{setTasks\}/);
  assert.match(page, /<PlaybackWorkspace[\s\S]*selectedTaskId=\{selectedTaskId\}/);
  assert.match(page, /<ReportWorkspace[\s\S]*selectedTaskId=\{selectedTaskId\}/);
});

test("task model uses automatic sequence numbers without a task-name field", () => {
  assert.match(taskModel, /sequence:\s*number/);
  assert.match(taskModel, /formatTaskSequence/);
  assert.doesNotMatch(taskModel, /taskName/);
  assert.doesNotMatch(`${page}${taskBrowser}${playback}${report}`, /task\.taskName|selectedTask\.taskName/);
});

test("stopped tasks expose direct playback and report navigation", () => {
  assert.match(page, /currentTask\.status === "已停止"/);
  assert.match(page, />查看数据<\/button>/);
  assert.match(page, />报告检查<\/button>/);
  assert.match(page, /onNavigate\("playback"\)/);
  assert.match(page, /onNavigate\("report"\)/);
});

test("playback keeps one complete task curve and removes playback controls", () => {
  assert.match(playback, /选择回放任务/);
  assert.match(playback, /完整净空高度曲线/);
  assert.match(playback, /50 Hz 测量序列/);
  assert.match(playback, /统计与数据质量/);
  assert.match(playback, /隧道端点/);
  assert.match(playback, /页面不会使用实时流或模拟曲线替代历史记录/);
  assert.match(playback, /<InteractiveClearanceChart/);
  assert.doesNotMatch(playback, /playback-control-panel|playback-scrubber|曲线回放控制|回放速度|跳转最低值|下一个异常/);
  assert.doesNotMatch(playback, /点云回放|地图轨迹|回放主视图|PlaybackView|useState/);
  assert.doesNotMatch(playback, /minimum_clearance|clearance_points/);
});

test("interactive clearance chart supports pan, zoom, reset and hover without mock data", () => {
  assert.match(interactiveChart, /onWheel=\{handleWheel\}/);
  assert.match(interactiveChart, /onPointerDown=\{handlePointerDown\}/);
  assert.match(interactiveChart, /setPointerCapture/);
  assert.match(interactiveChart, /onDoubleClick=\{resetView\}/);
  assert.match(interactiveChart, /拖拽平移/);
  assert.match(interactiveChart, /滚轮缩放/);
  assert.match(interactiveChart, /重置视图/);
  assert.match(interactiveChart, /onKeyDown=\{handleKeyDown\}/);
  assert.match(interactiveChart, /heightM:\s*number \| null/);
  assert.match(interactiveChart, /!sample\.valid \|\| sample\.heightM === null/);
  assert.doesNotMatch(interactiveChart, /mock|demo|Math\.random|模拟曲线/);
  assert.match(playback, /const EMPTY_CLEARANCE_SAMPLES: ClearanceSample\[] = \[]/);
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

  const leftBound = panChartView(zoomed, -2);
  assertView(leftBound, { start: 0, end: 0.5 });

  const rightBound = panChartView(zoomed, 2);
  assertView(rightBound, { start: 0.5, end: 1 });
});

test("report removes task-name fields and keeps only TXT and PDF exports", () => {
  assert.match(report, /50 Hz 测量明细/);
  assert.match(report, /隧道净空检测汇总/);
  assert.match(report, /记录时间/);
  assert.match(report, /隧道编号/);
  assert.match(report, /检测车道/);
  assert.match(report, /实时高度/);
  assert.match(report, /最低高度/);
  assert.match(report, /隧道入口 RTK/);
  assert.match(report, /隧道出口 RTK/);
  assert.match(report, /每个有效采样时刻输出一行/);
  assert.match(report, /tasks\.map/);
  assert.doesNotMatch(report, /任务名称|taskName|按任务名称归并/);
  assert.doesNotMatch(report, /任务与数据检查|报告内容配置|报告预览与导出|导出记录/);
  assert.doesNotMatch(report, /report-export-test|browser-download-test|模拟净空/);
});

test("report keeps exports disabled until real recorded data is available", () => {
  assert.match(report, /50 Hz 测量记录、最低高度和隧道端点 RTK 接口尚未接入/);
  assert.match(report, /<button type="button" className="button button--primary" disabled>导出 TXT<\/button>/);
  assert.match(report, /<button type="button" className="button button--primary" disabled>导出 PDF<\/button>/);
  assert.match(report, /无有效 RTK 坐标时保持空值并注明原因/);
});

test("task browser searches by automatic number and tunnel metadata", () => {
  assert.match(taskBrowser, /当前列表来自本次浏览器会话/);
  assert.match(taskBrowser, /任务编号、隧道编号或名称/);
  assert.match(taskBrowser, /formatTaskSequence\(task\.sequence\)/);
  assert.match(taskBrowser, /task\.tunnelCode\.toLowerCase/);
  assert.match(taskBrowser, /task\.tunnelName\.toLowerCase/);
  assert.doesNotMatch(taskBrowser, /task\.taskName/);
  assert.match(taskBrowser, />已停止</);
  assert.match(taskBrowser, />未结束</);
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
  assert.match(css, /\.report-field-list strong \{[^}]*font-size:\s*12px/i);
  assert.match(css, /\.report-export-card__footer \.button \{[^}]*min-height:\s*42px[^}]*font-size:\s*13px/i);
});
