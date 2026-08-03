import assert from "node:assert/strict";
import test from "node:test";

import { parseSystemStatusText } from "../components/system-status/systemStatusProtocol.ts";

test("parses a complete system status snapshot", () => {
  const device = {state: "ok", message: "正常", values: {age_ms: "12.0"}};
  const result = parseSystemStatusText(JSON.stringify({
    type: "system_status_snapshot", sequence: 1, emitted_at_ns: 2,
    lidar: device, rtk: device, controller: device,
    storage: {state: "warn", message: "空间较低", values: {available_bytes: "1024"}},
  }));
  assert.equal(result.type, "system_status_snapshot");
  assert.equal(result.storage.state, "warn");
});

test("rejects an invalid device state", () => {
  const device = {state: "connected", message: "正常", values: {}};
  assert.throws(() => parseSystemStatusText(JSON.stringify({
    type: "system_status_snapshot", sequence: 1, emitted_at_ns: 2,
    lidar: device, rtk: device, controller: device, storage: device,
  })), /字段无效/);
});
