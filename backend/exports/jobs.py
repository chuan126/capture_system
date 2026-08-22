from __future__ import annotations

import json
import os
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Literal


ExportJobState = Literal["queued", "running", "completed", "failed", "cancelled"]


class ExportJobError(RuntimeError):
    """导出任务不存在、状态冲突或持久化失败。"""


@dataclass(frozen=True, slots=True)
class ExportJobRecord:
    job_id: str
    export_format: Literal["txt", "pdf"]
    task_ids: tuple[str, ...]
    state: ExportJobState
    phase: str
    progress: float
    created_at: str
    updated_at: str
    error: str | None = None
    file_name: str | None = None
    file_size_bytes: int | None = None
    generated_at: str | None = None
    download_path: str | None = None
    report_id: str | None = None
    task_id: str | None = None
    included_task_count: int | None = None

    @staticmethod
    def from_payload(payload: dict[str, object]) -> "ExportJobRecord":
        task_ids = payload.get("task_ids")
        if not isinstance(task_ids, list) or not all(isinstance(item, str) for item in task_ids):
            raise ExportJobError("导出任务索引中的task_ids无效")
        return ExportJobRecord(
            job_id=str(payload["job_id"]),
            export_format=str(payload["export_format"]),  # type: ignore[arg-type]
            task_ids=tuple(task_ids),
            state=str(payload["state"]),  # type: ignore[arg-type]
            phase=str(payload["phase"]),
            progress=float(payload["progress"]),
            created_at=str(payload["created_at"]),
            updated_at=str(payload["updated_at"]),
            error=_optional_string(payload.get("error")),
            file_name=_optional_string(payload.get("file_name")),
            file_size_bytes=_optional_int(payload.get("file_size_bytes")),
            generated_at=_optional_string(payload.get("generated_at")),
            download_path=_optional_string(payload.get("download_path")),
            report_id=_optional_string(payload.get("report_id")),
            task_id=_optional_string(payload.get("task_id")),
            included_task_count=_optional_int(payload.get("included_task_count")),
        )


def _optional_string(value: object) -> str | None:
    return None if value is None else str(value)


def _optional_int(value: object) -> int | None:
    return None if value is None else int(value)


def utc_now_text() -> str:
    from datetime import datetime, timezone

    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def read_job_payload(path: Path) -> dict[str, object]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ExportJobError(f"导出任务索引不可读取：{error}") from error
    if not isinstance(payload, dict):
        raise ExportJobError("导出任务索引格式无效")
    return payload


def write_job_payload(path: Path, payload: dict[str, object]) -> None:
    temporary_path: Path | None = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=".export-job-",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_path = Path(temporary.name)
            json.dump(payload, temporary, ensure_ascii=False, indent=2)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_path, path)
    except OSError as error:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
        raise ExportJobError(f"导出任务索引不可写入：{error}") from error


class ExportJobManager:
    """持久化单并发导出队列；重计算在独立低优先级进程内执行。"""

    def __init__(
        self,
        data_root: Path,
        *,
        project_root: Path,
        pdf_font_path: Path | None = None,
        active_task_provider: Callable[[], bool] | None = None,
    ) -> None:
        self.data_root = data_root.resolve()
        self.root = (self.data_root / "export-jobs").resolve()
        self.project_root = project_root.resolve()
        self.pdf_font_path = pdf_font_path.resolve() if pdf_font_path else None
        self.active_task_provider = active_task_provider
        self._queue: queue.Queue[str] = queue.Queue()
        self._lock = threading.Lock()
        self._stop_event = threading.Event()
        self._worker_thread: threading.Thread | None = None
        self._current_job_id: str | None = None
        self._current_process: subprocess.Popen[bytes] | None = None

    def start(self) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        with self._lock:
            if self._worker_thread is not None:
                return
            for path in sorted(self.root.glob("*/job.json")):
                try:
                    payload = read_job_payload(path)
                    state = str(payload.get("state"))
                    if state == "running":
                        payload.update(
                            state="failed",
                            phase="interrupted",
                            error="Web服务重启导致导出进程中断，请重新提交",
                            updated_at=utc_now_text(),
                        )
                        write_job_payload(path, payload)
                    elif state == "queued":
                        self._queue.put(str(payload["job_id"]))
                except (ExportJobError, KeyError, TypeError):
                    continue
            self._stop_event.clear()
            self._worker_thread = threading.Thread(
                target=self._run_queue,
                name="capture-export-job-manager",
                daemon=True,
            )
            self._worker_thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        with self._lock:
            process = self._current_process
        if process is not None and process.poll() is None:
            self._terminate_process(process)
        thread = self._worker_thread
        if thread is not None:
            thread.join(timeout=5.0)
        with self._lock:
            self._worker_thread = None
            self._current_process = None
            self._current_job_id = None

    def submit(
        self,
        export_format: Literal["txt", "pdf"],
        task_ids: list[str],
    ) -> ExportJobRecord:
        identifiers = list(dict.fromkeys(task_ids))
        if not identifiers:
            raise ExportJobError("导出任务至少需要一个任务ID")
        if export_format == "txt" and len(identifiers) != 1:
            raise ExportJobError("TXT导出只能包含一个任务")
        if len(identifiers) > 500:
            raise ExportJobError("一次导出最多包含500个任务")

        request_key = json.dumps(
            {"export_format": export_format, "task_ids": identifiers},
            ensure_ascii=False,
            sort_keys=True,
        )
        with self._lock:
            existing = self._find_active_request(request_key)
            if existing is not None:
                return existing
            job_id = str(uuid.uuid4())
            now = utc_now_text()
            payload: dict[str, object] = {
                "schema_version": 1,
                "job_id": job_id,
                "request_key": request_key,
                "export_format": export_format,
                "task_ids": identifiers,
                "state": "queued",
                "phase": "等待导出资源",
                "progress": 0.0,
                "created_at": now,
                "updated_at": now,
                "error": None,
            }
            write_job_payload(self._job_path(job_id), payload)
            self._queue.put(job_id)
        return ExportJobRecord.from_payload(payload)

    def get(self, job_id: str) -> ExportJobRecord:
        return ExportJobRecord.from_payload(read_job_payload(self._job_path(job_id)))

    def cancel(self, job_id: str) -> ExportJobRecord:
        path = self._job_path(job_id)
        with self._lock:
            payload = read_job_payload(path)
            state = str(payload.get("state"))
            if state in {"completed", "failed", "cancelled"}:
                return ExportJobRecord.from_payload(payload)
            payload.update(
                state="cancelled",
                phase="已取消",
                progress=float(payload.get("progress", 0.0)),
                updated_at=utc_now_text(),
            )
            write_job_payload(path, payload)
            if self._current_job_id == job_id and self._current_process is not None:
                self._terminate_process(self._current_process)
            return ExportJobRecord.from_payload(payload)

    def resolve_download(self, job_id: str) -> Path:
        record = self.get(job_id)
        if record.state != "completed" or record.download_path is None:
            raise ExportJobError("导出任务尚未完成")
        candidate = Path(record.download_path).resolve()
        try:
            candidate.relative_to(self.data_root)
        except ValueError as error:
            raise ExportJobError("导出文件路径超出数据目录") from error
        if not candidate.is_file():
            raise ExportJobError("导出文件不存在")
        return candidate

    def _find_active_request(self, request_key: str) -> ExportJobRecord | None:
        for path in self.root.glob("*/job.json"):
            try:
                payload = read_job_payload(path)
            except ExportJobError:
                continue
            if payload.get("request_key") == request_key and payload.get("state") in {
                "queued",
                "running",
            }:
                return ExportJobRecord.from_payload(payload)
        return None

    def _job_path(self, job_id: str) -> Path:
        try:
            normalized = str(uuid.UUID(job_id))
        except ValueError as error:
            raise ExportJobError("导出任务ID无效") from error
        path = (self.root / normalized / "job.json").resolve()
        try:
            path.relative_to(self.root)
        except ValueError as error:
            raise ExportJobError("导出任务路径超出数据目录") from error
        return path

    def _run_queue(self) -> None:
        while not self._stop_event.is_set():
            try:
                job_id = self._queue.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                record = self.get(job_id)
                if record.state != "queued":
                    continue
                if self.active_task_provider is not None and self.active_task_provider():
                    self._update_waiting_for_capture(job_id)
                    if self._stop_event.wait(1.0):
                        return
                    self._queue.put(job_id)
                    continue
                self._execute(job_id)
            except Exception as error:
                self._mark_failed(job_id, f"导出任务管理失败：{error}")
            finally:
                self._queue.task_done()

    def _execute(self, job_id: str) -> None:
        path = self._job_path(job_id)
        payload = read_job_payload(path)
        payload.update(
            state="running",
            phase="分析测量数据",
            progress=0.1,
            updated_at=utc_now_text(),
            error=None,
        )
        write_job_payload(path, payload)
        command = [
            sys.executable,
            "-m",
            "backend.exports.worker",
            "--data-root",
            str(self.data_root),
            "--job-file",
            str(path),
        ]
        if self.pdf_font_path is not None:
            command.extend(["--pdf-font", str(self.pdf_font_path)])
        log_path = path.parent / "worker.log"
        with log_path.open("ab", buffering=0) as log_file:
            process = subprocess.Popen(
                command,
                cwd=self.project_root,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            with self._lock:
                self._current_job_id = job_id
                self._current_process = process
            while process.poll() is None and not self._stop_event.wait(0.25):
                current = self.get(job_id)
                if current.state == "cancelled":
                    self._terminate_process(process)
                    break
            if self._stop_event.is_set() and process.poll() is None:
                self._terminate_process(process)
            exit_code = process.wait(timeout=5.0)
        with self._lock:
            self._current_job_id = None
            self._current_process = None
        current = self.get(job_id)
        if current.state == "cancelled":
            return
        if exit_code != 0 and current.state != "failed":
            self._mark_failed(job_id, f"导出工作进程异常退出，exit_code={exit_code}")

    def _update_waiting_for_capture(self, job_id: str) -> None:
        path = self._job_path(job_id)
        payload = read_job_payload(path)
        if payload.get("state") != "queued":
            return
        payload.update(phase="正式采集中，等待空闲后导出", updated_at=utc_now_text())
        write_job_payload(path, payload)

    def _mark_failed(self, job_id: str, message: str) -> None:
        try:
            path = self._job_path(job_id)
            payload = read_job_payload(path)
            if payload.get("state") == "cancelled":
                return
            payload.update(
                state="failed",
                phase="导出失败",
                error=message,
                updated_at=utc_now_text(),
            )
            write_job_payload(path, payload)
        except ExportJobError:
            return

    @staticmethod
    def _terminate_process(process: subprocess.Popen[bytes]) -> None:
        if process.poll() is not None:
            return
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=3.0)
        except (OSError, subprocess.TimeoutExpired):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except OSError:
                pass
