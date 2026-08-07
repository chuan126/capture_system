import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";

const page = fs.readFileSync(new URL("../app/page.tsx", import.meta.url), "utf8");
const workspace = fs.readFileSync(new URL("../components/devtools/DevToolsWorkspace.tsx", import.meta.url), "utf8");
const api = fs.readFileSync(new URL("../components/devtools/devtoolsApi.ts", import.meta.url), "utf8");

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
  assert.match(workspace, /记录 5 秒/);
  assert.match(workspace, /记录 10 秒/);
  assert.match(workspace, /记录 30 秒/);
});

test("development frontend only talks to FastAPI development namespace", () => {
  assert.match(api, /\/api\/dev\/overview/);
  assert.match(api, /`\/api\/dev\/recordings\/\$\{profile\}\/start`/);
  assert.doesNotMatch(workspace + api, /rclpy|rclcpp|roslib|rosbridge/i);
});
