import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";

const page = fs.readFileSync(new URL("../app/page.tsx", import.meta.url), "utf8");
const component = fs.readFileSync(new URL("../components/network/WifiControl.tsx", import.meta.url), "utf8");
const api = fs.readFileSync(new URL("../components/network/wifiApi.ts", import.meta.url), "utf8");


test("wifi control is part of the formal sidebar and only displays the connected SSID", () => {
  assert.match(page, /<WifiControl \/>/);
  assert.match(component, /connected_ssid/);
  assert.match(component, /const displayName/);
  assert.doesNotMatch(component, /IP地址|接口名|当前信号|connected_ip|interface_name/);
});

test("wifi browser talks only to FastAPI network endpoints", () => {
  assert.match(api, /\/api\/v1\/network\/wifi\/status/);
  assert.match(api, /\/api\/v1\/network\/wifi\/rescan/);
  assert.match(api, /\/api\/v1\/network\/wifi\/connect/);
  assert.doesNotMatch(component + api, /nmcli|sudo|child_process|exec\(|spawn\(/);
});

test("wifi password is transient frontend state and not persisted in browser storage", () => {
  assert.match(component, /useState\(""\)/);
  assert.match(component, /type="password"/);
  assert.doesNotMatch(component + api, /localStorage|sessionStorage|indexedDB/);
});
