import assert from "node:assert/strict";
import test from "node:test";

import { downloadGeneratedFile, generateSummaryPdf, generateTaskTxt, loadReportPreview, startDeepSeekReportJob } from "../components/report/reportExportApi.ts";

const originalFetch = globalThis.fetch;
const originalDocument = globalThis.document;
test.afterEach(() => { globalThis.fetch = originalFetch; globalThis.document = originalDocument; });

const taskPayload = {
  task_id: "task-1",
  display_id: "20260807_145601",
  tunnel_code: "G45-001",
  tunnel_name: "测试隧道",
  status: "completed",
  exportable: true,
  blocked_reason: null,
  pdf_exportable: true,
  pdf_blocked_reason: null,
  data_origin: "recorded",
  lane: "left",
  started_at: "2026-08-07T06:56:10Z",
  ended_at: "2026-08-07T06:57:10Z",
  complete: true,
  total_samples: 3000,
  valid_samples: 2990,
  invalid_samples: 10,
  minimum_height_m: 5.18,
  normal_minimum_height_m: 5.20,
  clearance_threshold_m: 5.19,
  clearance_upper_limit_m: 5.80,
  entry_rtk: { timestamp_ms: 1, latitude_deg: 39.9, longitude_deg: 116.39, altitude_m: 48.2, fix_type: "RTK_FIXED", valid: true },
  exit_rtk: null,
};

const filePayload = {
  export_format: "txt",
  file_name: "20260807_145601_G45-001_50Hz测量明细.txt",
  file_size_bytes: 1234,
  generated_at: "2026-08-07T06:58:00Z",
  download_url: "/api/v1/tasks/task-1/exports/txt/download",
  report_id: null,
  task_id: "task-1",
  included_task_count: null,
  batch_id: null,
  batch_code: null,
};

test("loads selected-task report exportability from FastAPI without ROS 2 browser access", async () => {
  globalThis.fetch = async (url, init) => {
    assert.equal(url, "/api/v1/reports/clearance-summary/preview");
    assert.equal(init.method, "POST");
    assert.deepEqual(JSON.parse(init.body), { task_ids: ["task-1"] });
    return new Response(JSON.stringify({ task_count: 1, exportable_task_count: 1, generated_at: "2026-08-07T06:58:00Z", tasks: [taskPayload] }), { status: 200, headers: { "content-type": "application/json" } });
  };
  const preview = await loadReportPreview(["task-1"]);
  assert.equal(preview.exportableTaskCount, 1);
  assert.equal(preview.tasks[0].displayId, "20260807_145601");
  assert.equal(preview.tasks[0].lane, "左车道");
  assert.equal(preview.tasks[0].pdfExportable, true);
  assert.equal(preview.tasks[0].normalMinimumHeightM, 5.20);
  assert.equal(preview.tasks[0].clearanceUpperLimitM, 5.80);
});

test("preserves the right-based lane number in report preview", async () => {
  globalThis.fetch = async () => new Response(JSON.stringify({
    task_count: 1,
    exportable_task_count: 1,
    generated_at: "2026-08-07T06:58:00Z",
    tasks: [{
      ...taskPayload,
      lane: "unknown",
      travel_direction: "up",
      lane_side: "unknown",
      lane_number_from_right: 4,
    }],
  }), { status: 200, headers: { "content-type": "application/json" } });

  const preview = await loadReportPreview(["task-1"]);
  assert.equal(preview.tasks[0].lane, "上行右4车道");
});

test("creates TXT and selected-task PDF through FastAPI HTTP endpoints", async () => {
  const calls = [];
  globalThis.fetch = async (url, init) => {
    calls.push([url, init.method, init.body ? JSON.parse(init.body) : null]);
    const payload = url.includes("clearance-summary")
      ? { ...filePayload, export_format: "pdf", file_name: "20260807_150000_隧道净空检测汇总报告.pdf", report_id: "report-1", task_id: null, included_task_count: 1 }
      : filePayload;
    return new Response(JSON.stringify(payload), { status: 200, headers: { "content-type": "application/json" } });
  };
  const txt = await generateTaskTxt("task-1");
  const pdf = await generateSummaryPdf(["task-1"]);
  assert.equal(txt.exportFormat, "txt");
  assert.equal(pdf.exportFormat, "pdf");
  assert.deepEqual(calls, [
    ["/api/v1/tasks/task-1/exports/txt", "POST", null],
    ["/api/v1/reports/clearance-summary", "POST", { task_ids: ["task-1"] }],
  ]);
});

test("creates a fresh DeepSeek report job with the display-side key", async () => {
  let request = null;
  globalThis.fetch = async (url, init) => {
    request = [url, init.method, JSON.parse(init.body)];
    return new Response(JSON.stringify({
      job_id: "job-deepseek-1",
      export_format: "deepseek_pdf",
      task_ids: ["task-1"],
      state: "queued",
      phase: "等待导出资源",
      progress: 0,
      created_at: "2026-08-24T01:00:00Z",
      updated_at: "2026-08-24T01:00:00Z",
      error: null,
      file_name: null,
      file_size_bytes: null,
      generated_at: null,
      download_url: null,
      report_id: null,
      task_id: null,
      included_task_count: null,
    }), { status: 200, headers: { "content-type": "application/json" } });
  };

  const job = await startDeepSeekReportJob("task-1", {
    apiUrl: "https://api.deepseek.com/chat/completions",
    apiKey: "sk-entered-on-display",
    model: "deepseek-v4-pro",
    skillPrompt: "审计当前任务",
  });

  assert.equal(job.exportFormat, "deepseek_pdf");
  assert.deepEqual(request, [
    "/api/v1/tasks/task-1/export-jobs/deepseek-report",
    "POST",
    { api_key: "sk-entered-on-display", model: "deepseek-v4-pro", api_url: "https://api.deepseek.com/chat/completions", skill_prompt: "审计当前任务" },
  ]);
});

test("downloads generated files with the server-provided filename", async () => {
  let clicked = false;
  let removed = false;
  globalThis.document = {
    createElement: () => ({ href: "", download: "", rel: "", click: () => { clicked = true; }, remove: () => { removed = true; } }),
    body: { appendChild: () => {} },
  };
  await downloadGeneratedFile(filePayload);
  assert.equal(clicked, true);
  assert.equal(removed, true);
});
