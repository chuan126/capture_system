import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import {
  getPlaybackSessionId,
  loadMeasurementPrefix,
  loadMeasurementSeries,
  loadMeasurementSummary,
} from "../components/playback/measurementHistoryApi.ts";
import { loadInitialPlayback } from "../components/playback/playbackLoadCoordinator.ts";
import { getUserSeriesWindowRequest } from "../components/playback/playbackSeriesWindow.ts";

const summaryPayload = {
  task_id: "00000000-0000-4000-8000-000000000002",
  recording_schema_version: 1,
  data_origin: "test_fixture",
  lane: "left",
  travel_direction: "up",
  lane_side: "left",
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
  pause_interval_count: 1,
  first_sample_index: 0,
  last_sample_index: 4,
  first_timestamp_ms: 1785978000000,
  last_timestamp_ms: 1785978000080,
};

const seriesPayload = {
  task_id: summaryPayload.task_id,
  domain_start_timestamp_ms: 1785978000000,
  domain_end_timestamp_ms: 1785978000080,
  requested_start_timestamp_ms: 1785978000000,
  requested_end_timestamp_ms: 1785978000080,
  source_sample_count: 5,
  returned_sample_count: 2,
  downsampled: true,
  samples: [
    { sample_index: 0, timestamp_ms: 1785978000000, elapsed_ms: 0, height_m: 5.2, valid: true, invalid_reason: null },
    { sample_index: 2, timestamp_ms: 1785978000040, elapsed_ms: 40, height_m: null, valid: false, invalid_reason: "insufficient_points" },
  ],
};

test("keeps one playback session id so refresh and task switches can supersede stale work", () => {
  assert.equal(getPlaybackSessionId(), getPlaybackSessionId());
});

test("loads the fixed leading sample block without requesting the full task series", async () => {
  const originalFetch = globalThis.fetch;
  let capturedInput;
  let capturedInit;
  globalThis.fetch = async (input, init) => {
    capturedInput = input;
    capturedInit = init;
    return jsonResponse(seriesPayload);
  };

  try {
    const series = await loadMeasurementPrefix(summaryPayload.task_id, { maxSamples: 2000 });
    assert.match(String(capturedInput), /measurements\/series-prefix\?max_samples=2000/);
    assert.match(String(capturedInit.headers["X-Playback-Session"]), /^[0-9a-z-]+$/i);
    assert.equal(series.domainStartTimestampMs, 1785978000000);
    assert.equal(series.domainEndTimestampMs, 1785978000080);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("commits the prefix while the deferred summary is still pending", async () => {
  let resolveSummary;
  const pendingSummary = new Promise((resolve) => { resolveSummary = resolve; });
  const events = [];
  let scheduledSummary;

  await loadInitialPlayback({
    loadPrefix: async () => seriesPayload,
    loadSummary: () => pendingSummary,
    scheduleSummary: (load) => { scheduledSummary = load; },
    onPrefix: () => events.push("prefix committed"),
    onSummary: () => events.push("summary committed"),
    onPrefixError: (error) => { throw error; },
    onSummaryError: (error) => { throw error; },
  });

  assert.deepEqual(events, ["prefix committed"]);
  scheduledSummary();
  await Promise.resolve();
  assert.deepEqual(events, ["prefix committed"]);
  resolveSummary(summaryPayload);
  await pendingSummary;
  await Promise.resolve();
  assert.deepEqual(events, ["prefix committed", "summary committed"]);
});

test("initial prefix view does not request series and a user window requests only its time range", () => {
  const longSeries = {
    ...seriesPayload,
    domainStartTimestampMs: 1_000_000,
    domainEndTimestampMs: 2_000_000,
    requestedStartTimestampMs: 1_000_000,
    requestedEndTimestampMs: 1_040_000,
  };
  const initialView = { start: 0, end: 0.04 };
  assert.equal(getUserSeriesWindowRequest(longSeries, initialView, 0), null);

  const request = getUserSeriesWindowRequest(longSeries, { start: 0.5, end: 0.55 }, 1);
  assert.deepEqual(request, {
    startTimestampMs: 1_498_000,
    endTimestampMs: 1_552_000,
  });
  assert.ok(request.endTimestampMs - request.startTimestampMs < 1_000_000);
});

const jsonResponse = (body, status = 200) => new Response(JSON.stringify(body), {
  status,
  headers: { "Content-Type": "application/json" },
});

test("loads lightweight measurement summary without full sample payload", async () => {
  const originalFetch = globalThis.fetch;
  let capturedInput;
  globalThis.fetch = async (input) => {
    capturedInput = input;
    return jsonResponse(summaryPayload);
  };

  try {
    const summary = await loadMeasurementSummary(summaryPayload.task_id);
    assert.equal(capturedInput, `/api/v1/tasks/${summaryPayload.task_id}/measurements/summary`);
    assert.equal(summary.dataOrigin, "test_fixture");
    assert.equal(summary.lane, "上行左车道");
    assert.equal(summary.statistics.validSamples, 4);
    assert.equal(summary.pauseIntervalCount, 1);
    assert.equal(summary.firstSampleIndex, 0);
    assert.equal(summary.lastSampleIndex, 4);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("loads bounded adaptive series and preserves invalid gaps", async () => {
  const originalFetch = globalThis.fetch;
  let capturedInput;
  globalThis.fetch = async (input) => {
    capturedInput = input;
    return jsonResponse(seriesPayload);
  };

  try {
    const series = await loadMeasurementSeries(summaryPayload.task_id, {
      startTimestampMs: 1785978000000,
      endTimestampMs: 1785978000080,
      maxPoints: 4000,
    });
    assert.match(String(capturedInput), new RegExp(`/api/v1/tasks/${summaryPayload.task_id}/measurements/series\\?`));
    assert.match(String(capturedInput), /start_timestamp_ms=1785978000000/);
    assert.match(String(capturedInput), /end_timestamp_ms=1785978000080/);
    assert.match(String(capturedInput), /max_points=4000/);
    assert.equal(series.downsampled, true);
    assert.deepEqual(series.samples[1], {
      sampleIndex: 2,
      timestampMs: 1785978000040,
      elapsedMs: 40,
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
  assert.match(source, /\/api\/v1\/tasks\/\$\{encodeURIComponent\(taskId\)\}\/measurements\/summary/);
  assert.match(source, /\/api\/v1\/tasks\/\$\{encodeURIComponent\(taskId\)\}\/measurements\/series/);
  assert.match(source, /\/api\/v1\/tasks\/\$\{encodeURIComponent\(taskId\)\}\/measurements\/series-prefix/);
  assert.doesNotMatch(source, /rclpy|rclcpp|ros2|create_subscription|create_publisher|ros service|ros action/i);
});
