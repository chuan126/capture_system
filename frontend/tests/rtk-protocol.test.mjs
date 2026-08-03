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
  }));

  assert.equal(snapshot.type, "rtk_snapshot");
  assert.equal(snapshot.serial_connected, true);
  assert.equal(snapshot.gps_state, 0);
  assert.equal(snapshot.fix_status, -1);
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
    })),
    /数值字段无效/,
  );
});
