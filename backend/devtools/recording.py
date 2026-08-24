from __future__ import annotations

import json
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
from typing import Callable


class DevRecordingError(RuntimeError):
    pass


@dataclass(frozen=True, slots=True)
class RecordingGroup:
    key: str
    file_stem: str
    nominal_rate_hz: float | None
    topics: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class RecordingProfile:
    name: str
    directory_prefix: str
    topics: tuple[str, ...]
    allow_continuous: bool = True
    groups: tuple[RecordingGroup, ...] = ()


RAW_CLOUD_GROUPS = (
    RecordingGroup(
        "pointcloud_10hz", "pointcloud_10hz", 10.0,
        ("/capture/lidar/points_raw",),
    ),
    RecordingGroup(
        "radar_state_400hz", "radar_state_400hz", 400.0,
        (
            "/capture/imu/data",
            "/capture/odometry/high_rate_raw",
            "/capture/odometry/slam",
        ),
    ),
    RecordingGroup(
        "rtk_10hz", "rtk_10hz", 10.0,
        ("/capture/rtk/fix", "/capture/rtk/status"),
    ),
    RecordingGroup(
        "algorithm_10hz", "algorithm_10hz", 10.0,
        (
            "/capture/clearance/result",
            "/capture/clearance/raw_diagnostics",
        ),
    ),
    RecordingGroup(
        "events_diagnostics", "events_diagnostics", None,
        (
            "/capture/lidar/device_online",
            "/capture/lidar/device_offline",
            "/capture/task/status",
            "/capture/recording/status",
            "/capture/system/diagnostics",
            "/diagnostics",
        ),
    ),
)


RAW_CLOUD_PROFILE = RecordingProfile(
    name="raw_cloud",
    directory_prefix="raw-cloud",
    # 保留既有profile名和目录前缀，兼容已有样本及离线检测接口。测试页的一次保存
    # 必须形成自包含数据集，不同频率链路分组写入同一会话目录下的独立MCAP。
    topics=(
        "/capture/lidar/points_raw",
        "/capture/imu/data",
        "/capture/odometry/high_rate_raw",
        "/capture/odometry/slam",
        "/capture/rtk/fix",
        "/capture/rtk/status",
        "/capture/clearance/result",
        "/capture/clearance/raw_diagnostics",
        "/capture/lidar/device_online",
        "/capture/lidar/device_offline",
        "/capture/task/status",
        "/capture/recording/status",
        "/capture/system/diagnostics",
        "/diagnostics",
    ),
    groups=RAW_CLOUD_GROUPS,
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
        "/diagnostics",
        "/capture/odometry/high_rate_raw",
    ),
    allow_continuous=False,
)

RAW_SENSOR_PROFILE = RecordingProfile(
    name="raw_sensor",
    directory_prefix="raw-sensor",
    topics=(
        "/capture/lidar/points_raw",
        "/capture/imu/data",
        "/capture/odometry/high_rate_raw",
        "/capture/odometry/slam",
        "/capture/lidar/device_online",
        "/capture/lidar/device_offline",
    ),
)

ALGORITHM_DEBUG_PROFILE = RecordingProfile(
    name="algorithm_debug",
    directory_prefix="algorithm-debug",
    topics=(
        "/capture/lidar/points_raw",
        "/capture/clearance/result",
        "/capture/clearance/raw_diagnostics",
        "/capture/visualization/cloud_preview",
        "/capture/rtk/fix",
        "/capture/rtk/status",
        "/capture/task/status",
        "/capture/recording/status",
        "/capture/system/diagnostics",
        "/diagnostics",
    ),
)

FULL_DEBUG_PROFILE = RecordingProfile(
    name="full_debug",
    directory_prefix="full-debug",
    topics=tuple(dict.fromkeys((*RAW_SENSOR_PROFILE.topics, *ALGORITHM_DEBUG_PROFILE.topics))),
)

PROFILES: tuple[RecordingProfile, ...] = (
    RAW_SENSOR_PROFILE,
    ALGORITHM_DEBUG_PROFILE,
    FULL_DEBUG_PROFILE,
    RAW_CLOUD_PROFILE,
    DIAGNOSTIC_PROFILE,
)
_PROFILE_BY_PREFIX = {profile.directory_prefix: profile for profile in PROFILES}
_RECORDING_ID = re.compile(r"^[a-z0-9-]+_[0-9]{8}_[0-9]{6}_[0-9a-f]{6}$")


def _directory_bytes(path: Path | None) -> int:
    total = 0
    if path is None or not path.exists():
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

    def __init__(
        self,
        data_root: Path,
        min_free_bytes: int = 2 * 1024**3,
        parameter_snapshot_provider: Callable[[], dict[str, object]] | None = None,
    ) -> None:
        self.root = (data_root / "dev-tests").resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.min_free_bytes = min_free_bytes
        self.parameter_snapshot_provider = parameter_snapshot_provider
        self._lock = threading.Lock()
        self._process: subprocess.Popen[bytes] | None = None
        self._extra_processes: dict[str, subprocess.Popen[bytes]] = {}
        self._group_outputs: dict[str, Path] = {}
        self._profile: RecordingProfile | None = None
        self._recording_id: str | None = None
        self._path: Path | None = None
        self._started_at_ns: int | None = None
        self._duration_timer: threading.Timer | None = None
        self._watchdog_timer: threading.Timer | None = None
        self._last_error: str | None = None
        self._snapshot_complete: bool | None = None

    def stop_on_shutdown(self) -> None:
        try:
            self.stop()
        except DevRecordingError:
            pass

    def start(self, profile: RecordingProfile, duration_seconds: int | None = None) -> dict[str, object]:
        if duration_seconds is not None and duration_seconds not in {5, 10, 30}:
            raise DevRecordingError("自动录制时长仅支持5、10或30秒")
        if duration_seconds is None and not profile.allow_continuous:
            raise DevRecordingError("该录制模式仅支持5、10或30秒")
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

            processes: dict[str, subprocess.Popen[bytes]] = {}
            group_outputs: dict[str, Path] = {}
            try:
                if profile.groups:
                    path.mkdir(parents=True, exist_ok=False)
                    staging = path / ".recording"
                    staging.mkdir()
                    for group in profile.groups:
                        output = staging / group.file_stem
                        processes[group.key] = self._spawn_recorder(output, group.topics)
                        group_outputs[group.key] = output
                    primary_key = profile.groups[0].key
                else:
                    processes[profile.name] = self._spawn_recorder(path, profile.topics)
                    group_outputs[profile.name] = path
                    primary_key = profile.name
                self._wait_for_recorder_outputs(processes, group_outputs)
            except Exception:
                for process in processes.values():
                    self._terminate_untracked_process(process)
                if profile.groups and path.is_dir():
                    shutil.rmtree(path, ignore_errors=True)
                raise

            started_at_ns = time.time_ns()
            # 先确认录制进程并发布活动状态，参数快照随后异步写入。
            # 参数读取绝不能阻塞录制按钮或持有录制管理器锁。
            self._process = processes.pop(primary_key)
            self._extra_processes = processes
            self._group_outputs = group_outputs
            self._profile = profile
            self._recording_id = recording_id
            self._path = path
            self._started_at_ns = started_at_ns
            self._last_error = None
            self._snapshot_complete = None
            self._write_capture_metadata(path, recording_id, profile, started_at_ns, None)
            snapshot_thread = threading.Thread(
                target=self._write_snapshot_async,
                args=(path, recording_id, profile, started_at_ns),
                name=f"dev-snapshot-{recording_id}",
                daemon=True,
            )
            snapshot_thread.start()
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
        for prefix, profile in _PROFILE_BY_PREFIX.items():
            parent = self.root / prefix
            if not parent.exists():
                continue
            for path in sorted((item for item in parent.iterdir() if item.is_dir()), reverse=True):
                if not _RECORDING_ID.fullmatch(path.name):
                    continue
                stat = path.stat()
                snapshot_complete: bool | None = None
                snapshot_path = path / "parameter_snapshot.yaml"
                if snapshot_path.is_file():
                    try:
                        snapshot_payload = json.loads(snapshot_path.read_text(encoding="utf-8"))
                        snapshot_complete = bool(snapshot_payload.get("complete", False))
                    except (OSError, json.JSONDecodeError):
                        snapshot_complete = False
                manifest = self._read_manifest(path)
                topics = manifest.get("topics") if isinstance(manifest, dict) else None
                replay_inputs: dict[str, str] = {}
                files = manifest.get("files") if isinstance(manifest, dict) else None
                if manifest.get("layout") == "frequency_split_mcap" and isinstance(files, list):
                    for item in files:
                        if not isinstance(item, dict):
                            continue
                        key = str(item.get("key") or "")
                        file_name = str(item.get("file") or "")
                        candidate = path / file_name
                        if key and candidate.is_file():
                            replay_inputs[key] = str(candidate)
                    replay_ready = (
                        manifest.get("finalized") is True
                        and "pointcloud_10hz" in replay_inputs
                    )
                else:
                    replay_ready = (
                        isinstance(topics, list)
                        and "/capture/lidar/points_raw" in topics
                    )
                    if replay_ready:
                        replay_inputs["combined"] = str(path)
                duration_seconds = manifest.get("duration_seconds") if isinstance(manifest, dict) else None
                records.append({
                    "recording_id": path.name,
                    "profile": profile.name,
                    "path": str(path),
                    "bytes": _directory_bytes(path),
                    "modified_at_ns": int(stat.st_mtime_ns),
                    "active": path == self._path and self._process is not None,
                    "parameter_snapshot_complete": snapshot_complete,
                    "replay_ready": replay_ready,
                    "layout": str(manifest.get("layout") or "combined_mcap"),
                    "replay_inputs": replay_inputs,
                    "file_count": len(replay_inputs),
                    "duration_seconds": float(duration_seconds) if isinstance(duration_seconds, (int, float)) else None,
                })
        records.sort(key=lambda item: int(item["modified_at_ns"]), reverse=True)
        return records

    def get_recording(self, recording_id: str) -> dict[str, object]:
        path = self._resolve_recording_path(recording_id)
        for record in self.list_recordings():
            if record["recording_id"] == recording_id:
                return record
        raise DevRecordingError("未找到指定开发录制")

    def delete(self, recording_id: str) -> None:
        with self._lock:
            self._reap_locked()
            if self._recording_id == recording_id and self._process is not None:
                raise DevRecordingError("正在录制的文件不能删除")
            path = self._resolve_recording_path(recording_id)
            shutil.rmtree(path)

    def _resolve_recording_path(self, recording_id: str) -> Path:
        if not _RECORDING_ID.fullmatch(recording_id):
            raise DevRecordingError("录制编号无效")
        matches = list(self.root.glob(f"*/{recording_id}"))
        if len(matches) != 1 or not matches[0].is_dir():
            raise DevRecordingError("未找到指定开发录制")
        path = matches[0].resolve()
        if self.root not in path.parents:
            raise DevRecordingError("录制路径越界")
        return path

    @staticmethod
    def _read_manifest(path: Path) -> dict[str, object]:
        manifest_path = path / "capture_manifest.json"
        try:
            payload = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return {}
        return payload if isinstance(payload, dict) else {}

    def _write_snapshot_async(
        self, path: Path, recording_id: str, profile: RecordingProfile, started_at_ns: int
    ) -> None:
        try:
            snapshot = self.parameter_snapshot_provider() if self.parameter_snapshot_provider else None
            self._write_capture_metadata(path, recording_id, profile, started_at_ns, snapshot)
            complete = None if snapshot is None else bool(snapshot.get("complete", False))
            with self._lock:
                if self._recording_id == recording_id:
                    self._snapshot_complete = complete
        except Exception as error:
            with self._lock:
                if self._recording_id == recording_id:
                    self._snapshot_complete = False
                    if self._last_error is None:
                        self._last_error = f"参数快照写入失败：{error}"

    @staticmethod
    def _write_capture_metadata(
        path: Path,
        recording_id: str,
        profile: RecordingProfile,
        started_at_ns: int,
        snapshot: dict[str, object] | None,
    ) -> None:
        existing = RosbagRecordingManager._read_manifest(path)
        manifest = {
            "schema_version": 2 if profile.groups else 1,
            "recording_id": recording_id,
            "profile": profile.name,
            "started_at_ns": started_at_ns,
            "storage": "mcap",
            "topics": list(profile.topics),
            "topic_downsampling": False,
            "parameter_snapshot_file": "parameter_snapshot.yaml" if snapshot is not None else None,
        }
        if profile.groups:
            manifest["layout"] = "frequency_split_mcap"
            manifest["files"] = [
                {
                    "key": group.key,
                    "file": f"{group.file_stem}.mcap",
                    "metadata_file": f"{group.file_stem}.metadata.yaml",
                    "nominal_rate_hz": group.nominal_rate_hz,
                    "topics": list(group.topics),
                    "timestamp_semantics": (
                        "MCAP记录时间保留ROS接收时间；含header的消息同时保留传感器时间戳"
                    ),
                }
                for group in profile.groups
            ]
            manifest["finalized"] = bool(existing.get("finalized", False))
        if "stopped_at_ns" in existing:
            manifest["stopped_at_ns"] = existing["stopped_at_ns"]
        if "duration_seconds" in existing:
            manifest["duration_seconds"] = existing["duration_seconds"]
        (path / "capture_manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        if snapshot is None:
            return
        (path / "parameter_snapshot.yaml").write_text(
            json.dumps(snapshot, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        lines: list[str] = []
        binding = snapshot.get("binding_config")
        if isinstance(binding, dict):
            lines.append(f"{binding.get('sha256') or 'MISSING'}  {binding.get('path') or 'binding_config'}")
        source_configs = snapshot.get("source_configs")
        if isinstance(source_configs, list):
            for item in source_configs:
                if isinstance(item, dict):
                    lines.append(f"{item.get('sha256') or 'MISSING'}  {item.get('path') or 'unknown'}")
        (path / "source_config_sha256.txt").write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")

    @staticmethod
    def _spawn_recorder(output: Path, topics: tuple[str, ...]) -> subprocess.Popen[bytes]:
        command = [
            "ros2", "bag", "record", "--storage", "mcap", "--output", str(output),
            *topics,
        ]
        try:
            return subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
        except OSError as error:
            raise DevRecordingError(f"无法启动ros2 bag record：{error}") from error

    @staticmethod
    def _wait_for_recorder_outputs(
        processes: dict[str, subprocess.Popen[bytes]], outputs: dict[str, Path]
    ) -> None:
        time.sleep(0.15)
        for _ in range(20):
            failed = next(
                ((name, process.returncode) for name, process in processes.items() if process.poll() is not None),
                None,
            )
            if failed is not None:
                raise DevRecordingError(
                    f"ros2 bag record分组{failed[0]}启动失败：退出状态{failed[1]}"
                )
            if all(output.is_dir() for output in outputs.values()):
                return
            time.sleep(0.05)
        raise DevRecordingError("ros2 bag record未创建全部分组录制目录")

    @staticmethod
    def _terminate_untracked_process(process: subprocess.Popen[bytes]) -> None:
        try:
            os.killpg(process.pid, signal.SIGINT)
            process.wait(timeout=3.0)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def _timed_stop(self) -> None:
        try:
            self.stop()
        except Exception as error:
            with self._lock:
                self._last_error = str(error)

    def _reap_locked(self) -> None:
        if self._process is None:
            return
        named_processes = {"primary": self._process, **self._extra_processes}
        exited = [(name, process.poll()) for name, process in named_processes.items() if process.poll() is not None]
        if not exited:
            return
        if self._last_error is None:
            name, return_code = exited[0]
            self._last_error = f"ros2 bag分组{name}异常退出：{return_code}"
        for process in named_processes.values():
            if process.poll() is None:
                self._terminate_untracked_process(process)
        self._mark_recording_stopped_locked()
        self._process = None
        self._extra_processes.clear()
        self._cancel_timers_locked()

    def _mark_recording_stopped_locked(self) -> None:
        if self._path is None or self._started_at_ns is None or not self._path.is_dir():
            return
        stopped_at_ns = time.time_ns()
        manifest = self._read_manifest(self._path)
        if self._profile is not None and self._profile.groups:
            manifest["finalized"] = self._finalize_group_files_locked()
        manifest.update({
            "stopped_at_ns": stopped_at_ns,
            "duration_seconds": max(0.0, (stopped_at_ns - self._started_at_ns) / 1e9),
        })
        try:
            (self._path / "capture_manifest.json").write_text(
                json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        except OSError as error:
            if self._last_error is None:
                self._last_error = f"录制停止元数据写入失败：{error}"

    def _finalize_group_files_locked(self) -> bool:
        if self._path is None or self._profile is None:
            return False
        complete = True
        for group in self._profile.groups:
            output = self._group_outputs.get(group.key)
            if output is None or not output.is_dir():
                complete = False
                continue
            mcap_files = sorted(output.glob("*.mcap"))
            metadata = output / "metadata.yaml"
            if len(mcap_files) != 1 or not metadata.is_file():
                complete = False
                continue
            try:
                os.replace(mcap_files[0], self._path / f"{group.file_stem}.mcap")
                os.replace(metadata, self._path / f"{group.file_stem}.metadata.yaml")
            except OSError as error:
                complete = False
                if self._last_error is None:
                    self._last_error = f"分组文件整理失败：{group.key}: {error}"
        staging = self._path / ".recording"
        if complete and staging.is_dir():
            shutil.rmtree(staging, ignore_errors=True)
        if not complete and self._last_error is None:
            self._last_error = "部分分组录制文件未完整生成，样本不可离线回放"
        return complete

    def _stop_process_locked(self) -> None:
        assert self._process is not None
        processes = [self._process, *self._extra_processes.values()]
        self._cancel_timers_locked()
        for process in processes:
            try:
                os.killpg(process.pid, signal.SIGINT)
            except ProcessLookupError:
                pass
        for process in processes:
            try:
                process.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=3.0)
                except Exception as error:
                    self._last_error = f"录制进程未正常退出：{error}"
        self._mark_recording_stopped_locked()
        self._process = None
        self._extra_processes.clear()
        self._group_outputs.clear()

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
            "bytes": _directory_bytes(self._path),
            "last_error": self._last_error,
            "free_bytes": shutil.disk_usage(self.root).free,
            "parameter_snapshot_complete": self._snapshot_complete,
        }
