import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");
const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");
const socket = await readFile(new URL("../components/system-status/useSystemStatusSocket.ts", import.meta.url), "utf8");

test("system overview uses the revised labels and removes diagnostic helper copy", () => {
  assert.match(page, /warn: "系统告警"/);
  assert.match(page, /unknown: "检查中"/);
  assert.doesNotMatch(page, /系统有告警|系统检查中|状态灯依据实时设备诊断判断/);
  assert.doesNotMatch(page, /<p>\{systemStatus\.detail\}<\/p>/);
});

test("system overview contains current clearance and two reserved metric cards", () => {
  assert.match(page, /health-kpi-grid/);
  assert.match(page, />当前净空高度</);
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

test("system status socket clears stale snapshots", () => {
  assert.match(socket, /SNAPSHOT_TIMEOUT_MS = 5000/);
  assert.match(socket, /snapshot:\s*null/);
  assert.match(socket, /超过 5 秒未收到设备诊断数据/);
});
