import assert from "node:assert/strict";
import test from "node:test";

import { parseRtkText } from "../components/rtk/rtkProtocol.ts";

test("parses an indoor no-fix RTK snapshot without adding quality fields", () => {
  const snapshot = parseRtkText(JSON.stringify({
    type: "rtk_snapshot",
    sequence: 7,
    emitted_at_ns: 123,
    serial_connected: true,
    serial_message: "串口已连接",
    status_stamp_ns: 100,
    event_mask: 4,
    rmc_validity: 86,
    gps_state: 0,
    satellite_count: 0,
    hdop: 0,
    pdop: 0,
    latitude_sigma: 0,
    longitude_sigma: 0,
    height_sigma: 0,
    speed_knots: 0,
    track_degrees: 0,
    fix_stamp_ns: 101,
    fix_status: -1,
    latitude: 0,
    longitude: 0,
    altitude: 0,
    localization_stamp_ns: 200,
    localization_valid: false,
    localization_mode: 0,
    localization_heading_source: 0,
    localization_latitude: 0,
    localization_longitude: 0,
    localization_altitude: 0,
    localization_heading_deg: 0,
    localization_heading_alignment_valid: false,
    localization_delta_yaw_deg: 0,
    localization_scale_calibration_enabled: false,
    localization_scale_valid: false,
    localization_horizontal_scale: 1,
    localization_vertical_scale: 1,
    localization_scale_baseline_m: 0,
    localization_heading_baseline_m: 0,
    localization_distance_from_anchor_m: 0,
    localization_dr_duration_s: 0,
    localization_rtk_age_s: -1,
    localization_odometry_age_s: -1,
    localization_imu_age_s: -1,
    localization_position_difference_to_rtk_m: 0,
    localization_invalid_reason: "NO_VALID_RTK_ANCHOR",
  }));

  assert.equal(snapshot.type, "rtk_snapshot");
  assert.equal(snapshot.serial_connected, true);
  assert.equal(snapshot.gps_state, 0);
  assert.equal(snapshot.fix_status, -1);
  assert.equal(snapshot.localization_valid, false);
  assert.equal(snapshot.localization_invalid_reason, "NO_VALID_RTK_ANCHOR");
  assert.equal("quality" in snapshot, false);
  assert.equal("stable" in snapshot, false);
});

test("rejects malformed RTK numeric fields", () => {
  assert.throws(
    () => parseRtkText(JSON.stringify({
      type: "rtk_snapshot",
      sequence: 1,
      emitted_at_ns: 1,
      serial_connected: true,
      serial_message: "串口已连接",
      status_stamp_ns: null,
      event_mask: null,
      rmc_validity: null,
      gps_state: "0",
      satellite_count: null,
      hdop: null,
      pdop: null,
      latitude_sigma: null,
      longitude_sigma: null,
      height_sigma: null,
      speed_knots: null,
      track_degrees: null,
      fix_stamp_ns: null,
      fix_status: null,
      latitude: null,
      longitude: null,
      altitude: null,
      localization_stamp_ns: null,
      localization_mode: null,
      localization_heading_source: null,
      localization_latitude: null,
      localization_longitude: null,
      localization_altitude: null,
      localization_heading_deg: null,
      localization_delta_yaw_deg: null,
      localization_horizontal_scale: null,
      localization_vertical_scale: null,
      localization_scale_baseline_m: null,
      localization_heading_baseline_m: null,
      localization_distance_from_anchor_m: null,
      localization_dr_duration_s: null,
      localization_rtk_age_s: null,
      localization_odometry_age_s: null,
      localization_imu_age_s: null,
      localization_position_difference_to_rtk_m: null,
      localization_valid: null,
      localization_heading_alignment_valid: null,
      localization_scale_calibration_enabled: null,
      localization_scale_valid: null,
      localization_invalid_reason: null,
    })),
    /数值字段无效/,
  );
});
