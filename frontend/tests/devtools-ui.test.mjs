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
  for (const label of ["RTK与融合定位", "净空算法", "保存原始点云", "核心配置"]) {
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

test("RTK and capture home share localization state labels and validity logic", () => {
  assert.match(localization, /RTK固定/);
  assert.match(localization, /航位推算/);
  assert.match(localization, /RTK恢复/);
  assert.match(localization, /deriveLocalizationStatus/);
  assert.match(page, /deriveLocalizationStatus/);
  assert.match(page, /rtkSolutionLabel/);
  assert.match(workspace, /deriveLocalizationStatus/);
  assert.match(workspace, /rtkSolutionLabel/);
});

test("raw cloud samples expose only save stop delete and full-chain offline replay", () => {
  assert.match(workspace, /\/capture\/lidar\/points_raw/);
  assert.match(workspace, /原始高频里程计/);
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
  assert.match(workspace, /\/capture\/dev\/offline\/\*/);
  assert.match(workspace, /offlineActive \|\| recordingActive \|\| !selected\?\.replay_ready/);
});

test("dashboard exposes only nine core parameters", () => {
  const visible = JSON.parse(bindings).parameters.filter((item) => item.ui_visible).map((item) => item.key);
  assert.deepEqual(visible, [
    "motion.processing_poll_interval_ms",
    "motion.max_interpolation_gap_s",
    "motion.minimum_valid_pose_ratio",
    "clearance.distance_threshold_m",
    "clearance.max_candidate_planes",
    "clearance.min_inliers_absolute",
    "clearance.region_grid_size_m",
    "clearance.min_region_occupied_cells",
    "clearance.max_residual_p95_m",
  ]);
  assert.match(workspace, /运行值不一致/);
  assert.match(workspace, /正式配置值/);
  assert.match(workspace, /当前运行值/);
  assert.match(workspace, /运行时动态修改/);
  assert.match(workspace, /设置当前运行值/);
  assert.doesNotMatch(workspace, /临时修改|可临时修改/);
});

test("clearance card is fixed-height and shows real RANSAC plane count", () => {
  for (const field of ["ransac_plane_count", "surface_count", "selected_inlier_count", "processing_time_ms", "selected_area_m2", "selected_tilt_deg"]) {
    assert.ok(workspace.includes(field), `missing ${field}`);
  }
  assert.match(workspace, /label="曲面数量"/);
  assert.doesNotMatch(workspace, /snapshot\?\.candidate_count|<Metric label="残差 P95"|候选平面|合格候选区域/);
  assert.match(workspace, /RANSAC平面/);
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

test("offline card preserves last valid clearance and exposes ENU diagnostics", () => {
  assert.match(workspace, /latest_result_valid === false/);
  assert.match(workspace, /最后有效值/);
  for (const key of ["clouds_received_total", "clouds_processed_total", "clouds_dropped_total", "interpolation_failure_count", "pending_cloud_count"]) {
    assert.ok(workspace.includes(key), `missing ${key}`);
  }
  assert.match(api, /latest_result_valid: boolean \| null/);
  assert.match(api, /diagnostics: Record<string, number \| string \| null>/);
});

test("development APIs stay under FastAPI development namespace", () => {
  assert.match(api, /\/api\/dev\/overview/);
  assert.match(api, /`\/api\/dev\/recordings\/\$\{profile\}\/start`/);
  assert.match(api, /\/api\/dev\/offline\/status/);
  assert.match(api, /\/api\/dev\/offline\/start/);
  assert.match(api, /\/api\/dev\/offline\/stop/);
  assert.doesNotMatch(workspace + api, /rclpy|rclcpp|roslib|rosbridge/i);
});
