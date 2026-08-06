import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import { createTaskBatch, deleteTask, listTasks, TaskApiError } from "../components/workflow/taskApi.ts";

const apiTask = {
  task_id: "5db8d785-6247-4cc5-9376-4a6bb36bca67",
  sequence: 1,
  display_sequence: "01",
  tunnel_code: "T-001",
  tunnel_name: "东山隧道",
  status: "pending",
  created_at: "2026-08-06T12:00:00Z",
  updated_at: "2026-08-06T12:00:00Z",
  started_at: null,
  completed_at: null,
  has_measurements: false,
  recording_path: null,
  schema_version: 2,
  deleted_at: null,
  delete_reason: null,
};

const jsonResponse = (body, status = 200) => new Response(JSON.stringify(body), {
  status,
  headers: { "Content-Type": "application/json" },
});

test("loads persisted tasks from the FastAPI HTTP endpoint", async () => {
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
      sequence: 1,
      tunnelCode: "T-001",
      tunnelName: "东山隧道",
      status: "待执行",
      lane: null,
      createdAt: apiTask.created_at,
      updatedAt: apiTask.updated_at,
      startedAt: null,
      completedAt: null,
      hasMeasurements: false,
      recordingPath: null,
      schemaVersion: 2,
    }]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("creates tasks through FastAPI without any ROS 2 browser interface", async () => {
  const originalFetch = globalThis.fetch;
  let capturedInput;
  let capturedInit;
  globalThis.fetch = async (input, init) => {
    capturedInput = input;
    capturedInit = init;
    return jsonResponse([apiTask], 201);
  };

  try {
    const tasks = await createTaskBatch(
      [{ tunnelCode: "T-001", tunnelName: "东山隧道" }],
      "request-001",
    );
    assert.equal(tasks.length, 1);
    assert.equal(capturedInput, "/api/v1/tasks/batch");
    assert.equal(capturedInit.method, "POST");
    assert.equal(capturedInit.headers["Idempotency-Key"], "request-001");
    assert.deepEqual(JSON.parse(capturedInit.body), {
      tasks: [{ tunnel_code: "T-001", tunnel_name: "东山隧道" }],
    });
    assert.doesNotMatch(String(capturedInput), /ros|rclpy|service|action/i);
    const source = await readFile(new URL("../components/workflow/taskApi.ts", import.meta.url), "utf8");
    assert.match(source, /fetch\(input, init\)/);
    assert.doesNotMatch(source, /rclpy|rclcpp|ros2|create_subscription|create_publisher|ros service|ros action/i);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("surfaces backend task errors and does not synthesize a local task", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => jsonResponse({ detail: "任务数据库不可用" }, 503);

  try {
    await assert.rejects(
      () => createTaskBatch(
        [{ tunnelCode: "T-001", tunnelName: "东山隧道" }],
        "request-002",
      ),
      (error) => error instanceof TaskApiError &&
        error.status === 503 &&
        error.message === "任务数据库不可用",
    );
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
