import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

async function render() {
  const workerUrl = new URL("../dist/server/index.js", import.meta.url);
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

test("renders the tunnel clearance terminal shell", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);

  const html = await response.text();
  assert.match(html, /<html[^>]*lang="zh-CN"/i);
  assert.match(html, /<title>隧道净空测量显控终端<\/title>/i);
  assert.match(html, /车载隧道净空高度测量/);
  assert.match(html, /RTK定位/);
  assert.match(html, /融合定位/);
  assert.match(html, /俯仰/);
  assert.match(html, /任务控制/);
  assert.match(html, /创建任务/);
  assert.match(html, /高度下限阈值/);
  assert.match(html, /高度上限阈值/);
  assert.match(html, /雷达安装高度/);
  assert.match(html, /作业车道/);
  assert.match(html, /当前任务/);
  assert.match(html, /待测任务/);
  assert.match(html, /开始采集/);
  assert.match(html, /暂停/);
  assert.match(html, /停止/);
  assert.doesNotMatch(html, /入口坐标|出口坐标|预留指标一/);
  assert.doesNotMatch(html, /任务管理|历史任务管理|新建任务|批量创建/);
  assert.doesNotMatch(html, /任务进度|阶段进度|采集条件/);
});

test("keeps playback and report pages on real task context without simulated report data", async () => {
  const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");
  const playback = await readFile(new URL("../components/playback/PlaybackWorkspace.tsx", import.meta.url), "utf8");
  const report = await readFile(new URL("../components/report/ReportWorkspace.tsx", import.meta.url), "utf8");

  assert.match(page, /PlaybackWorkspace/);
  assert.match(page, /ReportWorkspace/);
  assert.match(playback, /净空高度曲线/);
  assert.match(report, /50 Hz 测量明细/);
  assert.match(report, /隧道净空检测汇总报告/);
  assert.doesNotMatch(`${page}${playback}${report}`, /browser-download-test|report-export-test|模拟数据/);
});

test("keeps the capture dashboard inside the viewport", async () => {
  const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");

  assert.match(css, /\.main--dashboard\s*\{[^}]*overflow:\s*hidden/i);
  assert.match(css, /\.main--dashboard \.page-content\s*\{[^}]*height:\s*100dvh/i);
  assert.match(css, /\.dashboard-page\s*\{[^}]*grid-template-rows:\s*clamp\(208px, 18vh, 236px\) minmax\(0, 1fr\)/i);
  assert.match(css, /@media \(max-width:\s*1180px\)[\s\S]*?\.main--dashboard\s*\{[^}]*overflow-y:\s*auto/i);
});

test("renders the local ENU point cloud as a three-dimensional view", async () => {
  const viewer = await readFile(
    new URL("../components/point-cloud/PointCloudViewer.tsx", import.meta.url),
    "utf8",
  );

  assert.match(viewer, /camera\.up\.set\(0, 0, 1\)/);
  assert.doesNotMatch(viewer, /OrthographicCamera|topCamera|沿X轴俯视/);
  assert.match(viewer, /enuSceneRoot\.add\(points\)/);
});

test("supports expanded point cloud and map panels", async () => {
  const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");
  const map = await readFile(new URL("../components/map/RealtimeAmap.tsx", import.meta.url), "utf8");
  const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");

  assert.match(page, /expandedVisual/);
  assert.match(page, /panel-expand-button/);
  assert.match(map, /onToggleExpanded/);
  assert.match(css, /\.visual-panel--expanded/);
});
