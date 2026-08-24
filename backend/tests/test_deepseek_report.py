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
    export_measurement_database,
    serialize_measurement_database,
)
from backend.exports import deepseek_report as deepseek_report_module
from backend.main import create_app
from backend.tests.test_report_export_api import (
    PDF_FONT,
    attach_measurements,
    make_static_site,
)


def test_measurements_db_is_exported_as_compact_table_json(tmp_path: Path) -> None:
    path = tmp_path / "measurements.db"
    with sqlite3.connect(path) as connection:
        connection.executescript(
            """
            CREATE TABLE recording_metadata (id INTEGER PRIMARY KEY, task_id TEXT);
            CREATE TABLE clearance_samples (
                sample_index INTEGER PRIMARY KEY,
                clearance_height_m REAL,
                valid INTEGER
            );
            INSERT INTO recording_metadata VALUES (1, 'task-1');
            INSERT INTO clearance_samples VALUES (0, 4.321, 1);
            """
        )

    payload = export_measurement_database(path)

    assert payload["format"] == "capture-measurements-db-json-v1"
    tables = {table["name"]: table for table in payload["tables"]}
    assert tables["clearance_samples"]["columns"] == [
        "sample_index", "clearance_height_m", "valid",
    ]
    assert tables["clearance_samples"]["rows"] == [[0, 4.321, 1]]
    assert "50Hz最近源帧保持序列" in payload["semantics"]["clearance_samples"]

    serialized = serialize_measurement_database(path, 100_000)
    assert json.loads(serialized) == payload


def test_measurements_db_serialization_fails_without_truncation_at_single_window_limit(
    tmp_path: Path,
) -> None:
    path = tmp_path / "measurements.db"
    with sqlite3.connect(path) as connection:
        connection.execute(
            "CREATE TABLE task_events (id INTEGER PRIMARY KEY, event_type TEXT, message TEXT)"
        )
        connection.executemany(
            "INSERT INTO task_events VALUES (?, 'note', ?)",
            [(index, "测量记录" * 50) for index in range(20)],
        )

    with pytest.raises(DeepSeekReportError, match="不做分块.*未向DeepSeek发送截断数据"):
        serialize_measurement_database(path, 500)


def test_deepseek_json_response_is_strictly_parsed() -> None:
    content = {
        "executive_summary": "任务整体有效。",
        "clearance_assessment": "最低净空需要现场复核。",
        "data_quality_assessment": "有效率较高。",
        "rtk_assessment": "入口和出口RTK有效。",
        "risk_assessment": "存在一处低值风险。",
        "recommendations": ["复核最低点簇。"],
        "conclusion": "建议结合现场复核后归档。",
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
    assert completion.analysis.recommendations == ("复核最低点簇。",)


def test_deepseek_client_uses_official_single_non_streaming_json_request(monkeypatch) -> None:
    captured: dict[str, object] = {}
    response_payload = {
        "id": "response-http",
        "model": "deepseek-v4-pro",
        "choices": [{
            "finish_reason": "stop",
            "message": {"content": json.dumps({
                "executive_summary": "任务完成。",
                "clearance_assessment": "净空结果可用。",
                "data_quality_assessment": "数据完整。",
                "rtk_assessment": "RTK端点已记录。",
                "risk_assessment": "需现场复核。",
                "recommendations": ["复核现场。"],
                "conclusion": "作为辅助分析。",
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
    ).analyze('{"tables":[]}', {"task_sequence": "20260824_010000"})

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
    assert len(body["messages"]) == 2


def test_deepseek_report_uses_one_database_request_and_generates_pdf(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"
    captured: dict[str, object] = {}

    class FakeClient:
        def analyze(self, database_json: str, local_summary: dict[str, object]) -> DeepSeekCompletion:
            captured["database_json"] = database_json
            captured["local_summary"] = local_summary
            return DeepSeekCompletion(
                response_id="response-test",
                model="deepseek-v4-flash",
                finish_reason="stop",
                usage={"prompt_tokens": 120, "completion_tokens": 40},
                analysis=DeepSeekAnalysis(
                    executive_summary="任务数据完整，已完成单窗口分析。",
                    clearance_assessment="设备端最低净空结果为可信分析依据。",
                    data_quality_assessment="有效记录数量满足报告要求。",
                    rtk_assessment="入口和出口RTK均有记录。",
                    risk_assessment="建议关注最低点簇对应位置。",
                    recommendations=("复核最低净空位置。", "保留原始数据库。"),
                    conclusion="本报告作为大模型辅助分析使用。",
                ),
            )

    def fake_factory(api_key: str, model: str) -> FakeClient:
        captured["api_key"] = api_key
        captured["model"] = model
        return FakeClient()

    with TestClient(
        create_app(
            static_dir,
            data_root=data_root,
            pdf_font_path=PDF_FONT,
            start_ros_bridge=False,
        )
    ) as client:
        task_payload = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "AI-001", "tunnel_name": "大模型报告测试隧道"},
        ).json()
        attach_measurements(data_root, task_payload)
        task = client.app.state.task_repository.get_task(task_payload["task_id"])
        service = DeepSeekReportService(
            data_root,
            client.app.state.report_export_service,
            client.app.state.measurement_repository,
            pdf_font_path=PDF_FONT,
            client_factory=fake_factory,
        )

        generated = service.generate(
            task,
            "sk-test-display-storage",
            "deepseek-v4-flash",
        )

    assert captured["api_key"] == "sk-test-display-storage"
    assert captured["model"] == "deepseek-v4-flash"
    assert '"name":"clearance_samples"' in str(captured["database_json"])
    assert captured["local_summary"]["task_sequence"] == task_payload["display_id"]
    assert generated.export_format == "deepseek_pdf"
    assert generated.path.name.endswith("_大模型辅助分析报告.pdf")
    assert generated.path.read_bytes().startswith(b"%PDF-")
    manifest = json.loads((generated.path.parent / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["deepseek_response_id"] == "response-test"
    assert manifest["deepseek_model"] == "deepseek-v4-flash"
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
