from __future__ import annotations

import os
import re
import shutil
import signal
import subprocess
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path


class DevRecordingError(RuntimeError):
    pass


@dataclass(frozen=True, slots=True)
class RecordingProfile:
    name: str
    directory_prefix: str
    topics: tuple[str, ...]


RAW_CLOUD_PROFILE = RecordingProfile(
    name="raw_cloud",
    directory_prefix="raw-cloud",
    topics=("/capture/lidar/points_raw",),
)

DIAGNOSTIC_PROFILE = RecordingProfile(
    name="diagnostic",
    directory_prefix="diagnostic",
    topics=(
        "/capture/clearance/result",
        "/capture/rtk/fix",
        "/capture/rtk/status",
        "/capture/task/status",
        "/capture/system/diagnostics",
        "/capture/odometry/high_rate",
    ),
)

_RECORDING_ID = re.compile(r"^[a-z0-9-]+_[0-9]{8}_[0-9]{6}_[0-9a-f]{6}$")


def _directory_bytes(path: Path) -> int:
    total = 0
    if not path.exists():
        return 0
    for item in path.rglob("*"):
        try:
            if item.is_file():
                total += item.stat().st_size
        except OSError:
            pass
    return total


class RosbagRecordingManager:
    """Restricted development rosbag2/MCAP recorder. No arbitrary command input is accepted."""

    def __init__(self, data_root: Path, min_free_bytes: int = 2 * 1024**3) -> None:
        self.root = (data_root / "dev-tests").resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.min_free_bytes = min_free_bytes
        self._lock = threading.Lock()
        self._process: subprocess.Popen[bytes] | None = None
        self._profile: RecordingProfile | None = None
        self._recording_id: str | None = None
        self._path: Path | None = None
        self._started_at_ns: int | None = None
        self._duration_timer: threading.Timer | None = None
        self._watchdog_timer: threading.Timer | None = None
        self._last_error: str | None = None

    def stop_on_shutdown(self) -> None:
        try:
            self.stop()
        except DevRecordingError:
            pass

    def start(self, profile: RecordingProfile, duration_seconds: int | None = None) -> dict[str, object]:
        if duration_seconds is not None and duration_seconds not in {5, 10, 30}:
            raise DevRecordingError("自动录制时长仅支持5、10或30秒")
        if duration_seconds is None and profile is not RAW_CLOUD_PROFILE:
            raise DevRecordingError("独立诊断录制仅支持5、10或30秒")
        with self._lock:
            self._reap_locked()
            if self._process is not None:
                raise DevRecordingError("已有开发诊断录制正在进行")
            if shutil.which("ros2") is None:
                raise DevRecordingError("未找到ros2命令")
            free_bytes = shutil.disk_usage(self.root).free
            if free_bytes < self.min_free_bytes:
                raise DevRecordingError(
                    f"开发录制剩余空间不足：{free_bytes / 1024**3:.2f} GiB，至少需要{self.min_free_bytes / 1024**3:.2f} GiB"
                )

            now = time.localtime()
            timestamp = time.strftime("%Y%m%d_%H%M%S", now)
            recording_id = f"{profile.directory_prefix}_{timestamp}_{uuid.uuid4().hex[:6]}"
            profile_root = self.root / profile.directory_prefix
            profile_root.mkdir(parents=True, exist_ok=True)
            path = profile_root / recording_id
            try:
                plugin_check = subprocess.run(
                    ["ros2", "pkg", "prefix", "rosbag2_storage_mcap"],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=2.0,
                    check=False,
                )
            except (OSError, subprocess.TimeoutExpired) as error:
                raise DevRecordingError(f"无法检查MCAP录制插件：{error}") from error
            if plugin_check.returncode != 0:
                raise DevRecordingError(
                    "未检测到rosbag2_storage_mcap，请安装ros-humble-rosbag2-storage-mcap"
                )

            command: list[str] = [
                "ros2",
                "bag",
                "record",
                "--storage",
                "mcap",
                "--output",
                str(path),
                *profile.topics,
            ]
            try:
                process = subprocess.Popen(
                    command,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    start_new_session=True,
                )
            except OSError as error:
                raise DevRecordingError(f"无法启动ros2 bag record：{error}") from error

            # 启动后短暂确认进程未立即退出，长期录制不保留PIPE，避免日志写满管道阻塞。
            time.sleep(0.15)
            if process.poll() is not None:
                raise DevRecordingError(f"ros2 bag record启动失败：退出状态{process.returncode}")

            self._process = process
            self._profile = profile
            self._recording_id = recording_id
            self._path = path
            self._started_at_ns = time.time_ns()
            self._last_error = None
            if duration_seconds is not None:
                self._duration_timer = threading.Timer(duration_seconds, self._timed_stop)
                self._duration_timer.daemon = True
                self._duration_timer.start()
            self._schedule_watchdog_locked()
            return self._status_locked()

    def stop(self) -> dict[str, object]:
        with self._lock:
            self._reap_locked()
            if self._process is None:
                return self._status_locked()
            self._stop_process_locked()
            return self._status_locked()

    def status(self) -> dict[str, object]:
        with self._lock:
            self._reap_locked()
            return self._status_locked()

    def list_recordings(self) -> list[dict[str, object]]:
        records: list[dict[str, object]] = []
        for prefix, profile in (("raw-cloud", "raw_cloud"), ("diagnostic", "diagnostic")):
            parent = self.root / prefix
            if not parent.exists():
                continue
            for path in sorted((item for item in parent.iterdir() if item.is_dir()), reverse=True):
                if not _RECORDING_ID.fullmatch(path.name):
                    continue
                stat = path.stat()
                records.append({
                    "recording_id": path.name,
                    "profile": profile,
                    "path": str(path),
                    "bytes": _directory_bytes(path),
                    "modified_at_ns": int(stat.st_mtime_ns),
                    "active": path == self._path and self._process is not None,
                })
        records.sort(key=lambda item: int(item["modified_at_ns"]), reverse=True)
        return records

    def delete(self, recording_id: str) -> None:
        if not _RECORDING_ID.fullmatch(recording_id):
            raise DevRecordingError("录制编号无效")
        with self._lock:
            self._reap_locked()
            if self._recording_id == recording_id and self._process is not None:
                raise DevRecordingError("正在录制的文件不能删除")
            matches = list(self.root.glob(f"*/{recording_id}"))
            if len(matches) != 1 or not matches[0].is_dir():
                raise DevRecordingError("未找到指定开发录制")
            path = matches[0].resolve()
            if self.root not in path.parents:
                raise DevRecordingError("录制路径越界")
            shutil.rmtree(path)

    def _timed_stop(self) -> None:
        try:
            self.stop()
        except Exception as error:
            with self._lock:
                self._last_error = str(error)

    def _reap_locked(self) -> None:
        if self._process is None:
            return
        return_code = self._process.poll()
        if return_code is None:
            return
        if return_code != 0 and self._last_error is None:
            self._last_error = f"ros2 bag异常退出：{return_code}"
        self._process = None
        self._cancel_timers_locked()

    def _stop_process_locked(self) -> None:
        assert self._process is not None
        process = self._process
        self._cancel_timers_locked()
        try:
            os.killpg(process.pid, signal.SIGINT)
            process.wait(timeout=10.0)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=3.0)
            except Exception as error:
                self._last_error = f"录制进程未正常退出：{error}"
        except ProcessLookupError:
            pass
        finally:
            self._process = None

    def _schedule_watchdog_locked(self) -> None:
        if self._process is None or self._watchdog_timer is not None:
            return
        self._watchdog_timer = threading.Timer(1.0, self._watchdog_tick)
        self._watchdog_timer.daemon = True
        self._watchdog_timer.start()

    def _watchdog_tick(self) -> None:
        with self._lock:
            self._watchdog_timer = None
            self._reap_locked()
            if self._process is None:
                return
            try:
                free_bytes = shutil.disk_usage(self.root).free
            except OSError as error:
                self._last_error = f"开发录制磁盘状态读取失败：{error}"
                self._stop_process_locked()
                return
            if free_bytes < self.min_free_bytes:
                self._last_error = (
                    f"开发录制已自动停止：剩余空间{free_bytes / 1024**3:.2f} GiB，"
                    f"低于安全下限{self.min_free_bytes / 1024**3:.2f} GiB"
                )
                self._stop_process_locked()
                return
            self._schedule_watchdog_locked()

    def _cancel_timers_locked(self) -> None:
        if self._duration_timer is not None:
            self._duration_timer.cancel()
            self._duration_timer = None
        if self._watchdog_timer is not None:
            self._watchdog_timer.cancel()
            self._watchdog_timer = None

    def _status_locked(self) -> dict[str, object]:
        active = self._process is not None and self._process.poll() is None
        elapsed_seconds = 0.0
        if active and self._started_at_ns is not None:
            elapsed_seconds = max(0.0, (time.time_ns() - self._started_at_ns) / 1e9)
        return {
            "active": active,
            "profile": self._profile.name if self._profile is not None else None,
            "recording_id": self._recording_id,
            "path": str(self._path) if self._path is not None else None,
            "started_at_ns": self._started_at_ns,
            "elapsed_seconds": round(elapsed_seconds, 1),
            "bytes": _directory_bytes(self._path) if self._path is not None else 0,
            "last_error": self._last_error,
            "free_bytes": shutil.disk_usage(self.root).free,
        }
