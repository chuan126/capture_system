import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");
const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");
const socket = await readFile(new URL("../components/system-status/useSystemStatusSocket.ts", import.meta.url), "utf8");

const taskCard = page.match(/<article className="panel task-operation-panel">([\s\S]*?)<\/article>\n        <\/aside>/)?.[1] ?? "";
const taskDialog = page.match(/function TaskCreateDialog\([\s\S]*?\nconst navigation:/)?.[0] ?? "";

test("system overview uses the revised labels and removes diagnostic helper copy", () => {
  assert.match(page, /warn: "系统告警"/);
  assert.match(page, /unknown: "检查中"/);
  assert.doesNotMatch(page, /系统有告警|系统检查中|状态灯依据实时设备诊断判断/);
  assert.doesNotMatch(page, /<p>\{systemStatus\.detail\}<\/p>/);
});

test("system overview contains current clearance and two reserved metric cards", () => {
  assert.match(page, /health-kpi-grid/);
  assert.match(page, />净空高度</);
  assert.match(page, /\{currentHeightText\}<small>m<\/small>/);
  assert.match(page, />预留指标一</);
  assert.match(page, />预留指标二</);
  assert.match(css, /health-kpi-grid[\s\S]*grid-template-columns:\s*repeat\(3/);
});

test("capture sidebar is split into RTK and task control cards", () => {
  assert.match(page, /dashboard-side-stack/);
  assert.match(page, /className="panel rtk-control-panel"/);
  assert.match(page, /title="RTK 定位"/);
  assert.match(page, /className="panel task-operation-panel"/);
  assert.match(page, /title="任务控制"/);
  assert.doesNotMatch(page, /title="测量与任务控制"/);
  assert.match(css, /dashboard-side-stack[\s\S]*grid-template-rows:/);
});

test("device cards remain in a two-by-two grid with explicit connection lamps", () => {
  assert.match(css, /health-device-grid[\s\S]*grid-template-columns:\s*repeat\(2/);
  assert.match(css, /health-device-grid[\s\S]*grid-template-rows:\s*repeat\(2/);
  assert.match(page, /health-device-card__lamp/);
});

test("top metric cards use enlarged top-left labels and centered values", () => {
  assert.match(css, /health-kpi-card > span[^{]*\{[^}]*font-size:\s*clamp\(12px/);
  assert.match(css, /health-kpi-card > strong[^{]*\{[^}]*justify-self:\s*center/);
  assert.match(css, /health-kpi-card > strong[^{]*\{[^}]*clamp\(29px/);
});

test("top device cards use enlarged identity icons and labels", () => {
  assert.match(css, /health-device-card__icon[\s\S]*width:\s*34px/);
  assert.match(css, /health-device-card__identity b[^{]*\{[^}]*font-size:\s*11px/);
  assert.match(css, /health-device-card__identity small[^{]*\{[^}]*font-size:\s*9px/);
});

test("RTK card uses three equal metrics followed by entrance and exit rows", () => {
  assert.match(page, /className="rtk-metric-grid"/);
  assert.match(page, />卫星数</);
  assert.match(page, />HDOP \/ PDOP</);
  assert.match(page, />高度</);
  assert.match(page, /className="rtk-coordinate-stack"/);
  assert.match(page, />入口坐标</);
  assert.match(page, />出口坐标</);
  assert.match(css, /rtk-metric-grid[\s\S]*grid-template-columns:\s*repeat\(3/);
});

test("system status socket clears stale snapshots", () => {
  assert.match(socket, /SNAPSHOT_TIMEOUT_MS = 5000/);
  assert.match(socket, /snapshot:\s*null/);
  assert.match(socket, /超过 5 秒未收到设备诊断数据/);
});

test("task card exposes only one create-task entry", () => {
  assert.match(taskCard, />创建任务<\/button>/);
  assert.doesNotMatch(taskCard, />新建任务<\/button>|>批量创建<\/button>/);
  assert.match(page, /setTaskDialogOpen\(true\)/);
});

test("height threshold is a shared task-card setting and not a per-task field", () => {
  assert.match(taskCard, /task-threshold-card/);
  assert.match(taskCard, />高度阈值</);
  assert.match(taskCard, />所有任务共用</);
  assert.match(taskCard, /value=\{heightThreshold\}/);
  assert.doesNotMatch(taskDialog, />高度阈值 \/ m</);
  assert.doesNotMatch(page, /heightThreshold:\s*Number\(draft\.heightThreshold\)/);
});

test("task creation supports one or many rows with the required task fields", () => {
  assert.match(taskDialog, />创建检测任务</);
  assert.match(taskDialog, />任务编号</);
  assert.match(taskDialog, />隧道名称</);
  assert.match(taskDialog, />雷达安装高度 \/ m</);
  assert.match(taskDialog, />作业车道</);
  assert.match(taskDialog, /option value="左车道"/);
  assert.match(taskDialog, /option value="右车道"/);
  assert.match(taskDialog, />＋ 添加任务</);
  assert.match(taskDialog, />复制<\/button>/);
  assert.match(taskDialog, />删除<\/button>/);
});

test("task card follows threshold, selection, pending tasks and controls order", () => {
  const thresholdIndex = taskCard.indexOf("task-threshold-card");
  const selectionIndex = taskCard.indexOf("task-selection-card");
  const queueIndex = taskCard.indexOf("task-queue-preview");
  const controlsIndex = taskCard.indexOf("task-operation-actions");

  assert.ok(thresholdIndex >= 0);
  assert.ok(selectionIndex > thresholdIndex);
  assert.ok(queueIndex > selectionIndex);
  assert.ok(controlsIndex > queueIndex);
  assert.match(taskCard, />选择任务</);
  assert.match(taskCard, />待测任务</);
  assert.match(taskCard, />开始采集</);
  assert.match(taskCard, /暂停/);
  assert.match(taskCard, />停止</);
});

test("task card omits progress, measurement and readiness sections", () => {
  assert.doesNotMatch(taskCard, /阶段进度|任务进度|采集时长|有效帧数|当前净空|最低净空|采集条件/);
  assert.doesNotMatch(taskCard, /task-progress-line|task-readiness/);
});

test("dedicated task-management interface is removed", () => {
  assert.doesNotMatch(page, /id: "tasks"|function Tasks\(|任务管理|历史任务管理/);
  assert.match(page, /\{ id: "playback", label: "数据回放", index: "02" \}/);
  assert.match(page, /\{ id: "report", label: "报告导出", index: "03" \}/);
  assert.doesNotMatch(css, /\.task-overview|\.task-page-grid|\.task-form|\.queue-rule/);
});

test("task dialog and compact queue retain responsive styling", () => {
  assert.match(css, /task-dialog-mask/);
  assert.match(css, /task-dialog-panel--wide/);
  assert.match(css, /task-queue-preview__list/);
  assert.match(css, /task-threshold-card/);
  assert.match(css, /task-selection-card/);
});
