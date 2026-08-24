from __future__ import annotations

from pathlib import Path
import json
import time

import pytest

from backend.exports.jobs import ExportJobError, ExportJobManager


def make_manager(tmp_path: Path) -> ExportJobManager:
    return ExportJobManager(tmp_path, project_root=Path(__file__).resolve().parents[2])


def test_export_job_is_durable_deduplicated_and_cancellable(tmp_path: Path) -> None:
    manager = make_manager(tmp_path)
    first = manager.submit("txt", ["task-1"])
    duplicate = manager.submit("txt", ["task-1"])

    assert duplicate.job_id == first.job_id
    assert manager.get(first.job_id).state == "queued"
    assert (tmp_path / "export-jobs" / first.job_id / "job.json").is_file()

    cancelled = manager.cancel(first.job_id)
    assert cancelled.state == "cancelled"
    assert manager.get(first.job_id).state == "cancelled"


def test_export_job_validates_scope_and_download_state(tmp_path: Path) -> None:
    manager = make_manager(tmp_path)

    with pytest.raises(ExportJobError, match="至少需要一个任务"):
        manager.submit("pdf", [])
    with pytest.raises(ExportJobError, match="TXT导出只能包含一个任务"):
        manager.submit("txt", ["task-1", "task-2"])

    queued = manager.submit("pdf", ["task-1", "task-1", "task-2"])
    assert queued.task_ids == ("task-1", "task-2")
    with pytest.raises(ExportJobError, match="尚未完成"):
        manager.resolve_download(queued.job_id)


def test_deepseek_job_always_creates_new_window_and_scrubs_key_on_cancel(tmp_path: Path) -> None:
    manager = make_manager(tmp_path)
    first = manager.submit(
        "deepseek_pdf",
        ["task-1"],
        deepseek_api_key="sk-test-one",
        deepseek_model="deepseek-v4-flash",
        deepseek_api_url="https://api.deepseek.com/chat/completions",
        deepseek_skill_prompt="审计当前任务",
    )
    second = manager.submit(
        "deepseek_pdf",
        ["task-1"],
        deepseek_api_key="sk-test-one",
        deepseek_model="deepseek-v4-flash",
        deepseek_api_url="https://api.deepseek.com/chat/completions",
        deepseek_skill_prompt="审计当前任务",
    )

    assert first.job_id != second.job_id
    first_path = tmp_path / "export-jobs" / first.job_id / "job.json"
    assert json.loads(first_path.read_text(encoding="utf-8"))["deepseek_api_key"] == "sk-test-one"

    manager.cancel(first.job_id)
    assert "deepseek_api_key" not in json.loads(first_path.read_text(encoding="utf-8"))

    with pytest.raises(ExportJobError, match="只能包含一个任务"):
        manager.submit(
            "deepseek_pdf",
            ["task-1", "task-2"],
            deepseek_api_key="sk-test-one",
        )
    with pytest.raises(ExportJobError, match="API Key不能为空"):
        manager.submit("deepseek_pdf", ["task-1"])


def test_export_job_waits_while_formal_capture_is_active(tmp_path: Path) -> None:
    manager = ExportJobManager(
        tmp_path,
        project_root=Path(__file__).resolve().parents[2],
        active_task_provider=lambda: True,
    )
    manager.start()
    try:
        record = manager.submit("txt", ["task-1"])
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            record = manager.get(record.job_id)
            if record.phase == "正式采集中，等待空闲后导出":
                break
            time.sleep(0.02)
        assert record.state == "queued"
        assert record.phase == "正式采集中，等待空闲后导出"
    finally:
        manager.stop()
