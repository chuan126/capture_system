import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import test from "node:test";

const outputUrl = new URL("../out/", import.meta.url);

test("device build exports a standalone static site", async () => {
  await access(new URL("index.html", outputUrl));
  await access(new URL("favicon.svg", outputUrl));
  await access(new URL("ctd-group-logo.png", outputUrl));

  const html = await readFile(new URL("index.html", outputUrl), "utf8");
  assert.match(html, /<html[^>]*lang="zh-CN"/i);
  assert.match(html, /<title>交科净界-大件运输净空动态分析系统<\/title>/i);
  assert.match(html, /蜀交科发 CTD GROUP/);
  assert.doesNotMatch(html, /https?:\/\/localhost/i);
});
