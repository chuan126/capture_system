import assert from "node:assert/strict";
import test from "node:test";

import { parseClearanceText } from "../components/clearance/clearanceProtocol.ts";

test("parses a valid clearance snapshot", () => {
  const result = parseClearanceText(JSON.stringify({
    type: "clearance_snapshot",
    sequence: 7,
    emitted_at_ns: 100,
    stamp_ns: 90,
    frame_id: "lidar",
    valid: true,
    lidar_to_top_m: 1.723,
    candidate_count: 4,
    selected_inlier_count: 1234,
    selected_area_m2: 1.1,
    selected_tilt_deg: 2.3,
    residual_median_m: 0.01,
    residual_p95_m: 0.03,
    minimum_position_east_m: -0.4,
    minimum_position_north_m: 0.2,
    minimum_position_up_m: 1.723,
    valid_point_ratio: 0.51,
    invalid_reason: "NONE",
    processing_time_ms: 46.8,
  }));

  assert.equal(result.type, "clearance_snapshot");
  assert.equal(result.valid, true);
  assert.equal(result.lidar_to_top_m, 1.723);
});

test("accepts null measurement fields for an invalid frame", () => {
  const result = parseClearanceText(JSON.stringify({
    type: "clearance_snapshot",
    sequence: 8,
    emitted_at_ns: 100,
    stamp_ns: 90,
    frame_id: "lidar",
    valid: false,
    lidar_to_top_m: null,
    candidate_count: 0,
    selected_inlier_count: 0,
    selected_area_m2: null,
    selected_tilt_deg: null,
    residual_median_m: null,
    residual_p95_m: null,
    minimum_position_east_m: null,
    minimum_position_north_m: null,
    minimum_position_up_m: null,
    valid_point_ratio: 0.2,
    invalid_reason: "NO_PLANE_FOUND",
    processing_time_ms: 40,
  }));

  assert.equal(result.type, "clearance_snapshot");
  assert.equal(result.valid, false);
  assert.equal(result.lidar_to_top_m, null);
});
