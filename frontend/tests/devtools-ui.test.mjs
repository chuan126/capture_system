import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";

const page = fs.readFileSync(new URL("../app/page.tsx", import.meta.url), "utf8");
const workspace = fs.readFileSync(new URL("../components/devtools/DevToolsWorkspace.tsx", import.meta.url), "utf8");
const api = fs.readFileSync(new URL("../components/devtools/devtoolsApi.ts", import.meta.url), "utf8");
const localization = fs.readFileSync(new URL("../components/rtk/localizationView.ts", import.meta.url), "utf8");
const bindings = fs.readFileSync(new URL("../../ros2_ws/src/bringup/config/dev_parameter_bindings.yaml", import.meta.url), "utf8");
const styles = fs.readFileSync(new URL("../app/globals.css", import.meta.url), "utf8");

test("development navigation is controlled by generated build entry", () => {
  assert.match(page, /DEVTOOLS_ENABLED/);
  assert.match(page, /label: "测试"/);
  assert.match(page, /DevToolsWorkspace/);
});

test("development workspace is a single-page four-section dashboard", () => {
  for (const label of ["RTK定位", "净空算法", "保存完整测试数据", "核心配置"]) {
    assert.ok(workspace.includes(label), `missing ${label}`);
  }
  for (const obsolete of ["概览", "激光雷达数据状态", "运动补偿数据流", "任务控制链路", "原始传感器记录", "算法诊断记录", "完整开发记录"]) {
    assert.doesNotMatch(workspace, new RegExp(obsolete));
  }
  assert.doesNotMatch(workspace, /dev-tabs|role="tab"|DevTab/);
  assert.match(workspace, /dev-dashboard-grid/);
});

test("single-page dashboard does not renew the high-rate development overview lease", () => {
  assert.doesNotMatch(workspace, /getDevOverview|\/api\/dev\/overview/);
  assert.equal((workspace.match(/useRtkSocket\(\)/g) ?? []).length, 1);
  assert.equal((workspace.match(/useClearanceSocket\(\)/g) ?? []).length, 1);
  assert.doesNotMatch(workspace, /\/ws\/dev\/raw-cloud-preview|PointCloudViewer/);
});

test("RTK panels use raw receiver state without fusion fallback", () => {
  assert.match(localization, /RTK固定/);
  assert.match(page, /rtkSolutionLabel/);
  assert.match(workspace, /rtkSolutionLabel/);
  assert.match(workspace, /不使用里程计融合回退/);
  assert.match(workspace, /snapshot\?\.latitude/);
  assert.match(workspace, /snapshot\.longitude/);
  assert.doesNotMatch(page, /deriveLocalizationStatus|localization_valid|localization_mode/);
  assert.doesNotMatch(workspace, /deriveLocalizationStatus|localization_valid|localization_mode|航位推算|RTK恢复/);
});

test("complete test samples expose only save stop delete and full-chain offline replay", () => {
  assert.match(workspace, /原始点云、400 Hz雷达状态、RTK、算法结果和诊断按频率\/职责拆分为独立MCAP/);
  assert.match(workspace, /并保留消息时间戳/);
  assert.match(workspace, /startDevRecording\("raw-cloud", null\)/);
  for (const label of [">保存<", ">停止<", ">删除<", "离线算法调试", "开始检测", "停止检测"]) {
    assert.ok(workspace.includes(label), `missing ${label}`);
  }
  for (const obsolete of ["记录 5 秒", "记录 10 秒", "记录 30 秒", "连续记录"]) {
    assert.doesNotMatch(workspace, new RegExp(obsolete));
  }
  assert.match(workspace, /deleteDevRecording/);
  assert.match(workspace, /getDevOfflineReplayStatus/);
  assert.match(workspace, /startDevOfflineReplay/);
  assert.match(workspace, /stopDevOfflineReplay/);
  assert.match(workspace, /正在启动完整测试数据保存/);
  assert.match(workspace, /role="status"/);
  assert.match(workspace, /role="alert"/);
  assert.match(workspace, /actionError \?\? pollError \?\? status\?\.last_error/);
  assert.match(workspace, /\/capture\/dev\/offline\/\*/);
  assert.match(workspace, /offlineActive \|\| recordingActive \|\| !selected\?\.replay_ready/);
  assert.doesNotMatch(workspace, /records\.slice\(0,\s*8\)/);
});

test("dashboard exposes the raw-cluster core parameters", () => {
  const visible = JSON.parse(bindings).parameters.filter((item) => item.ui_visible).map((item) => item.key);
  assert.deepEqual(visible, [
    "clearance.min_detection_x_m",
    "clearance.detection_radius_m",
    "clearance.support_height_band_m",
    "clearance.min_support_points",
    "clearance.spatial_grid_size_m",
    "clearance.min_occupied_cells",
    "clearance.min_spatial_span_m",
  ]);
  assert.doesNotMatch(workspace, /运动补偿/);
  assert.match(workspace, /运行值不一致/);
  assert.match(workspace, /正式配置值/);
  assert.match(workspace, /当前运行值/);
  assert.match(workspace, /运行时动态修改/);
  assert.match(workspace, /设置当前运行值/);
  assert.doesNotMatch(workspace, /临时修改|可临时修改/);
});

test("clearance card is fixed-height and shows trusted raw cluster metrics", () => {
  for (const field of ["candidate_count", "selected_inlier_count", "processing_time_ms"]) {
    assert.ok(workspace.includes(field), `missing ${field}`);
  }
  assert.match(workspace, /label="可信点簇"/);
  assert.match(workspace, /label="最低簇点数"/);
  assert.match(workspace, /label="有效点比例"/);
  assert.doesNotMatch(workspace, /<Metric label="曲面数量"|残差 P95|候选平面|合格候选区域|RANSAC平面|平面倾角/);
  assert.match(workspace, /dev-clearance-reason/);
  assert.match(workspace, /不叠加任务安装高度/);
  assert.match(styles, /\.dev-clearance-primary \{ height: 92px;/);
  assert.match(styles, /\.dev-clearance-reason \{ height: 14px;/);
});

test("dashboard places offline replay under core config and keeps both columns height-aligned", () => {
  assert.match(workspace, /dev-dashboard-left[\s\S]*<ConfigPanel \/>[\s\S]*<OfflineReplayPanel controller=\{recording\} offline=\{offline\} selected=\{selectedRecording\} \/>/);
  assert.match(workspace, /dev-dashboard-right[\s\S]*<ClearancePanel clearance=\{clearance\} \/>[\s\S]*<PositionPanel rtk=\{rtk\} \/>[\s\S]*<RawCloudPanel controller=\{recording\} offline=\{offline\}/);
  assert.match(styles, /\.dev-dashboard-grid \{[^}]*align-items: stretch;/);
  assert.match(styles, /\.dev-dashboard-left, \.dev-dashboard-right \{[^}]*flex-direction: column;[^}]*gap: 12px;/);
  assert.match(styles, /\.dev-dashboard-left > \.dev-dashboard-card:last-child, \.dev-dashboard-right > \.dev-dashboard-card:last-child \{[^}]*flex: 1 1 auto;/);
});

test("offline card preserves last valid clearance and exposes cluster statistics", () => {
  assert.match(workspace, /latest_result_valid === false/);
  assert.match(workspace, /最后有效值/);
  for (const removedMotionMetric of ["clouds_received_total", "clouds_processed_total", "clouds_dropped_total", "interpolation_failure_count", "pending_cloud_count"]) {
    assert.ok(!workspace.includes(removedMotionMetric), `unexpected raw-only motion metric ${removedMotionMetric}`);
  }
  assert.match(api, /latest_result_valid: boolean \| null/);
  assert.match(api, /diagnostics: Record<string, number \| string \| null>/);
  for (const key of ["cluster_point_count_last", "cluster_point_count_mean", "cluster_point_count_max"]) {
    assert.ok(workspace.includes(key), `missing ${key}`);
    assert.ok(api.includes(key), `missing api ${key}`);
  }
  assert.doesNotMatch(workspace + api, /ransac_plane_(last|mean|max)/);
});

test("development APIs stay under FastAPI development namespace", () => {
  assert.match(api, /\/api\/dev\/overview/);
  assert.match(api, /`\/api\/dev\/recordings\/\$\{profile\}\/start`/);
  assert.match(api, /\/api\/dev\/offline\/status/);
  assert.match(api, /\/api\/dev\/offline\/start/);
  assert.match(api, /\/api\/dev\/offline\/stop/);
  assert.doesNotMatch(workspace + api, /rclpy|rclcpp|roslib|rosbridge/i);
});
