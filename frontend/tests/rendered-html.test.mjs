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
  assert.match(html, /Odin1 Lite/);
  assert.match(html, /采集首页/);
  assert.match(html, /任务管理/);
  assert.match(html, /点云实时预览/);
  assert.doesNotMatch(html, /Odin1 Lite SLAM世界点云 · 浏览器轻量预览/);
  assert.match(html, /尚未选择任务/);
  assert.match(html, />开始<\/button>/);
  assert.match(html, />暂停<\/button>/);
  assert.match(html, />停止<\/button>/);
  assert.match(html, /测量与任务控制/);
  assert.match(html, /雷达到当前最低顶面/);
  assert.doesNotMatch(html, /有效候选面/);
  assert.doesNotMatch(html, /单帧算法耗时/);
  assert.doesNotMatch(html, /净空计算状态/);
  assert.match(html, /卫星数 \/ HDOP \/ PDOP/);
  assert.match(html, /当前坐标/);
  assert.match(html, /RMC --/);
  assert.doesNotMatch(html, /暂无有效结果/);
  assert.doesNotMatch(html, /结果状态/);
  assert.doesNotMatch(html, /标记入口/);
  assert.doesNotMatch(html, /标记出口/);
  assert.match(html, /浏览器断开不会终止RK3588上的采集与计算任务/);
  assert.doesNotMatch(html, /诊断链路尚未接入/);
  assert.doesNotMatch(html, /任务进度/);
  assert.doesNotMatch(html, /任务设置/);
  assert.match(html, /任务编号/);
  assert.match(html, /隧道名称/);
  assert.match(html, /检测车道/);
  assert.match(html, /参数方案/);
  assert.doesNotMatch(html, /预览延迟/);
  assert.doesNotMatch(html, /等待诊断/);
  assert.doesNotMatch(html, /当前无报警/);
  assert.doesNotMatch(html, />点云预览<\/button>/);
  assert.match(html, /数据回放/);
  assert.match(html, /报告导出/);
  assert.doesNotMatch(html, /codex-preview|react-loading-skeleton/i);
});

test("keeps report export focused on one TXT file and task data", async () => {
  const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");

  assert.match(page, /下载测试 TXT/);
  assert.match(page, /browser-download-test/);
  assert.match(page, /\/api\/v1\/report-export-test/);
  assert.match(page, /任务数据概览/);
  assert.match(page, /净空高度曲线/);
  assert.doesNotMatch(page, /综合检测报告/);
  assert.doesNotMatch(page, /报告内容/);
});

test("keeps the 1920x1080 capture dashboard inside the viewport", async () => {
  const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");

  assert.match(css, /\.main--dashboard\s*\{[^}]*overflow:\s*hidden/i);
  assert.match(css, /\.main--dashboard \.page-content\s*\{[^}]*height:\s*100dvh/i);
  assert.match(css, /\.dashboard-page\s*\{[^}]*grid-template-rows:\s*64px minmax\(0, 1fr\)/i);
  assert.match(css, /@media \(max-width:\s*1180px\)[\s\S]*?\.main--dashboard\s*\{[^}]*overflow-y:\s*auto/i);
});

test("keeps dashboard visualizations visible at responsive breakpoints", async () => {
  const css = await readFile(new URL("../app/globals.css", import.meta.url), "utf8");
  const tabletStart = css.indexOf("@media (max-width: 1180px)");
  const mobileStart = css.indexOf("@media (max-width: 760px)");
  const tabletRules = css.slice(tabletStart, mobileStart);
  const mobileRules = css.slice(mobileStart);

  assert.ok(tabletStart >= 0 && mobileStart > tabletStart);
  assert.match(tabletRules, /\.dashboard-cloud-panel \.dashboard-cloud-stage\s*\{[^}]*flex:\s*0 0 380px[^}]*min-height:\s*380px/i);
  assert.match(tabletRules, /\.dashboard-main > \.panel > \.chart\s*\{[^}]*min-height:\s*168px/i);
  assert.match(mobileRules, /\.dashboard-cloud-panel \.dashboard-cloud-stage\s*\{[^}]*flex-basis:\s*340px[^}]*min-height:\s*340px/i);
  assert.match(mobileRules, /\.dashboard-main > \.panel > \.chart\s*\{[^}]*min-height:\s*190px/i);
});

test("renders the local ENU point cloud and world axes", async () => {
  const viewer = await readFile(
    new URL("../components/point-cloud/PointCloudViewer.tsx", import.meta.url),
    "utf8",
  );

  assert.match(viewer, /perspectiveCamera\.up\.set\(0, 0, 1\)/);
  assert.match(viewer, /topCamera\.position\.set\(center\.x, center\.y, center\.z \+ span \* 2\)/);
  assert.match(viewer, /grid\.rotation\.x = Math\.PI \/ 2/);
  assert.match(viewer, /makeAxisLabel\("东 E"/);
  assert.match(viewer, /makeAxisLabel\("北 N"/);
  assert.match(viewer, /makeAxisLabel\("天 U"/);
  assert.match(viewer, /enuSceneRoot\.add\(points\)/);
  assert.match(viewer, /enuSceneRoot\.add\(eastLabel, northLabel, upLabel\)/);
  assert.match(viewer, /雷达局部东北天 · X=东 · Y=北 · Z=天/);
});

test("renders the current clearance value and rolling live chart", async () => {
  const page = await readFile(new URL("../app/page.tsx", import.meta.url), "utf8");

  assert.match(page, /雷达到当前最低顶面/);
  assert.match(page, /function LiveClearanceChart/);
  assert.match(page, /slice\(-120\)/);
  assert.match(page, /snapshot\.lidar_to_top_m/);
  assert.match(page, /实时净空高度曲线/);
});
