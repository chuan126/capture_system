import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import { loadMeasurementHistory } from "../components/playback/measurementHistoryApi.ts";

const payload = {
  task_id: "00000000-0000-4000-8000-000000000002",
  recording_schema_version: 1,
  data_origin: "test_fixture",
  lane: "left",
  started_at: "2026-08-06T01:00:00Z",
  ended_at: "2026-08-06T01:00:00.080Z",
  complete: true,
  algorithm_version: "test-algorithm",
  config_version: "test-config",
  software_version: "0.2.0-test",
  statistics: {
    total_samples: 5,
    valid_samples: 4,
    invalid_samples: 1,
    minimum_height_m: 5.18,
    average_height_m: 5.195,
    maximum_height_m: 5.21,
    duration_ms: 80,
    nominal_sample_rate_hz: 50,
    actual_average_sample_rate_hz: 50,
  },
  entry_rtk: {
    timestamp_ms: 1785978000000,
    latitude_deg: 39.9,
    longitude_deg: 116.39,
    altitude_m: 48.2,
    fix_type: "RTK_FIXED",
    valid: true,
  },
  exit_rtk: null,
  pause_intervals: [{ started_elapsed_ms: 30, ended_elapsed_ms: 35 }],
  samples: [
    { sample_index: 0, timestamp_ms: 1785978000000, elapsed_ms: 0, height_m: 5.2, lidar_to_top_m: 2.9, valid: true, invalid_reason: null, quality_score: 0.95 },
    { sample_index: 1, timestamp_ms: 1785978000020, elapsed_ms: 20, height_m: null, lidar_to_top_m: null, valid: false, invalid_reason: "insufficient_points", quality_score: null },
  ],
};

const jsonResponse = (body, status = 200) => new Response(JSON.stringify(body), {
  status,
  headers: { "Content-Type": "application/json" },
});

test("loads complete measurement history from FastAPI and preserves invalid gaps", async () => {
  const originalFetch = globalThis.fetch;
  let capturedInput;
  globalThis.fetch = async (input) => {
    capturedInput = input;
    return jsonResponse(payload);
  };

  try {
    const history = await loadMeasurementHistory(payload.task_id);
    assert.equal(capturedInput, `/api/v1/tasks/${payload.task_id}/measurements`);
    assert.equal(history.dataOrigin, "test_fixture");
    assert.equal(history.lane, "左车道");
    assert.equal(history.statistics.validSamples, 4);
    assert.equal(history.pauseIntervalCount, 1);
    assert.deepEqual(history.samples[1], {
      timestampMs: 1785978000020,
      heightM: null,
      valid: false,
      reason: "insufficient_points",
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("browser history code contains no direct ROS 2 interface", async () => {
  const source = await readFile(new URL("../components/playback/measurementHistoryApi.ts", import.meta.url), "utf8");
  assert.match(source, /\/api\/v1\/tasks\/\$\{encodeURIComponent\(taskId\)\}\/measurements/);
  assert.doesNotMatch(source, /rclpy|rclcpp|ros2|create_subscription|create_publisher|ros service|ros action/i);
});
