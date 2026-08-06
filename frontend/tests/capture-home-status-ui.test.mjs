import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");
const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");
const socket = await readFile(new URL("../components/system-status/useSystemStatusSocket.ts", import.meta.url), "utf8");

const taskCard = page.match(/<article className="panel task-operation-panel">([\s\S]*?)<\/article>\n        <\/aside>/)?.[1] ?? "";
const rtkCard = page.match(/<article className="panel rtk-control-panel">([\s\S]*?)<\/article>\n\n          <article className="panel task-operation-panel">/)?.[1] ?? "";
const taskDialog = page.match(/function TaskCreateDialog\([\s\S]*?\nconst navigation:/)?.[0] ?? "";

test("system overview uses the revised labels and removes diagnostic helper copy", () => {
  assert.match(page, /warn: "系统告警"/);
  assert.match(page, /unknown: "检查中"/);
  assert.doesNotMatch(page, /系统有告警|系统检查中|状态灯依据实时设备诊断判断/);
  assert.doesNotMatch(page, /<p>\{systemStatus\.detail\}<\/p>/);
});

test("system overview contains clearance, current RTK coordinate and one reserved metric", () => {
  assert.match(page, /health-kpi-grid/);
  assert.match(page, />净空高度</);
  assert.match(page, /\{currentHeightText\}<small>m<\/small>/);
  assert.match(page, />当前坐标</);
  assert.match(page, /\{longitudeText\}/);
  assert.match(page, /\{latitudeText\}/);
  assert.match(page, />预留指标二</);
  assert.doesNotMatch(page, />预留指标一</);
  assert.match(css, /health-kpi-grid[\s\S]*grid-template-columns:\s*repeat\(3/);
  assert.match(css, /health-kpi-card--coordinate/);
});

test("current coordinate requires a valid streaming RTK fix", () => {
  assert.match(page, /rtk\.connection === "connected"/);
  assert.match(page, /rtk\.streamState === "streaming"/);
  assert.match(page, /rmcCharacter !== "V"/);
  assert.match(page, /latitudeValue\.toFixed\(7\)/);
  assert.match(page, /longitudeValue\.toFixed\(7\)/);
});

test("capture sidebar is split into compact RTK and expanded task control cards", () => {
  assert.match(page, /dashboard-side-stack/);
  assert.match(page, /className="panel rtk-control-panel"/);
  assert.match(page, /className="panel task-operation-panel"/);
  assert.match(css, /dashboard-side-stack[\s\S]*grid-template-rows:\s*auto minmax\(0, 1fr\)/);
  assert.match(css, /rtk-control-panel \{ flex:\s*0 0 auto/);
});

test("RTK card keeps only three equal quality metrics", () => {
  assert.match(rtkCard, /className="rtk-metric-grid"/);
  assert.match(rtkCard, />卫星数</);
  assert.match(rtkCard, />HDOP \/ PDOP</);
  assert.match(rtkCard, />高度</);
  assert.doesNotMatch(rtkCard, /入口坐标|出口坐标|rtk-coordinate-stack/);
  assert.match(css, /rtk-metric-grid[\s\S]*grid-template-columns:\s*repeat\(3/);
  assert.doesNotMatch(css, /\.rtk-coordinate-stack/);
});

test("device cards remain in a two-by-two grid with explicit connection lamps", () => {
  assert.match(css, /health-device-grid[\s\S]*grid-template-columns:\s*repeat\(2/);
  assert.match(css, /health-device-grid[\s\S]*grid-template-rows:\s*repeat\(2/);
  assert.match(page, /health-device-card__lamp/);
});

test("system status socket clears stale snapshots", () => {
  assert.match(socket, /SNAPSHOT_TIMEOUT_MS = 5000/);
  assert.match(socket, /snapshot:\s*null/);
  assert.match(socket, /超过 5 秒未收到设备诊断数据/);
});

test("task card exposes only one create-task entry", () => {
  assert.match(taskCard, />创建任务<\/button>/);
  assert.doesNotMatch(taskCard, />新建任务<\/button>|>批量创建<\/button>/);
});

test("height threshold, mount height and lane are shared task-card settings", () => {
  assert.match(taskCard, /task-parameter-strip/);
  assert.match(taskCard, />高度阈值</);
  assert.match(taskCard, /value=\{heightThreshold\}/);
  assert.match(taskCard, />雷达安装高度</);
  assert.match(taskCard, /value=\{mountHeight\}/);
  assert.match(taskCard, />作业车道</);
  assert.match(taskCard, /value=\{operationLane\}/);
  assert.match(taskCard, /option value="左车道"/);
  assert.match(taskCard, /option value="右车道"/);
  assert.doesNotMatch(taskDialog, />高度阈值/);
  assert.doesNotMatch(taskDialog, />雷达安装高度/);
  assert.doesNotMatch(taskDialog, />作业车道/);
});

test("task creation uses automatic sequence numbers and only asks for tunnel fields", () => {
  assert.match(taskDialog, />创建检测任务</);
  assert.match(taskDialog, /任务编号由系统按创建顺序自动生成/);
  assert.match(taskDialog, />隧道编号</);
  assert.match(taskDialog, />隧道名称</);
  assert.match(taskDialog, />＋ 添加任务</);
  assert.match(taskDialog, />复制<\/button>/);
  assert.match(taskDialog, />删除<\/button>/);
  assert.match(taskDialog, /row\.tunnelCode/);
  assert.match(taskDialog, /row\.tunnelName/);
  assert.doesNotMatch(taskDialog, />任务名称|row\.taskName/);
  assert.match(page, /createTaskId\(sequence, draft\.tunnelCode\)/);
  assert.match(page, /sequence,\s*\n\s*tunnelCode:/);
  assert.match(page, /formatTaskSequence\(task\.sequence\)/);
  assert.doesNotMatch(taskDialog, /row\.mountHeight|row\.lane/);
});

test("task card follows parameters, current task, pending tasks and controls order", () => {
  const settingsIndex = taskCard.indexOf("task-parameter-strip");
  const currentIndex = taskCard.indexOf("task-current-card");
  const queueIndex = taskCard.indexOf("task-queue-section");
  const controlsIndex = taskCard.indexOf("task-operation-actions");

  assert.ok(settingsIndex >= 0);
  assert.ok(currentIndex > settingsIndex);
  assert.ok(queueIndex > currentIndex);
  assert.ok(controlsIndex > queueIndex);
  assert.doesNotMatch(taskCard, /task-selection-card|选择检测任务/);
});

test("task card uses task-operation-body as its only internal vertical scroll area", () => {
  assert.match(taskCard, /task-operation-body/);
  assert.match(taskCard, /task-queue-list/);

  const bodyRule = css.match(/\.task-operation-body\s*\{([^}]+)\}/)?.[1] ?? "";
  const queueRule = css.match(/\.task-queue-list\s*\{([^}]+)\}/)?.[1] ?? "";

  assert.match(bodyRule, /min-height:\s*0/);
  assert.match(bodyRule, /overflow-y:\s*auto/);
  assert.match(queueRule, /max-height:\s*none/);
  assert.match(queueRule, /overflow:\s*visible/);
  assert.doesNotMatch(queueRule, /overflow-y:\s*(auto|scroll)/);
});

test("current task provides a dedicated switch dialog", () => {
  assert.match(taskCard, /切换任务/);
  assert.match(page, /function TaskSwitchDialog/);
  assert.match(page, /task-switch-list/);
  assert.match(page, /setTaskSwitchOpen\(true\)/);
});

test("task actions use a dominant start control and outlined secondary controls", () => {
  assert.match(taskCard, /task-start-button/);
  assert.match(taskCard, /task-pause-button/);
  assert.match(taskCard, /task-stop-button/);
  assert.match(taskCard, /task-running-state/);
  assert.match(css, /task-operation-actions[\s\S]*grid-template-columns:\s*1\.55fr 1fr 1fr/);
});

test("task start validates the shared numeric settings", () => {
  assert.match(page, /const mountHeightValid/);
  assert.match(page, /!heightThresholdValid \|\|[\s\S]*!mountHeightValid/);
  assert.match(page, /请输入有效的雷达安装高度/);
  assert.match(page, /status: "采集中", lane: operationLane/);
});

test("task card omits progress and measurement sections", () => {
  assert.doesNotMatch(taskCard, /阶段进度|任务进度|采集时长|有效帧数|当前净空|最低净空|采集条件/);
});

test("dedicated task-management interface remains removed", () => {
  assert.doesNotMatch(page, /id: "tasks"|function Tasks\(|任务管理|历史任务管理/);
  assert.match(page, /\{ id: "playback", label: "数据回放", index: "02" \}/);
  assert.match(page, /\{ id: "report", label: "报告导出", index: "03" \}/);
});



test("notebook task card keeps the middle scrollable without reducing readable controls", () => {
  assert.match(css, /@media \(max-width:\s*1600px\) and \(min-width:\s*761px\)[\s\S]*?\.task-operation-panel \{[^}]*min-height:\s*0[^}]*overflow:\s*hidden/i);
  assert.match(css, /@media \(max-width:\s*1600px\) and \(min-width:\s*761px\)[\s\S]*?\.task-operation-body \{[^}]*flex:\s*1 1 auto[^}]*overflow-y:\s*auto/i);
  assert.match(css, /@media \(max-width:\s*1600px\) and \(min-width:\s*761px\)[\s\S]*?\.task-queue-list \{[^}]*max-height:\s*none[^}]*overflow:\s*visible/i);
  assert.doesNotMatch(css, /@media \(max-width:\s*1600px\) and \(min-width:\s*761px\)[\s\S]*?\.task-operation-panel \{[^}]*height:\s*auto[^}]*overflow:\s*visible !important/i);
  assert.doesNotMatch(css, /@media \(max-width:\s*1600px\) and \(min-width:\s*761px\)[\s\S]*?\.task-queue-list \{[^}]*max-height:\s*260px/i);
  assert.match(css, /\.task-operation-actions \.button \{[^}]*min-height:\s*42px[^}]*font-size:\s*11px/i);
  assert.match(css, /\.task-queue-list > button \{[^}]*min-height:\s*52px/i);
});

test("task title and bottom controls stay outside the scrollable task body", () => {
  const bodyIndex = taskCard.indexOf('className="task-operation-body"');
  const actionsIndex = taskCard.indexOf('className="task-operation-actions"');
  assert.ok(bodyIndex >= 0);
  assert.ok(actionsIndex > bodyIndex);
  assert.match(css, /\.task-operation-panel\s*\{[^}]*display:\s*flex[^}]*flex-direction:\s*column[^}]*overflow:\s*hidden/i);
  assert.match(css, /\.task-operation-actions\s*\{[^}]*flex:\s*0 0 auto/i);
});

test("capture dashboard keeps notebook text, icons and controls readable", () => {
  assert.match(css, /\.dashboard-page\s*\{[^}]*grid-template-rows:\s*clamp\(208px,\s*18vh,\s*236px\) minmax\(0,\s*1fr\)/i);
  assert.match(css, /\.health-overview__copy > span \{[^}]*font-size:\s*12px/i);
  assert.match(css, /\.health-device-card__identity b \{[^}]*font-size:\s*12px/i);
  assert.match(css, /\.dashboard-page \.panel-expand-button \{[^}]*width:\s*38px[^}]*height:\s*38px/i);
  assert.match(css, /\.dashboard-clearance-panel \.chart__axis \{[^}]*font-size:\s*10px/i);
  assert.match(css, /\.rtk-metric-grid span \{[^}]*font-size:\s*11px/i);
  assert.match(css, /\.task-section-heading h3 \{[^}]*font-size:\s*12px/i);
  assert.match(css, /\.task-parameter-grid input,[\s\S]*?\.task-parameter-grid select \{[^}]*height:\s*40px[^}]*font-size:\s*12px/i);
  assert.match(css, /\.task-operation-actions \.button \{[^}]*min-height:\s*42px[^}]*font-size:\s*11px/i);
  assert.match(css, /@media \(max-width:\s*1440px\) and \(min-width:\s*1181px\)[\s\S]*?\.dashboard-layout \{ grid-template-columns:\s*1fr/i);
  assert.match(css, /@media \(max-width:\s*1440px\) and \(min-width:\s*1181px\)[\s\S]*?\.main--dashboard \{ overflow-y:\s*auto/i);
});
