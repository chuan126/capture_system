import assert from "node:assert/strict";
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
  assert.match(html, /点云空间预览/);
  assert.match(html, /待机 · 尚未选择检测任务/);
  assert.match(html, /测量质量/);
  assert.match(html, /解状态 -- · 卫星 -- · HDOP --/);
  assert.match(html, /标记入口/);
  assert.match(html, /标记出口/);
  assert.match(html, /浏览器断开不会终止采集/);
  assert.match(html, /等待系统诊断/);
  assert.doesNotMatch(html, /当前无报警/);
  assert.doesNotMatch(html, />点云预览<\/button>/);
  assert.match(html, /数据回放/);
  assert.match(html, /报告导出/);
  assert.doesNotMatch(html, /codex-preview|react-loading-skeleton/i);
});
