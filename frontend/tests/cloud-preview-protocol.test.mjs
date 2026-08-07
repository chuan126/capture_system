import assert from "node:assert/strict";
import test from "node:test";

import {
  parseCloudPreviewBinary,
  parseCloudPreviewText,
} from "../components/point-cloud/cloudPreviewProtocol.ts";

function makeFrame(pointCount = 2) {
  const buffer = new ArrayBuffer(24 + pointCount * 12);
  const bytes = new Uint8Array(buffer);
  bytes.set([80, 67, 86, 49], 0);
  const view = new DataView(buffer);
  view.setUint16(4, 1, true);
  view.setUint16(6, 2, true);
  view.setUint32(8, 7, true);
  view.setBigUint64(12, 123n, true);
  view.setUint32(20, pointCount, true);
  return buffer;
}

test("parses a valid PCV1 frame", () => {
  const frame = parseCloudPreviewBinary(makeFrame());
  assert.equal(frame.sequence, 7);
  assert.equal(frame.sensorStampNs, 123n);
  assert.equal(frame.pointCount, 2);
  assert.equal(frame.positions.length, 6);
});

test("rejects truncated and oversized PCV1 frames", () => {
  assert.throws(() => parseCloudPreviewBinary(new ArrayBuffer(23)));
  assert.throws(() => parseCloudPreviewBinary(makeFrame(2), 1));

  const wrongLength = makeFrame(2).slice(0, 35);
  assert.throws(() => parseCloudPreviewBinary(wrongLength));
});

test("accepts production ENU and development sensor stream descriptions", () => {
  const stream = parseCloudPreviewText(JSON.stringify({
    type: "stream_info",
    protocol: "PCV1",
    version: 1,
    header_bytes: 24,
    point_format: "xyz_float32_le",
    point_stride: 12,
    max_points: 10_000,
    frame_id: "lidar_local_enu",
    coordinate_mode: "local_enu",
    sensor_clock: "device_boot",
    color_mode: "single",
  }));
  assert.equal(stream.type, "stream_info");
  assert.equal(stream.frame_id, "lidar_local_enu");

  const sensorStream = parseCloudPreviewText(JSON.stringify({
    ...stream,
    frame_id: "odin_sensor",
    coordinate_mode: "sensor",
  }));
  assert.equal(sensorStream.type, "stream_info");
  assert.equal(sensorStream.coordinate_mode, "sensor");

  assert.throws(() => parseCloudPreviewText(JSON.stringify({
    ...stream,
    coordinate_mode: "sensor_local",
  })));
});
