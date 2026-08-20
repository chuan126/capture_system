import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import { createTask, createTaskBatch, deleteSelectedTasks, deleteTask, listTasks, TaskApiError } from "../components/workflow/taskApi.ts";

const apiTask = {
  task_id: "5db8d785-6247-4cc5-9376-4a6bb36bca67",
  display_id: "20260807_145601",
  batch_id: "legacy-batch-1",
  batch_code: "20260807-01",
  sequence: 1,
  global_sequence: 51,
  display_sequence: "20260807_145601",
  tunnel_code: "T-001",
  tunnel_name: "东山隧道",
  status: "pending",
  operation_phase: "idle",
  status_revision: 0,
  created_at: "2026-08-07T06:56:01Z",
  updated_at: "2026-08-07T06:56:01Z",
  start_requested_at: null,
  started_at: null,
  stop_requested_at: null,
  completed_at: null,
  entry_rtk_status: "not_requested",
  exit_rtk_status: "not_requested",
  has_measurements: false,
  recording_path: null,
  local_data_purged_at: null,
  purged_bytes: 0,
  last_error_code: null,
  last_error_message: null,
  warning_code: null,
  schema_version: 6,
  deleted_at: null,
  delete_reason: null,
};

const jsonResponse = (body, status = 200) => new Response(JSON.stringify(body), {
  status,
  headers: { "Content-Type": "application/json" },
});

test("loads persisted time-numbered tasks from the FastAPI HTTP endpoint", async () => {
  const originalFetch = globalThis.fetch;
  let capturedInput;
  let capturedInit;
  globalThis.fetch = async (input, init) => {
    capturedInput = input;
    capturedInit = init;
    return jsonResponse([apiTask]);
  };

  try {
    const tasks = await listTasks();
    assert.equal(capturedInput, "/api/v1/tasks?limit=500&offset=0&order=asc");
    assert.equal(capturedInit.method, "GET");
    assert.deepEqual(tasks, [{
      taskId: apiTask.task_id,
      displayId: apiTask.display_id,
      tunnelCode: "T-001",
      tunnelName: "东山隧道",
      status: "待执行",
      operationPhase: "idle",
      statusRevision: 0,
      lane: null,
      lidarMountHeightM: null,
      clearanceThresholdM: null,
      clearanceUpperLimitM: 20,
      createdAt: apiTask.created_at,
      updatedAt: apiTask.updated_at,
      startRequestedAt: null,
      startedAt: null,
      stopRequestedAt: null,
      completedAt: null,
      entryRtkStatus: "not_requested",
      exitRtkStatus: "not_requested",
      hasMeasurements: false,
      recordingPath: null,
      localDataPurgedAt: null,
      purgedBytes: 0,
      lastErrorCode: null,
      lastErrorMessage: null,
      warningCode: null,
      schemaVersion: 6,
    }]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("creates one task per formal frontend request without any ROS 2 browser interface", async () => {
  const originalFetch = globalThis.fetch;
  let capturedInput;
  let capturedInit;
  globalThis.fetch = async (input, init) => {
    capturedInput = input;
    capturedInit = init;
    return jsonResponse(apiTask, 201);
  };

  try {
    const task = await createTask(
      { tunnelCode: "T-001", tunnelName: "东山隧道", lane: "上行左车道", clearanceThresholdM: 4.5, clearanceUpperLimitM: 5.8 },
      "request-001",
    );
    assert.equal(task.displayId, "20260807_145601");
    assert.equal(capturedInput, "/api/v1/tasks");
    assert.equal(capturedInit.method, "POST");
    assert.equal(capturedInit.headers["Idempotency-Key"], "request-001");
    assert.deepEqual(JSON.parse(capturedInit.body), {
      tunnel_code: "T-001",
      tunnel_name: "东山隧道",
      travel_direction: "up",
      lane_side: "left",
      clearance_threshold_m: 4.5,
      clearance_upper_limit_m: 5.8,
    });
    const source = await readFile(new URL("../components/workflow/taskApi.ts", import.meta.url), "utf8");
    assert.doesNotMatch(source, /batch_mode|rclpy|rclcpp|ros2|create_subscription|create_publisher|ros service|ros action/i);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("keeps batch creation only as a compatibility API", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => jsonResponse([apiTask], 201);
  try {
    const tasks = await createTaskBatch([{ tunnelCode: "T-001", tunnelName: "东山隧道", lane: "下行右车道", clearanceThresholdM: 4.2, clearanceUpperLimitM: 5.8 }], "compat-001");
    assert.equal(tasks.length, 1);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("deletes selected tasks through one FastAPI transaction endpoint", async () => {
  const originalFetch = globalThis.fetch;
  let capturedInput;
  let capturedInit;
  globalThis.fetch = async (input, init) => {
    capturedInput = input;
    capturedInit = init;
    return jsonResponse({ deleted_task_count: 1, task_ids: [apiTask.task_id] });
  };
  try {
    const result = await deleteSelectedTasks([apiTask.task_id]);
    assert.equal(capturedInput, "/api/v1/tasks/delete-selected");
    assert.equal(capturedInit.method, "POST");
    assert.deepEqual(JSON.parse(capturedInit.body), { task_ids: [apiTask.task_id] });
    assert.equal(result.deletedTaskCount, 1);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("surfaces backend task errors and does not synthesize a local task", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => jsonResponse({ detail: "任务数据库不可用" }, 503);
  try {
    await assert.rejects(
      () => createTask({ tunnelCode: "T-001", tunnelName: "东山隧道", lane: "下行左车道", clearanceThresholdM: 4.1, clearanceUpperLimitM: 5.8 }, "request-002"),
      (error) => error instanceof TaskApiError && error.status === 503 && error.message === "任务数据库不可用",
    );
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("loads planned lane and threshold before start and prefers frozen actual values after start", async () => {
  const originalFetch = globalThis.fetch;
  let call = 0;
  globalThis.fetch = async () => {
    call += 1;
    if (call === 1) return jsonResponse([{
      ...apiTask,
      planned_travel_direction: "down",
      planned_lane_side: "left",
      planned_clearance_threshold_m: 4.6,
      planned_clearance_upper_limit_m: 5.8,
      travel_direction: null,
      lane_side: null,
      lane: null,
      clearance_threshold_m: null,
      clearance_upper_limit_m: null,
    }]);
    return jsonResponse([{
      ...apiTask,
      status: "running",
      operation_phase: "recording",
      planned_travel_direction: "down",
      planned_lane_side: "left",
      planned_clearance_threshold_m: 4.6,
      planned_clearance_upper_limit_m: 5.8,
      travel_direction: "up",
      lane_side: "right",
      lane: "right",
      clearance_threshold_m: 4.3,
      clearance_upper_limit_m: 5.6,
    }]);
  };
  try {
    const [pending] = await listTasks();
    assert.equal(pending.lane, "下行左车道");
    assert.equal(pending.clearanceThresholdM, 4.6);
    assert.equal(pending.clearanceUpperLimitM, 5.8);
    const [running] = await listTasks();
    assert.equal(running.lane, "上行右车道");
    assert.equal(running.clearanceThresholdM, 4.3);
    assert.equal(running.clearanceUpperLimitM, 5.6);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("deletes tasks through the FastAPI HTTP endpoint", async () => {
  const originalFetch = globalThis.fetch;
  let capturedInput;
  let capturedInit;
  globalThis.fetch = async (input, init) => {
    capturedInput = input;
    capturedInit = init;
    return new Response(null, { status: 204 });
  };
  try {
    await deleteTask(apiTask.task_id);
    assert.equal(capturedInput, `/api/v1/tasks/${apiTask.task_id}`);
    assert.equal(capturedInit.method, "DELETE");
    assert.doesNotMatch(String(capturedInput), /ros|rclpy|service|action/i);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("parses frozen zero-valued task parameters for active-task restoration", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => jsonResponse([{
    ...apiTask,
    lane: "right",
    lidar_mount_height_m: 0,
    clearance_threshold_m: 0,
    clearance_upper_limit_m: 0,
  }]);
  try {
    const [task] = await listTasks();
    assert.equal(task.lane, "右车道");
    assert.equal(task.lidarMountHeightM, 0);
    assert.equal(task.clearanceThresholdM, 0);
    assert.equal(task.clearanceUpperLimitM, 0);
  } finally {
    globalThis.fetch = originalFetch;
  }
});
