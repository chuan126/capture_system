from __future__ import annotations

import json
import sqlite3
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from backend.exports.deepseek_report import (
    DeepSeekAnalysis,
    DeepSeekClient,
    DeepSeekCompletion,
    DeepSeekReportError,
    DeepSeekReportService,
    _parse_completion,
)
from backend.exports import deepseek_report as deepseek_report_module
from backend.exports import deepseek_payload as deepseek_payload_module
from backend.exports.deepseek_payload import (
    MAX_SUPPORTED_DATABASE_BYTES,
    DeepSeekPayloadError,
    build_bounded_analysis_payload,
)
from backend.exports.service import ReportExportService
from backend.main import create_app
from backend.measurements.repository import MeasurementRepository
from backend.tasks.models import TaskCreateRequest
from backend.tasks.repository import TaskRepository
from backend.tests.test_report_export_api import (
    PDF_FONT,
    attach_measurements,
    create_measurement_database,
    make_static_site,
)


def test_measurements_db_is_reduced_to_a_bounded_source_frame_audit_package(
    tmp_path: Path,
) -> None:
    path = tmp_path / "task-1" / "measurements.db"
    create_measurement_database(path, "task-1")
    with sqlite3.connect(path) as connection:
        # schema v13升级后尚未写入真实源帧时，必须安全回退到非重复测量序列。
        connection.execute(
            """
            CREATE TABLE clearance_source_frames (
                source_sequence INTEGER PRIMARY KEY,
                source_timestamp_ns INTEGER NOT NULL,
                valid INTEGER NOT NULL,
                lidar_to_top_m REAL
            )
            """
        )

    result = build_bounded_analysis_payload(
        path,
        {"task_sequence": "20260824_010000", "database_path": str(path)},
        100_000,
    )
    payload = json.loads(result.json_text)

    assert payload["format"] == "capture-clearance-audit-v2"
    assert payload["policy"]["raw_database_uploaded"] is False
    assert payload["policy"]["high_rate_rows_uploaded"] is False
    assert "tables" not in payload
    assert payload["source_frame_statistics"]["total_frames"] == 4
    assert payload["source_frame_statistics"]["valid_frames"] == 3
    assert payload["source_frame_statistics"]["raw_min_m"] == 5.18
    assert len(payload["lowest_20_real_frames"]) == 3
    assert len(payload["candidate_contexts"]) <= 3
    assert payload["data_quality"]["imu_samples"]["rows"] == 2
    assert "database_path" not in payload["task"]
    assert result.byte_count < 100_000


def test_high_rate_rows_do_not_expand_deepseek_payload_with_database_size(
    tmp_path: Path,
) -> None:
    path = tmp_path / "task-large" / "measurements.db"
    create_measurement_database(path, "task-large")
    with sqlite3.connect(path) as connection:
        connection.execute("DELETE FROM imu_samples")
        connection.execute(
            """
            WITH RECURSIVE counter(value) AS (
                VALUES(0)
                UNION ALL
                SELECT value + 1 FROM counter WHERE value < 99999
            )
            INSERT INTO imu_samples(sample_index, recorded_timestamp_ns)
            SELECT value, 1785978000000000000 + value * 2500000 FROM counter
            """
        )

    result = build_bounded_analysis_payload(path, {}, 100_000)
    payload = json.loads(result.json_text)

    assert payload["data_quality"]["imu_samples"]["rows"] == 100_000
    assert result.database_size_bytes > result.byte_count
    assert result.byte_count < 100_000


def test_payload_builder_supports_two_gibibyte_limit_and_rejects_larger(
    tmp_path: Path,
    monkeypatch,
) -> None:
    path = tmp_path / "task-2g" / "measurements.db"
    create_measurement_database(path, "task-2g")
    monkeypatch.setattr(
        deepseek_payload_module,
        "_database_size",
        lambda _path: MAX_SUPPORTED_DATABASE_BYTES,
    )
    accepted = build_bounded_analysis_payload(path, {}, 100_000)
    assert accepted.database_size_bytes == MAX_SUPPORTED_DATABASE_BYTES

    monkeypatch.setattr(
        deepseek_payload_module,
        "_database_size",
        lambda _path: MAX_SUPPORTED_DATABASE_BYTES + 1,
    )
    with pytest.raises(DeepSeekPayloadError, match="超过当前支持的2 GiB上限"):
        build_bounded_analysis_payload(path, {}, 100_000)


def test_deepseek_json_response_is_strictly_parsed() -> None:
    content = {
        "n": 120,
        "raw": 4.12,
        "eff": 4.18,
        "f": 37,
        "t": "2026-08-24T01:00:00Z",
        "pre": [4.3, 4.2],
        "post": [4.2, 4.3],
        "len_f": 5,
        "len_m": None,
        "s": "R",
        "c": 0.72,
        "why": ["最低候选连续5帧"],
        "q": ["缺少可信里程"],
    }
    completion = _parse_completion(
        {
            "id": "response-1",
            "model": "deepseek-v4-flash",
            "choices": [
                {
                    "finish_reason": "stop",
                    "message": {"content": json.dumps(content, ensure_ascii=False)},
                }
            ],
            "usage": {"prompt_tokens": 100, "completion_tokens": 30},
        }
    )

    assert completion.response_id == "response-1"
    assert completion.analysis.effective_minimum_m == 4.18
    assert completion.analysis.confidence_score == 72
    assert "综合判定为待复核" in completion.analysis.report_analysis
    assert "V/R/O判定结论可信度为72%" in completion.analysis.data_quality_analysis
    assert "存在误检可能" in completion.analysis.data_quality_analysis


def test_deepseek_client_uses_official_single_non_streaming_json_request(monkeypatch) -> None:
    captured: dict[str, object] = {}
    response_payload = {
        "id": "response-http",
        "model": "deepseek-v4-pro",
        "choices": [{
            "finish_reason": "stop",
            "message": {"content": json.dumps({
                "n": 10, "raw": 4.2, "eff": 4.2, "f": 3, "t": "01:00:00",
                "pre": [], "post": [], "len_f": 4, "len_m": 2.1,
                "s": "V", "c": 0.9, "why": ["连续结构"], "q": [],
            }, ensure_ascii=False)},
        }],
        "usage": {},
    }

    class FakeResponse:
        def __enter__(self):
            return self

        def __exit__(self, *_args) -> None:
            return None

        def read(self) -> bytes:
            return json.dumps(response_payload, ensure_ascii=False).encode("utf-8")

    def fake_urlopen(request, *, timeout):
        captured["url"] = request.full_url
        captured["authorization"] = request.headers["Authorization"]
        captured["timeout"] = timeout
        captured["body"] = json.loads(request.data)
        return FakeResponse()

    monkeypatch.setattr(deepseek_report_module, "urlopen", fake_urlopen)
    completion = DeepSeekClient(
        "sk-from-display",
        "deepseek-v4-pro",
        timeout_seconds=45,
    ).analyze('{"format":"capture-clearance-audit-v2"}', {
        "task_sequence": "20260824_010000",
        "skill_prompt": "审计数据库={DB_PATH}；范围={TASK_OR_TIME_RANGE}。",
        "database_path": "/runtime/tasks/task-1/measurements.db",
        "task_range": "task-1",
    })

    assert completion.response_id == "response-http"
    assert captured["url"] == "https://api.deepseek.com/chat/completions"
    assert captured["authorization"] == "Bearer sk-from-display"
    assert captured["timeout"] == 45
    body = captured["body"]
    assert body["model"] == "deepseek-v4-pro"
    assert body["stream"] is False
    assert body["thinking"] == {"type": "enabled"}
    assert body["reasoning_effort"] == "high"
    assert body["response_format"] == {"type": "json_object"}
    assert body["max_tokens"] == 384_000
    assert "/runtime/tasks/task-1/measurements.db" in body["messages"][0]["content"]
    assert "最终输出契约（后端硬性要求" in body["messages"][0]["content"]
    assert '"n":真实有效源帧整数' in body["messages"][0]["content"]
    assert "analysis_package" in body["messages"][1]["content"]
    assert "capture-clearance-audit-v2" in body["messages"][1]["content"]
    assert len(body["messages"]) == 2


def test_deepseek_length_finish_reason_explains_how_to_raise_thinking_budget() -> None:
    with pytest.raises(
        DeepSeekReportError,
        match="CAPTURE_DEEPSEEK_MAX_TOKENS",
    ):
        _parse_completion({
            "choices": [{
                "finish_reason": "length",
                "message": {"content": "{\"n\":10"},
            }],
        })


def test_deepseek_parser_accepts_common_json_wrapper_and_value_variants() -> None:
    completion = _parse_completion({
        "id": "wrapped-response",
        "model": "deepseek-v4-flash",
        "choices": [{
            "finish_reason": "stop",
            "message": {"content": json.dumps({
                "result": {
                    "n": "12",
                    "raw": "4.21",
                    "eff": None,
                    "f": "103",
                    "t": "2026-08-24T01:02:03Z",
                    "len_f": "5",
                    "len_m": None,
                    "s": "待复核",
                    "c": 72,
                    "why": "连续性证据不足",
                    "q": "RTK无效",
                },
            }, ensure_ascii=False)},
        }],
    })

    assert completion.analysis.effective_minimum_m == 4.21
    assert completion.analysis.confidence_score == 72
    assert "待复核" in completion.analysis.report_analysis
    assert "RTK无效" in completion.analysis.data_quality_analysis


def test_deepseek_parser_reports_missing_contract_fields() -> None:
    with pytest.raises(
        DeepSeekReportError,
        match="缺少必填字段：n,s,c.*report_analysis",
    ):
        _parse_completion({
            "choices": [{
                "finish_reason": "stop",
                "message": {"content": '{"report_analysis":"自由格式"}'},
            }],
        })


def test_deepseek_report_uses_one_database_request_and_generates_pdf(tmp_path: Path) -> None:
    data_root = tmp_path / "runtime"
    captured: dict[str, object] = {}

    class FakeClient:
        def analyze(self, analysis_json: str, local_summary: dict[str, object]) -> DeepSeekCompletion:
            captured["analysis_json"] = analysis_json
            captured["local_summary"] = local_summary
            return DeepSeekCompletion(
                response_id="response-test",
                model="deepseek-v4-flash",
                finish_reason="stop",
                usage={"prompt_tokens": 120, "completion_tokens": 40},
                analysis=DeepSeekAnalysis(
                    report_analysis="任务数据完整，综合判定为待复核。",
                    data_quality_analysis="数据可信度中等，存在误检可能。",
                    effective_minimum_m=5.18,
                    confidence_score=72,
                ),
            )

    def fake_factory(api_key: str, model: str) -> FakeClient:
        captured["api_key"] = api_key
        captured["model"] = model
        return FakeClient()

    task_repository = TaskRepository(data_root / "capture.db", data_root / "tasks")
    task_repository.initialize()
    created = task_repository.create_tasks(
        [TaskCreateRequest(tunnel_code="AI-001", tunnel_name="大模型报告测试隧道")]
    )[0]
    relative_path = f"{created.task_id}/measurements.db"
    create_measurement_database(data_root / "tasks" / relative_path, created.task_id)
    with sqlite3.connect(data_root / "capture.db") as connection:
        connection.execute(
            """
            UPDATE tasks SET status='completed', operation_phase='completed', has_measurements=1,
                recording_path=?, started_at='2026-08-06T01:00:00Z',
                completed_at='2026-08-06T01:00:00.060Z' WHERE task_id=?
            """,
            (relative_path, created.task_id),
        )
    task = task_repository.get_task(created.task_id)
    measurement_repository = MeasurementRepository(data_root / "tasks")
    report_service = ReportExportService(
        data_root,
        task_repository,
        measurement_repository,
        pdf_font_path=PDF_FONT,
    )
    service = DeepSeekReportService(
        data_root,
        report_service,
        measurement_repository,
        pdf_font_path=PDF_FONT,
        client_factory=fake_factory,
    )
    generated = service.generate(
        task,
        "sk-test-display-storage",
        "deepseek-v4-flash",
        skill_prompt="数据库={DB_PATH}；范围={TASK_OR_TIME_RANGE}。",
    )

    assert captured["api_key"] == "sk-test-display-storage"
    assert captured["model"] == "deepseek-v4-flash"
    analysis_payload = json.loads(str(captured["analysis_json"]))
    assert analysis_payload["format"] == "capture-clearance-audit-v2"
    assert analysis_payload["data_quality"]["clearance_samples"]["rows"] == 4
    assert captured["local_summary"]["task_sequence"] == task.display_id
    assert captured["local_summary"]["skill_prompt"].startswith("数据库=")
    assert generated.export_format == "deepseek_pdf"
    assert generated.path.name.endswith("_大模型辅助分析报告.pdf")
    assert generated.path.read_bytes().startswith(b"%PDF-")
    manifest = json.loads((generated.path.parent / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["deepseek_response_id"] == "response-test"
    assert manifest["deepseek_model"] == "deepseek-v4-flash"
    assert manifest["analysis_payload_format"] == "capture-clearance-audit-v2"
    assert manifest["analysis_payload_bytes"] < 100_000
    assert "api_key" not in json.dumps(manifest)


def test_deepseek_report_endpoint_creates_a_new_job_with_display_key(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    with TestClient(
        create_app(
            static_dir,
            data_root=data_root,
            pdf_font_path=PDF_FONT,
            start_ros_bridge=False,
        )
    ) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "AI-ROUTE", "tunnel_name": "接口测试隧道"},
        ).json()
        # 端点契约只验证提交，不在此测试中访问外部网络。
        client.app.state.export_job_manager.stop()
        first = client.post(
            f"/api/v1/tasks/{task['task_id']}/export-jobs/deepseek-report",
            json={"api_key": "sk-route-test", "model": "deepseek-v4-flash"},
        )
        second = client.post(
            f"/api/v1/tasks/{task['task_id']}/export-jobs/deepseek-report",
            json={"api_key": "sk-route-test", "model": "deepseek-v4-flash"},
        )

        assert first.status_code == 202
        assert second.status_code == 202
        assert first.json()["export_format"] == "deepseek_pdf"
        assert first.json()["job_id"] != second.json()["job_id"]
        job_path = data_root / "export-jobs" / first.json()["job_id"] / "job.json"
        job_payload = json.loads(job_path.read_text(encoding="utf-8"))
        assert job_payload["deepseek_api_key"] == "sk-route-test"


def test_deepseek_settings_are_saved_in_runtime_and_reloaded(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"
    payload = {
        "api_url": "https://api.deepseek.com/chat/completions",
        "api_key": "sk-runtime",
        "model": "deepseek-v4-pro",
        "skill_prompt": "数据库={DB_PATH}；范围={TASK_OR_TIME_RANGE}。",
    }
    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        saved = client.put("/api/v1/deepseek/settings", json=payload)
        loaded = client.get("/api/v1/deepseek/settings")
    assert saved.status_code == 200
    assert loaded.json() == payload
    settings_path = data_root / "settings" / "device_settings.json"
    assert json.loads(settings_path.read_text(encoding="utf-8"))["deepseek"]["api_key"] == "sk-runtime"
