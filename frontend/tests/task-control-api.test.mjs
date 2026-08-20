import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import {
  getTaskControlReadiness,
  pauseTaskControl,
  recoverTaskControl,
  resumeTaskControl,
  startTaskControl,
  stopTaskControl,
} from "../components/workflow/taskControlApi.ts";

const jsonResponse = (body, status = 200) => new Response(JSON.stringify(body), {
  status,
  headers: { "Content-Type": "application/json" },
});

const accepted = {
  command_id: "command-001",
  accepted: true,
  task_id: "task-001",
  status: "running",
  operation_phase: "recording",
  status_revision: 2,
  message: "采集已开始，入口RTK坐标未确认",
  error_code: null,
};

test("starts a task only through FastAPI HTTP and forwards frozen parameters", async () => {
  const originalFetch = globalThis.fetch;
  let capturedInput;
  let capturedInit;
  globalThis.fetch = async (input, init) => {
    capturedInput = input;
    capturedInit = init;
    return jsonResponse(accepted, 202);
  };
  try {
    const result = await startTaskControl("task-001", {
      lane: "上行右车道",
      lidarMountHeightM: 1.86,
      clearanceThresholdM: 4.5,
      clearanceUpperLimitM: 5.8,
      expectedRevision: 0,
      idempotencyKey: "command-001",
    });
    assert.equal(capturedInput, "/api/v1/tasks/task-001/start");
    assert.equal(capturedInit.method, "POST");
    assert.equal(capturedInit.headers["Idempotency-Key"], "command-001");
    assert.deepEqual(JSON.parse(capturedInit.body), {
      travel_direction: "up",
      lane_side: "right",
      lane: "right",
      lidar_mount_height_m: 1.86,
      clearance_threshold_m: 4.5,
      clearance_upper_limit_m: 5.8,
      expected_revision: 0,
    });
    assert.equal(result.operationPhase, "recording");
    const source = await readFile(new URL("../components/workflow/taskControlApi.ts", import.meta.url), "utf8");
    assert.doesNotMatch(source, /rclpy|rclcpp|create_client|ros2\s|ros service|ros action/i);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("pause resume and stop use revisioned FastAPI commands", async () => {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (input, init) => {
    calls.push([input, JSON.parse(init.body)]);
    return jsonResponse(accepted, 202);
  };
  try {
    await pauseTaskControl("task-001", 2, "pause-1");
    await resumeTaskControl("task-001", 3, "resume-1");
    await stopTaskControl("task-001", 4, "stop-1");
    await recoverTaskControl("task-001", 5, "recover-1");
    assert.deepEqual(calls, [
      ["/api/v1/tasks/task-001/pause", { expected_revision: 2 }],
      ["/api/v1/tasks/task-001/resume", { expected_revision: 3 }],
      ["/api/v1/tasks/task-001/stop", { expected_revision: 4 }],
      ["/api/v1/tasks/task-001/recover", { expected_revision: 5 }],
    ]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("readiness requires both lidar and RTK online before start", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => jsonResponse({
    ready: true,
    state: "ready",
    detail: "任务控制可用，雷达与RTK均已上线",
    bridge_available: true,
    services: { start: true, pause: true, resume: true, stop: true, recover: true },
    missing_services: [],
    active_task_id: null,
    active_phase: null,
    sensor_data_checked: true,
    lidar_online: true,
    rtk_online: true,
    sensor_blockers: [],
    can_start: true,
    can_pause: false,
    can_resume: false,
    can_stop: false,
    can_recover: false,
  });
  try {
    const readiness = await getTaskControlReadiness();
    assert.equal(readiness.ready, true);
    assert.equal(readiness.sensorDataChecked, true);
    assert.equal(readiness.lidarOnline, true);
    assert.equal(readiness.rtkOnline, true);
    assert.deepEqual(readiness.sensorBlockers, []);
    assert.equal(readiness.canStart, true);
    assert.equal(readiness.canStop, false);
    assert.match(readiness.detail, /雷达与RTK均已上线/);
  } finally {
    globalThis.fetch = originalFetch;
  }
});


test("readiness exposes recovery and stop capabilities for a stuck transition", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => jsonResponse({
    ready: false,
    state: "busy",
    detail: "任务 02 正在创建正式测量文件",
    bridge_available: true,
    services: { start: true, pause: true, resume: true, stop: true, recover: true },
    missing_services: [],
    active_task_id: "task-002",
    active_phase: "recorder_preparing",
    sensor_data_checked: true,
    lidar_online: false,
    rtk_online: false,
    sensor_blockers: ["system_status"],
    can_start: false,
    can_pause: false,
    can_resume: false,
    can_stop: true,
    can_recover: true,
  });
  try {
    const readiness = await getTaskControlReadiness();
    assert.equal(readiness.activeTaskId, "task-002");
    assert.equal(readiness.activePhase, "recorder_preparing");
    assert.equal(readiness.canStop, true);
    assert.equal(readiness.canRecover, true);
  } finally {
    globalThis.fetch = originalFetch;
  }
});


test("readiness keeps normal controls available when recover service is missing", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => jsonResponse({
    ready: true,
    state: "degraded",
    detail: "任务控制可用；恢复服务不可用",
    bridge_available: true,
    services: { start: true, pause: true, resume: true, stop: true, recover: false },
    missing_services: ["recover"],
    active_task_id: null,
    active_phase: null,
    sensor_data_checked: true,
    lidar_online: true,
    rtk_online: true,
    sensor_blockers: [],
    can_start: true,
    can_pause: false,
    can_resume: false,
    can_stop: false,
    can_recover: false,
  });
  try {
    const readiness = await getTaskControlReadiness();
    assert.equal(readiness.bridgeAvailable, true);
    assert.equal(readiness.services.start, true);
    assert.equal(readiness.services.recover, false);
    assert.deepEqual(readiness.missingServices, ["recover"]);
    assert.equal(readiness.canStart, true);
    assert.equal(readiness.canRecover, false);
  } finally {
    globalThis.fetch = originalFetch;
  }
});


test("readiness exposes the exact offline sensor blocker", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => jsonResponse({
    ready: false,
    state: "sensor_offline",
    detail: "RTK未上线，等待设备上线后才能开始采集",
    bridge_available: true,
    services: { start: true, pause: true, resume: true, stop: true, recover: true },
    missing_services: [],
    active_task_id: null,
    active_phase: null,
    sensor_data_checked: true,
    lidar_online: true,
    rtk_online: false,
    sensor_blockers: ["rtk"],
    can_start: false,
    can_pause: false,
    can_resume: false,
    can_stop: false,
    can_recover: false,
  });
  try {
    const readiness = await getTaskControlReadiness();
    assert.equal(readiness.canStart, false);
    assert.equal(readiness.lidarOnline, true);
    assert.equal(readiness.rtkOnline, false);
    assert.deepEqual(readiness.sensorBlockers, ["rtk"]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});
