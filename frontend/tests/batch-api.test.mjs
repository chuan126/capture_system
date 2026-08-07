import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import {
  archiveBatch,
  completeBatch,
  createBatch,
  listBatches,
  purgeBatch,
} from "../components/workflow/batchApi.ts";

const batchPayload = {
  batch_id: "batch-1",
  batch_code: "20260807-01",
  operation_date: "2026-08-07",
  daily_sequence: 1,
  status: "active",
  created_at: "2026-08-07T01:00:00Z",
  started_at: "2026-08-07T01:00:00Z",
  completed_at: null,
  archived_at: null,
  purged_at: null,
  task_count: 2,
  visible_task_count: 2,
  measurement_bytes: 4096,
  report_id: null,
  report_path: null,
  report_sha256: null,
  report_generated_at: null,
  purged_bytes: 0,
};

const jsonResponse = (body, status = 200) => new Response(JSON.stringify(body), {
  status,
  headers: { "Content-Type": "application/json" },
});

test("loads and creates operation batches through FastAPI", async () => {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (input, init) => {
    calls.push([input, init?.method]);
    return String(input) === "/api/v1/batches" && init?.method !== "POST"
      ? jsonResponse([batchPayload])
      : jsonResponse(batchPayload, 201);
  };
  try {
    const batches = await listBatches();
    const created = await createBatch();
    assert.equal(batches[0].batchCode, "20260807-01");
    assert.equal(batches[0].measurementBytes, 4096);
    assert.equal(created.status, "active");
    assert.deepEqual(calls, [
      ["/api/v1/batches", undefined],
      ["/api/v1/batches", "POST"],
    ]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("completes archives and purges only through batch HTTP endpoints", async () => {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (input, init) => {
    calls.push([input, init?.method]);
    if (String(input).endsWith("/purge")) {
      return jsonResponse({
        batch: { ...batchPayload, status: "purged", purged_at: "2026-08-10T01:00:00Z", purged_bytes: 4096 },
        released_bytes: 4096,
        removed_task_count: 2,
      });
    }
    const status = String(input).endsWith("/archive") ? "archived" : "completed";
    return jsonResponse({ ...batchPayload, status });
  };
  try {
    await completeBatch("batch-1");
    await archiveBatch("batch-1");
    const purged = await purgeBatch("batch-1");
    assert.equal(purged.releasedBytes, 4096);
    assert.equal(purged.removedTaskCount, 2);
    assert.deepEqual(calls, [
      ["/api/v1/batches/batch-1/complete", "POST"],
      ["/api/v1/batches/batch-1/archive", "POST"],
      ["/api/v1/batches/batch-1/purge", "POST"],
    ]);
    const source = await readFile(new URL("../components/workflow/batchApi.ts", import.meta.url), "utf8");
    assert.doesNotMatch(source, /rclpy|rclcpp|ros2|create_subscription|create_publisher/i);
  } finally {
    globalThis.fetch = originalFetch;
  }
});
