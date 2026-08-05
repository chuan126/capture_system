import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const RENDER_WORKER = "../dist/server/index.js";

async function renderHome() {
  const workerUrl = new URL(RENDER_WORKER, import.meta.url);
  workerUrl.searchParams.set("test", `${process.pid}-${Date.now()}`);
  const { default: worker } = await import(workerUrl.href);

  return worker.fetch(
    new Request("http://localhost/", {
      headers: { accept: "text/html" },
    }),
    {
      ASSETS: {
        fetch: async () => new Response("Not found", { status: 404 }),
      },
    },
    {
      waitUntil() {},
      passThroughOnException() {},
    },
  );
}

/* ------------------------------------------------------------------ */
/*  WGS-84 → GCJ-02 转换（独立副本，用于验证组件内实现）               */
/* ------------------------------------------------------------------ */

const PI = Math.PI;
const A = 6378245.0;
const EE = 0.00669342162296594323;

function isOutOfChina(lat, lon) {
  return lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271;
}

function transformLat(x, y) {
  let ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * Math.sqrt(Math.abs(x));
  ret += ((20.0 * Math.sin(6.0 * x * PI) + 20.0 * Math.sin(2.0 * x * PI)) * 2.0) / 3.0;
  ret += ((20.0 * Math.sin(y * PI) + 40.0 * Math.sin((y / 3.0) * PI)) * 2.0) / 3.0;
  ret += ((160.0 * Math.sin((y / 12.0) * PI) + 320 * Math.sin((y * PI) / 30.0)) * 2.0) / 3.0;
  return ret;
}

function transformLon(x, y) {
  let ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * Math.sqrt(Math.abs(x));
  ret += ((20.0 * Math.sin(6.0 * x * PI) + 20.0 * Math.sin(2.0 * x * PI)) * 2.0) / 3.0;
  ret += ((20.0 * Math.sin(x * PI) + 40.0 * Math.sin((x / 3.0) * PI)) * 2.0) / 3.0;
  ret += ((150.0 * Math.sin((x / 12.0) * PI) + 300.0 * Math.sin((x / 30.0) * PI)) * 2.0) / 3.0;
  return ret;
}

function wgs84ToGcj02(lat, lon) {
  if (isOutOfChina(lat, lon)) return [lon, lat];
  let dLat = transformLat(lon - 105.0, lat - 35.0);
  let dLon = transformLon(lon - 105.0, lat - 35.0);
  const radLat = (lat / 180.0) * PI;
  let magic = Math.sin(radLat);
  magic = 1 - EE * magic * magic;
  const sqrtMagic = Math.sqrt(magic);
  dLat = (dLat * 180.0) / (((A * (1 - EE)) / (magic * sqrtMagic)) * PI);
  dLon = (dLon * 180.0) / ((A / sqrtMagic) * Math.cos(radLat) * PI);
  return [lon + dLon, lat + dLat];
}

/* ------------------------------------------------------------------ */
/*  坐标转换测试                                                        */
/* ------------------------------------------------------------------ */

test("WGS-84 → GCJ-02 converts a known Beijing coordinate within expected offset", () => {
  // 天安门附近：WGS84 (39.9087, 116.3975) → GCJ-02 偏移约 300-500m
  const [gcjLon, gcjLat] = wgs84ToGcj02(39.9087, 116.3975);
  // GCJ-02 相对于 WGS-84 在东和北方向有正向偏移
  assert.ok(gcjLon > 116.3975, "GCJ-02 经度应大于 WGS-84");
  assert.ok(gcjLat > 39.9087, "GCJ-02 纬度应大于 WGS-84");
  // 偏移量通常在 100-700 米范围（约 0.001-0.007 度）
  const dLon = gcjLon - 116.3975;
  const dLat = gcjLat - 39.9087;
  assert.ok(dLon > 0.001 && dLon < 0.02, `经度偏移 ${dLon.toFixed(6)} 应在合理范围`);
  assert.ok(dLat > 0.001 && dLat < 0.02, `纬度偏移 ${dLat.toFixed(6)} 应在合理范围`);
});

test("WGS-84 → GCJ-02 returns unchanged coordinates outside China", () => {
  // 东京：WGS84 (35.6762, 139.6503) — 不在中国境内
  const [gcjLon, gcjLat] = wgs84ToGcj02(35.6762, 139.6503);
  assert.equal(gcjLon, 139.6503);
  assert.equal(gcjLat, 35.6762);
});

test("WGS-84 → GCJ-02 is deterministic", () => {
  const a = wgs84ToGcj02(31.2304, 121.4737); // 上海
  const b = wgs84ToGcj02(31.2304, 121.4737);
  assert.deepEqual(a, b);
});

/* ------------------------------------------------------------------ */
/*  渲染 HTML 测试                                                     */
/* ------------------------------------------------------------------ */

test("rendered dashboard includes real-time amap panel", async () => {
  const response = await renderHome();
  assert.equal(response.status, 200);

  const html = await response.text();
  assert.match(html, /实时地图/);
  assert.match(html, /高德地图/);
  assert.match(html, /地图设置/);
  assert.match(html, /amap-stage/);
  assert.match(html, /amap-container/);
  assert.match(html, /点云与实时地图/);
});

/* ------------------------------------------------------------------ */
/*  源码测试                                                           */
/* ------------------------------------------------------------------ */

test("page.tsx imports RealtimeAmap from the map component directory", async () => {
  const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");

  assert.match(page, /import RealtimeAmap from/);
  assert.match(page, /@\/components\/map\/RealtimeAmap/);
  assert.match(page, /<RealtimeAmap/);
  assert.match(page, /snapshot=\{rtkSnapshot\}/);
  assert.match(page, /hasFix=\{hasFix && rmcCharacter !== "V"\}/);
  assert.match(page, /connectionDetail=\{rtk\.detail\}/);
});

test("RealtimeAmap component contains WGS-84 to GCJ-02 conversion", async () => {
  const component = await readFile(
    new URL("../components/map/RealtimeAmap.tsx", import.meta.url),
    "utf8",
  );

  assert.match(component, /wgs84ToGcj02/);
  assert.match(component, /isOutOfChina/);
  assert.match(component, /transformLat/);
  assert.match(component, /transformLon/);
  assert.match(component, /MAX_TRACK_POINTS/);
  assert.match(component, /amap_js_key/);
  assert.match(component, /amap_security_code/);
  assert.match(component, /NEXT_PUBLIC_AMAP_KEY/);
  assert.match(component, /NEXT_PUBLIC_AMAP_SECURITY_CODE/);
  assert.match(component, /loadAmapScript/);
});

test("globals.css includes amap layout and marker styles", async () => {
  const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");

  assert.match(css, /\.dashboard-map-panel/);
  assert.match(css, /\.amap-stage/);
  assert.match(css, /\.amap-container/);
  assert.match(css, /\.amap-empty/);
  assert.match(css, /\.amap-status-row/);
  assert.match(css, /\.amap-map-tip/);
  assert.match(css, /\.amap-vehicle-marker/);
  assert.match(css, /\.map-modal-mask/);
  assert.match(css, /\.map-modal-panel/);
  assert.match(css, /\.amap-chip--button/);
});

test("globals.css responsive breakpoints preserve amap layout", async () => {
  const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");
  const tabletStart = css.indexOf("@media (max-width: 1180px)");
  const mobileStart = css.indexOf("@media (max-width: 760px)");
  const tabletRules = css.slice(tabletStart, mobileStart);
  const mobileRules = css.slice(mobileStart);

  assert.ok(tabletStart >= 0 && mobileStart > tabletStart);
  // 平板断点：visual-grid 保持两列
  assert.match(tabletRules, /\.dashboard-visual-grid\s*\{[^}]*grid-template-columns:\s*repeat\(2,\s*minmax\(0,\s*1fr\)\)/i);
  // 平板断点：amap-stage 有最小高度
  assert.match(tabletRules, /\.dashboard-main\s*\{[^}]*grid-template-rows:\s*minmax\(380px,\s*1fr\)\s*168px/i);
  // 手机断点：visual-grid 变为单列
  assert.match(mobileRules, /\.dashboard-visual-grid\s*\{[^}]*grid-template-columns:\s*1fr/i);
  // 手机断点：amap-stage 有固定高度
  assert.match(mobileRules, /\.amap-stage\s*\{[^}]*flex:\s*0 0 340px[^}]*min-height:\s*340px/i);
});
