import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";

import {
  parseCloudPreviewBinary,
  parseCloudPreviewText,
} from "../components/point-cloud/cloudPreviewProtocol.ts";

const viewerSource = fs.readFileSync(
  new URL("../components/point-cloud/PointCloudViewer.tsx", import.meta.url),
  "utf8",
);

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

function makeClassifiedFrame(classifications = [0, 1, 2]) {
  const buffer = new ArrayBuffer(24 + classifications.length * 16);
  const bytes = new Uint8Array(buffer);
  bytes.set([80, 67, 86, 50], 0);
  const view = new DataView(buffer);
  view.setUint16(4, 2, true);
  view.setUint16(6, 2, true);
  view.setUint32(8, 8, true);
  view.setBigUint64(12, 456n, true);
  view.setUint32(20, classifications.length, true);
  classifications.forEach((classification, index) => {
    const offset = 24 + index * 16;
    view.setFloat32(offset, index + 0.25, true);
    view.setFloat32(offset + 4, index + 0.5, true);
    view.setFloat32(offset + 8, -index, true);
    view.setUint8(offset + 12, classification);
  });
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

test("parses PCV2 positions and exact blue green red palette", () => {
  const frame = parseCloudPreviewBinary(makeClassifiedFrame());
  assert.equal(frame.sequence, 8);
  assert.equal(frame.sensorStampNs, 456n);
  assert.equal(frame.pointCount, 3);
  assert.deepEqual(Array.from(frame.positions), [0.25, 0.5, -0, 1.25, 1.5, -1, 2.25, 2.5, -2]);
  const expected = [
    0x3b / 255, 0x82 / 255, 0xf6 / 255,
    0x22 / 255, 0xc5 / 255, 0x5e / 255,
    1, 0x4d / 255, 0x4f / 255,
  ];
  assert.ok(frame.colors);
  Array.from(frame.colors).forEach((value, index) => {
    assert.ok(Math.abs(value - expected[index]) < 1e-6, `color[${index}]`);
  });
});

test("PCV2 falls back unknown classifications to blue and validates length", () => {
  const frame = parseCloudPreviewBinary(makeClassifiedFrame([255]));
  assert.ok(frame.colors);
  assert.ok(Math.abs(frame.colors[0] - 0x3b / 255) < 1e-6);
  assert.ok(Math.abs(frame.colors[1] - 0x82 / 255) < 1e-6);
  assert.ok(Math.abs(frame.colors[2] - 0xf6 / 255) < 1e-6);
  assert.throws(() => parseCloudPreviewBinary(makeClassifiedFrame([0, 1]).slice(0, 55)));
});

test("accepts production and development raw sensor stream descriptions", () => {
  const stream = parseCloudPreviewText(JSON.stringify({
    type: "stream_info",
    protocol: "PCV1",
    version: 1,
    header_bytes: 24,
    point_format: "xyz_float32_le",
    point_stride: 12,
    max_points: 10_000,
    frame_id: "device0/odom",
    coordinate_mode: "sensor",
    sensor_clock: "device_boot",
    color_mode: "single",
  }));
  assert.equal(stream.type, "stream_info");
  assert.equal(stream.frame_id, "device0/odom");

  const sensorStream = parseCloudPreviewText(JSON.stringify({
    ...stream,
    frame_id: "odin_sensor",
    coordinate_mode: "sensor",
  }));
  assert.equal(sensorStream.type, "stream_info");
  assert.equal(sensorStream.coordinate_mode, "sensor");

  const classifiedStream = parseCloudPreviewText(JSON.stringify({
    ...stream,
    protocol: "PCV2",
    version: 2,
    point_format: "xyz_float32_class_uint8_le",
    point_stride: 16,
    color_mode: "classification",
  }));
  assert.equal(classifiedStream.protocol, "PCV2");
  assert.equal(classifiedStream.color_mode, "classification");

  assert.throws(() => parseCloudPreviewText(JSON.stringify({
    ...stream,
    protocol: "PCV2",
    version: 2,
    point_stride: 16,
    color_mode: "single",
  })));

  assert.throws(() => parseCloudPreviewText(JSON.stringify({
    ...stream,
    coordinate_mode: "sensor_local",
  })));
});

test("viewer only renders device classifications and exposes matching legend", () => {
  assert.match(viewerSource, /frame\.colors/);
  assert.match(viewerSource, /vertexColors: true/);
  assert.match(viewerSource, /#3B82F6/);
  assert.match(viewerSource, /#22C55E/);
  assert.match(viewerSource, /#FF4D4F/);
  assert.match(viewerSource, /仅预览/);
  assert.match(viewerSource, /计算范围/);
  assert.match(viewerSource, /最低可信簇/);
  assert.match(viewerSource, /makeAxisLabel\("X"/);
  assert.match(viewerSource, /makeAxisLabel\("Y"/);
  assert.match(viewerSource, /makeAxisLabel\("Z"/);
  assert.doesNotMatch(viewerSource, /东 E|北 N|天 U|axisMode|lidar-local-enu/);
  assert.doesNotMatch(
    viewerSource,
    /detection_radius|clearance_threshold|clearance_upper|lowest_cluster|Math\.sqrt/,
  );
});
