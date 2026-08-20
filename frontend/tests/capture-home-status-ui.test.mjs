import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");
const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");
const socket = await readFile(new URL("../components/system-status/useSystemStatusSocket.ts", import.meta.url), "utf8");

const taskCardStart = page.indexOf('<article className="panel task-operation-panel">');
const taskCardEnd = page.indexOf("</aside>", taskCardStart);
const taskCard = taskCardStart >= 0 && taskCardEnd > taskCardStart
  ? page.slice(taskCardStart, taskCardEnd)
  : "";
const rtkSummary = page.match(/<article className="health-kpi-card health-kpi-card--rtk">([\s\S]*?)<\/article>/)?.[1] ?? "";
const localizationCard = page.match(/<article className="panel localization-panel">([\s\S]*?)<\/article>\r?\n\r?\n          <article className="panel task-operation-panel">/)?.[1] ?? "";
const taskDialog = page.match(/function TaskCreateDialog\([\s\S]*?\nconst navigation:/)?.[0] ?? "";

test("system overview uses the revised labels and removes diagnostic helper copy", () => {
  assert.match(page, /warn: "系统告警"/);
  assert.match(page, /unknown: "检查中"/);
  assert.doesNotMatch(page, /系统有告警|系统检查中|状态灯依据实时设备诊断判断/);
  assert.doesNotMatch(page, /<p>\{systemStatus\.detail\}<\/p>/);
});

test("system overview contains clearance and raw RTK position and removes old summary cards", () => {
  assert.match(page, /health-kpi-grid/);
  assert.match(page, />净空高度</);
  assert.match(page, /\{currentHeightText\}<small>m<\/small>/);
  assert.match(page, /displayedClearanceHeightM = clearanceValid && mountHeightValid/);
  assert.match(page, /clearanceSnapshot\.lidar_to_top_m! \+ parsedMountHeight/);
  assert.match(rtkSummary, />RTK定位</);
  assert.match(rtkSummary, /\{rawLatitudeText\}/);
  assert.match(rtkSummary, /\{rawLongitudeText\}/);
  assert.match(rtkSummary, /\{rawAltitudeText\}/);
  assert.match(rtkSummary, />卫导星数\s*</);
  assert.match(rtkSummary, />HDOP \/ PDOP\s*</);
  assert.doesNotMatch(page, />当前坐标|>异常高度</);
  assert.doesNotMatch(page, />预留指标二|>预留指标一</);
  assert.match(page, /displayedClearanceHeightM < parsedHeightThreshold/);
  assert.match(css, /health-kpi-card--alert[\s\S]*#c53030/);
  assert.match(css, /health-kpi-grid[\s\S]*grid-template-columns:\s*minmax\(190px, \.9fr\)/);
  assert.match(css, /health-kpi-card--rtk[\s\S]*grid-column:\s*auto/);
  assert.match(page, /formatMetric\(rtkSnapshot\?\.altitude, 2\)/);
});

test("top clearance summary adds mount height while live source protocol remains raw", () => {
  assert.match(page, /displayedClearanceHeightM = clearanceValid && mountHeightValid/);
  assert.match(page, /clearanceSnapshot\.lidar_to_top_m! \+ parsedMountHeight/);
  assert.match(page, /const heightM = snapshot\.valid && snapshot\.lidar_to_top_m !== null[\s\S]*?\? snapshot\.lidar_to_top_m[\s\S]*?: null/);
});

test("current coordinate requires a valid streaming RTK fix", () => {
  assert.match(page, /rtk\.connection === "connected"/);
  assert.match(page, /rtk\.streamState === "streaming"/);
  assert.match(page, /rmcCharacter !== "V"/);
  assert.match(page, /latitudeValue\.toFixed\(7\)/);
  assert.match(page, /longitudeValue\.toFixed\(7\)/);
});

test("capture sidebar is split into fusion localization and task control cards", () => {
  assert.match(page, /dashboard-side-stack/);
  assert.match(page, /className="panel localization-panel"/);
  assert.doesNotMatch(page, /className="panel rtk-control-panel"/);
  assert.match(page, /className="panel task-operation-panel"/);
  assert.match(css, /dashboard-side-stack[\s\S]*grid-template-rows:\s*auto minmax\(0, 1fr\)/);
  assert.match(css, /localization-panel \{ flex:\s*0 0 auto/);
});

test("fusion card shows derived LLH and vehicle attitude only", () => {
  assert.match(localizationCard, /fusion-position-grid/);
  assert.match(localizationCard, /\{localizationLatitudeText\}/);
  assert.match(localizationCard, /\{localizationLongitudeText\}/);
  assert.match(localizationCard, /fusion-attitude-grid/);
  assert.match(localizationCard, />俯仰</);
  assert.match(localizationCard, />横滚</);
  assert.match(localizationCard, />方位</);
  assert.match(localizationCard, /\{vehiclePitchText\}/);
  assert.match(localizationCard, /\{vehicleRollText\}/);
  assert.match(localizationCard, /\{vehicleHeadingText\}/);
  assert.match(page, /rawCoordinateAvailable[\s\S]*rawTrackDeg[\s\S]*localizationHeadingDeg/);
  assert.match(page, /localization_heading_deg/);
  assert.match(page, /displayedVehicleHeadingDeg = localizationHeadingValid/);
  assert.doesNotMatch(page, /formatMetric\(rtkSnapshot\?\.localization_vehicle_heading_deg/);
  assert.doesNotMatch(localizationCard, /车辆航向|模式 \/ 航向源|DR时间 \/ 锚点距|水平尺度 \/ 状态|航向偏差|恢复误差/);
  assert.match(css, /fusion-attitude-grid[\s\S]*grid-template-columns:\s*repeat\(3/);
});

test("live clearance chart exposes independent vertical zoom controls", () => {
  assert.match(page, /实时曲线纵向缩放/);
  assert.match(page, /adjustVerticalZoom/);
  assert.match(page, /纵向放大实时曲线/);
  assert.match(page, /纵向缩小实时曲线/);
  assert.match(css, /live-clearance-chart__tools/);
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

test("task card shows a two-by-two parameter grid with the inclusive height range", () => {
  assert.match(taskCard, /task-parameter-strip/);
  assert.match(taskCard, />高度下限阈值</);
  assert.match(taskCard, /value=\{heightThreshold\}/);
  assert.match(taskCard, />高度上限阈值</);
  assert.match(taskCard, /value=\{heightUpperLimit\}/);
  assert.match(taskCard, /正常区间：高度下限阈值 ≤ 净空高度 ≤ 高度上限阈值/);
  assert.match(taskCard, />雷达安装高度</);
  assert.match(taskCard, /value=\{mountHeight\}/);
  assert.match(taskCard, />作业车道</);
  assert.match(taskCard, /value=\{operationLane\}/);
  for (const lane of ["上行左车道", "上行右车道", "下行左车道", "下行右车道"]) {
    assert.match(taskCard, new RegExp(`option value="${lane}"`));
  }
  assert.match(taskCard, /当前任务参数/);
  assert.match(taskCard, /disabled=\{taskLocked\}/);
  assert.match(taskCard, /taskLocked \? \(currentTask\.lane \?\? operationLane\) : operationLane/);
  assert.match(page, /useState\("0\.00"\)/);
  assert.match(page, /useState\("20\.00"\)/);
  assert.match(css, /\.task-parameter-grid\s*\{[^}]*grid-template-columns:\s*repeat\(2/);
  assert.match(taskCard, /min="0"/);
  assert.match(taskCard, /max="20"/);
});

test("task creation stores independent planned lane and height range", () => {
  assert.match(taskDialog, />创建检测任务</);
  assert.match(taskDialog, /任务编号由设备端按创建时间生成/);
  assert.match(taskDialog, /20260807_145601/);
  assert.match(taskDialog, />隧道编号</);
  assert.match(taskDialog, />隧道名称</);
  assert.match(taskDialog, />作业车道</);
  assert.match(taskDialog, />高度下限阈值</);
  assert.match(taskDialog, />高度上限阈值</);
  for (const lane of ["上行左车道", "上行右车道", "下行左车道", "下行右车道"]) {
    assert.match(taskDialog, new RegExp(`option value="${lane}"`));
  }
  assert.match(taskDialog, /"保存并关闭"/);
  assert.match(taskDialog, /"保存并继续创建"/);
  assert.match(taskDialog, /await onCreate\(\{ tunnelCode, tunnelName, lane: draft\.lane, clearanceThreshold:[\s\S]*clearanceUpperLimit:/);
  assert.match(taskDialog, /setDraft\(createTaskDraft\(\)\)/);
  assert.doesNotMatch(taskDialog, />＋ 添加任务|>复制<\/button>|>删除<\/button>|rows\.map/);
  assert.doesNotMatch(taskDialog, />雷达安装高度</);
  assert.doesNotMatch(taskDialog, />任务名称|row\.taskName|batchMode|作业批次/);
  assert.match(page, /const created = await createTask\(\{/);
  assert.match(page, /clearanceThresholdM: Number\(draft\.clearanceThreshold\)/);
  assert.match(page, /clearanceUpperLimitM: Number\(draft\.clearanceUpperLimit\)/);
  assert.doesNotMatch(page, /createTaskBatch\(/);
  assert.match(page, /task\.displayId/);
  assert.doesNotMatch(page, /createTaskId|nextTaskSequence|formatTaskSequence/);
});


test("dashboard defaults task execution to the earliest pending task and preserves manual pending selection", () => {
  assert.match(page, /const firstPendingTask = tasks\.find\(\(task\) => task\.status === "待执行"\) \?\? null/);
  assert.match(page, /currentTask = activeTask \?\? selectedPendingTask \?\? firstPendingTask \?\? selectedTask \?\? null/);
  assert.match(page, /selectedExecutableTask = tasks\.find/);
  assert.match(page, /preferredTaskId = selectedExecutableTask\?\.taskId \?\? firstPendingTask\?\.taskId \?\? null/);
  assert.match(page, /setSelectedTaskId\(preferredTaskId \?\? created\.taskId\)/);
  assert.match(page, /persistedTasks\.find\(\(task\) => task\.status === "待执行"\)\?\.taskId/);
  assert.match(page, /pendingTasks = tasks[\s\S]*?sort\(\(left, right\) => left\.createdAt\.localeCompare\(right\.createdAt\)\)/);
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
  assert.match(page, /parsedMountHeight >= 0 && parsedMountHeight <= 20/);
  assert.match(page, /parsedHeightThreshold >= 0 && parsedHeightThreshold <= 20/);
  assert.match(page, /parsedHeightUpperLimit >= 0 && parsedHeightUpperLimit <= 20/);
  assert.match(page, /parsedHeightThreshold <= parsedHeightUpperLimit/);
  assert.match(page, /!heightRangeValid \|\|[\s\S]*!mountHeightValid/);
  assert.match(page, /请输入有效的雷达安装高度/);
  assert.match(page, /startTaskControl\(currentTask\.taskId/);
  assert.match(page, /lidarMountHeightM: parsedMountHeight/);
  assert.match(page, /clearanceThresholdM: parsedHeightThreshold/);
  assert.match(page, /clearanceUpperLimitM: parsedHeightUpperLimit/);
  assert.doesNotMatch(page, /status: "采集中", lane: operationLane/);
  assert.doesNotMatch(page, /!captureReady/);
});

test("current clearance turns red outside the inclusive configured height range", () => {
  assert.match(page, /const clearanceAbnormalReason = displayedClearanceHeightM === null/);
  assert.match(page, /displayedClearanceHeightM < parsedHeightThreshold/);
  assert.match(page, /displayedClearanceHeightM > parsedHeightUpperLimit/);
  assert.match(page, /低于下限阈值/);
  assert.match(page, /超过上限阈值/);
  assert.match(page, /health-kpi-card--primary\$\{clearanceAbnormal \? " health-kpi-card--alert"/);
  assert.doesNotMatch(page, /latestAbnormalHeightM|health-kpi-card--anomaly/);
  assert.match(css, /\.health-kpi-grid[\s\S]*grid-template-columns:\s*repeat\(3/);
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
  assert.match(css, /\.fusion-position-grid span,[\s\S]*?font-size:\s*11px/i);
  assert.match(css, /\.task-section-heading h3 \{[^}]*font-size:\s*12px/i);
  assert.match(css, /\.task-parameter-grid input,[\s\S]*?\.task-parameter-grid select \{[^}]*height:\s*40px[^}]*font-size:\s*12px/i);
  assert.match(css, /@media \(max-width:\s*1600px\) and \(min-width:\s*761px\)[\s\S]*?\.task-parameter-grid \{[^}]*gap:\s*10px/i);
  assert.match(css, /\.task-operation-actions \.button \{[^}]*min-height:\s*42px[^}]*font-size:\s*11px/i);
  assert.match(css, /@media \(max-width:\s*1440px\) and \(min-width:\s*1181px\)[\s\S]*?\.dashboard-layout \{ grid-template-columns:\s*1fr/i);
  assert.match(css, /@media \(max-width:\s*1440px\) and \(min-width:\s*1181px\)[\s\S]*?\.main--dashboard \{ overflow-y:\s*auto/i);
});

test("task start follows backend readiness instead of diagnostic card states", () => {
  assert.match(page, /setControlAvailable\(readiness\.canStart\)/);
  assert.doesNotMatch(page, /setControlAvailable\(readiness\.state\s*!==/);
  assert.doesNotMatch(page, /captureReady|isDeviceConnected\(systemStatus/);
});


test("start and stop execute without confirmation dialogs", () => {
  assert.doesNotMatch(page, /window\.confirm|confirm\(/);
  assert.match(page, /executeControl\("start"/);
  assert.match(page, /executeControl\("stop"/);
});

test("task creation no longer exposes operation-batch choices", () => {
  assert.match(taskDialog, /创建检测任务/);
  assert.match(taskDialog, /按创建时间生成/);
  assert.doesNotMatch(taskDialog, /提交后自动新建作业|加入当前作业|开始一次新作业|batchMode/);
  assert.doesNotMatch(taskCard, />新建作业<|>结束作业|作业批次/);
});

test("completed stop automatically selects the next pending task by creation time", () => {
  assert.match(page, /autoAdvanceTaskId/);
  assert.match(page, /task\.status === "待执行"/);
  assert.match(page, /left\.createdAt\.localeCompare\(right\.createdAt\)/);
  assert.match(page, /task\.createdAt > stopped\.createdAt/);
  assert.match(page, /setSelectedTaskId\(next\?\.taskId/);
  assert.doesNotMatch(page, /task\.batchId === stopped\.batchId/);
});

test("active transitional tasks retain stop and recovery controls", () => {
  assert.match(page, /setCanStop\(readiness\.canStop\)/);
  assert.match(page, /setCanRecover\(readiness\.canRecover\)/);
  assert.match(page, /recoverTaskControl/);
  assert.match(taskCard, /恢复控制/);
});
