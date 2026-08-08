import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";

const page = fs.readFileSync(new URL("../app/page.tsx", import.meta.url), "utf8");
const workspace = fs.readFileSync(new URL("../components/devtools/DevToolsWorkspace.tsx", import.meta.url), "utf8");
const api = fs.readFileSync(new URL("../components/devtools/devtoolsApi.ts", import.meta.url), "utf8");
const bindings = fs.readFileSync(new URL("../../ros2_ws/src/bringup/config/dev_parameter_bindings.yaml", import.meta.url), "utf8");

test("development navigation is controlled by generated build entry", () => {
  assert.match(page, /DEVTOOLS_ENABLED/);
  assert.match(page, /label: "测试"/);
  assert.match(page, /DevToolsWorkspace/);
});

test("development workspace exposes approved diagnostic sections", () => {
  for (const label of ["概览", "激光雷达", "运动补偿", "RTK", "净空", "任务与记录", "参数"]) {
    assert.ok(workspace.includes(label), `missing ${label}`);
  }
  assert.match(workspace, /\/ws\/dev\/raw-cloud-preview/);
  assert.match(workspace, /保存原始点云/);
  assert.match(workspace, /原始传感器记录/);
  assert.match(workspace, /算法诊断记录/);
  assert.match(workspace, /完整开发记录/);
  assert.match(workspace, /当前不录制视觉和传感器内部温度/);
  assert.match(workspace, /ROS Topic不经浏览器且不降频/);
  assert.match(workspace, /核心参数装订/);
  assert.match(workspace, /参数快照/);
  assert.match(workspace, /记录 5 秒/);
  assert.match(workspace, /记录 10 秒/);
  assert.match(workspace, /记录 30 秒/);
});

test("development frontend only talks to FastAPI development namespace", () => {
  assert.match(api, /\/api\/dev\/overview/);
  assert.match(api, /`\/api\/dev\/recordings\/\$\{profile\}\/start`/);
  assert.match(api, /"raw-sensor" \| "algorithm-debug" \| "full-debug"/);
  assert.doesNotMatch(workspace + api, /rclpy|rclcpp|roslib|rosbridge/i);
});


test("development telemetry fields have explicit render-safe types", () => {
  for (const declaration of [
    "candidate_count?: number",
    "selected_inlier_count?: number",
    "processing_time_ms?: number | null",
    "valid_point_ratio?: number | null",
    "status_revision?: number",
    "total_samples?: number",
  ]) {
    assert.ok(api.includes(declaration), `missing ${declaration}`);
  }
  assert.doesNotMatch(api, /\[key: string\]: unknown/);
});


test("parameter page exposes the requested eight clearance parameters", () => {
  const visible = JSON.parse(bindings).parameters.filter((item) => item.ui_visible).map((item) => item.parameter);
  assert.deepEqual(visible, [
    "ransac.distance_threshold_m",
    "ransac.max_candidate_planes",
    "ransac.min_remaining_points",
    "ransac.min_inliers_absolute",
    "region.grid_size_m",
    "region.min_span_cells",
    "region.min_occupied_cells",
    "region.max_residual_p95_m",
  ]);
  const minRemaining = JSON.parse(bindings).parameters.find((item) => item.parameter === "ransac.min_remaining_points");
  assert.equal(minRemaining.writable, false);
  assert.match(workspace, /正式配置值来自所属YAML/);
  assert.match(workspace, /节点不可用时不以配置值代替运行值/);
  assert.match(workspace, />配置值</);
  assert.match(workspace, />运行值</);
});


test("recording controls share one busy state and one status poll per active panel", () => {
  const source = workspace;
  assert.match(source, /const \[busy, setBusy\] = useState\(false\)/);
  assert.match(source, /setBusy\(true\)/);
  assert.match(source, /window\.setInterval\(\(\) => void refreshStatus\(\), 1000\)/);
  assert.match(source, /const recordingController = useDevRecordingController\(true\)/);
  assert.match(source, /controller=\{recordingController\}/);
  assert.doesNotMatch(source, /setInterval\([^)]*listDevRecordings/);
  assert.match(source, /录制启动不等待参数快照/);
});


test("lidar raw-cloud recording history exposes safe deletion", () => {
  assert.match(workspace, /function LidarPanel[\s\S]*useDevRecordingController\(true\)/);
  assert.match(workspace, /确定删除该开发录制吗？此操作不可恢复。/);
  assert.match(workspace, /const visibleRecords = compact \? ownRecords : ownRecords\.slice\(0, 10\)/);
  assert.match(workspace, /dev-record-list--compact/);
  assert.match(workspace, /disabled=\{record\.active \|\| busy\}/);
  assert.match(api, /DELETE/);
  assert.match(api, /\/api\/dev\/recordings\/\$\{encodeURIComponent\(recordingId\)\}/);
});
