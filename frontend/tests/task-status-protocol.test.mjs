import assert from "node:assert/strict";
import test from "node:test";

import { parseTaskStatusSnapshot } from "../components/task-status/taskStatusProtocol.ts";

test("parses device-side task status including unconfirmed RTK endpoints", () => {
  const parsed = parseTaskStatusSnapshot({
    type: "task_status_snapshot",
    task_id: "task-001",
    task_sequence: 1,
    status: "running",
    operation_phase: "recording",
    status_revision: 4,
    command_id: "start-001",
    message: "采集已开始，入口RTK坐标未确认",
    error_code: null,
    entry_rtk_status: "unconfirmed",
    exit_rtk_status: "not_requested",
    has_measurements: false,
    recording_path: "task-001/measurements.db",
    started_at_ns: 1,
    completed_at_ns: 0,
    emitted_at_ns: 2,
  });
  assert.equal(parsed?.taskId, "task-001");
  assert.equal(parsed?.entryRtkStatus, "unconfirmed");
  assert.equal(parsed?.operationPhase, "recording");
});

test("rejects non-task status messages", () => {
  assert.equal(parseTaskStatusSnapshot({ type: "status", state: "waiting" }), null);
});
