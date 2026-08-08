import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const component = await readFile(new URL("../components/map/RealtimeAmap.tsx", import.meta.url), "utf8");

test("realtime amap uses RK3588 device configuration and server proxy", () => {
  assert.match(component, /\/api\/v1\/map\/config/);
  assert.match(component, /serviceHost/);
  assert.match(component, /_AMapSecurityConfig/);
  assert.doesNotMatch(component, /localStorage|NEXT_PUBLIC_AMAP_KEY|NEXT_PUBLIC_AMAP_SECURITY_CODE/);
  assert.match(component, /地图设置/);
  assert.match(component, /method: "PUT"/);
  assert.match(component, /security_js_code/);
  assert.match(component, /runtime\/settings/);
});
