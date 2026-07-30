import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import test from "node:test";

const outputUrl = new URL("../out/", import.meta.url);

test("device build exports a standalone static site", async () => {
  await access(new URL("index.html", outputUrl));
  await access(new URL("favicon.svg", outputUrl));

  const html = await readFile(new URL("index.html", outputUrl), "utf8");
  assert.match(html, /<html[^>]*lang="zh-CN"/i);
  assert.match(html, /<title>隧道净空测量显控终端<\/title>/i);
  assert.match(html, /车载隧道净空高度测量/);
  assert.doesNotMatch(html, /https?:\/\/localhost/i);
});
