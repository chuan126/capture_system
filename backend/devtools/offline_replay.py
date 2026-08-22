from __future__ import annotations

import math
import os
import shutil
import signal
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from typing import Callable

from backend.devtools.recording import DevRecordingError, RosbagRecordingManager


class OfflineReplayError(RuntimeError):
    pass


OFFLINE_RAW_CLOUD_TOPIC = "/capture/dev/offline/lidar/points_raw"
OFFLINE_RAW_ODOMETRY_TOPIC = "/capture/dev/offline/odometry/high_rate_raw"
OFFLINE_ODOMETRY_TOPIC = "/capture/dev/offline/odometry/high_rate"
OFFLINE_COMPENSATED_CLOUD_TOPIC = "/capture/dev/offline/lidar/points_compensated_enu"
OFFLINE_CLEARANCE_TOPIC = "/capture/dev/offline/clearance/result"
OFFLINE_DIAGNOSTICS_TOPIC = "/capture/dev/offline/diagnostics"

_SOURCE_RAW_CLOUD_TOPIC = "/capture/lidar/points_raw"
_SOURCE_RAW_ODOMETRY_TOPIC = "/capture/odometry/high_rate_raw"


def _project_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _ros_literal(value: object) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if not math.isfinite(value):
            raise OfflineReplayError("离线参数必须为有限数值")
        # ROS 2 parameter overrides are type-sensitive.  Keep the decimal point
        # for integral-valued floats such as 400.0 so rcl_yaml_param_parser
        # continues to classify them as doubles instead of integers.
        return repr(value)
    if isinstance(value, str):
        return value
    raise OfflineReplayError(f"离线参数类型不支持：{type(value).__name__}")


def _rewrite_parameter_file(source: Path, destination: Path, node_name: str) -> None:
    try:
        lines = source.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise OfflineReplayError(f"离线参数文件读取失败：{source}: {error}") from error
    replaced = False
    output: list[str] = []
    for line in lines:
        stripped = line.strip()
        if not replaced and line == line.lstrip() and stripped and not stripped.startswith("#") and stripped.endswith(":"):
            output.append(f"{node_name}:")
            replaced = True
        else:
            output.append(line)
    if not replaced:
        raise OfflineReplayError(f"离线参数文件缺少节点根键：{source}")
    destination.write_text("\n".join(output) + "\n", encoding="utf-8")


class _OfflineClearanceListener:
    def __init__(
        self,
        callback: Callable[[object], None],
        diagnostics_callback: Callable[[object], None] | None = None,
        topic: str = OFFLINE_CLEARANCE_TOPIC,
        diagnostics_topic: str = OFFLINE_DIAGNOSTICS_TOPIC,
    ) -> None:
        self.callback = callback
        self.diagnostics_callback = diagnostics_callback
        self.topic = topic
        self.diagnostics_topic = diagnostics_topic
        self.error: str | None = None
        self._thread: threading.Thread | None = None
        self._started = threading.Event()
        self._stop_requested = threading.Event()
        self._executor: object | None = None

    def start(self, timeout_seconds: float = 3.0) -> bool:
        if self._thread is not None:
            return self.error is None
        self._thread = threading.Thread(target=self._run, name="dev-offline-clearance-listener", daemon=True)
        self._thread.start()
        if not self._started.wait(timeout_seconds):
            self.error = "离线净空结果ROS监听启动超时"
            return False
        return self.error is None

    def stop(self, timeout_seconds: float = 3.0) -> None:
        self._stop_requested.set()
        executor = self._executor
        if executor is not None:
            try:
                executor.wake()
            except Exception:
                pass
        if self._thread is not None:
            self._thread.join(timeout_seconds)

    def _run(self) -> None:
        context = node = executor = None
        try:
            import rclpy
            from diagnostic_msgs.msg import DiagnosticArray
            from interfaces.msg import ClearanceResult
            from rclpy.context import Context
            from rclpy.executors import SingleThreadedExecutor
            from rclpy.node import Node
            from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy

            context = Context()
            rclpy.init(context=context)
            node = Node("web_dev_offline_clearance_listener", context=context)
            qos = QoSProfile(
                reliability=ReliabilityPolicy.RELIABLE,
                durability=DurabilityPolicy.VOLATILE,
                history=HistoryPolicy.KEEP_LAST,
                depth=10,
            )
            node.create_subscription(ClearanceResult, self.topic, self.callback, qos)
            if self.diagnostics_callback is not None:
                node.create_subscription(
                    DiagnosticArray, self.diagnostics_topic, self.diagnostics_callback, qos
                )
            executor = SingleThreadedExecutor(context=context)
            executor.add_node(node)
            self._executor = executor
            self._started.set()
            while not self._stop_requested.is_set():
                executor.spin_once(timeout_sec=0.05)
        except Exception as error:
            self.error = f"{type(error).__name__}: {error}"
            self._started.set()
        finally:
            self._executor = None
            if executor is not None:
                try:
                    if node is not None:
                        executor.remove_node(node)
                    executor.shutdown(timeout_sec=1.0)
                except Exception:
                    pass
            if node is not None:
                try:
                    node.destroy_node()
                except Exception:
                    pass
            if context is not None:
                try:
                    context.try_shutdown()
                except Exception:
                    pass


class OfflineReplayManager:
    """Run the formal timestamp-adapter -> ENU -> clearance chain on an isolated saved sample."""

    def __init__(
        self,
        recording_manager: RosbagRecordingManager,
        parameter_snapshot_provider: Callable[[], dict[str, object]],
        *,
        project_root: Path | None = None,
        listener_factory: Callable[..., object] | None = None,
        startup_delay_seconds: float = 0.6,
        drain_delay_seconds: float = 1.0,
    ) -> None:
        self.recording_manager = recording_manager
        self.parameter_snapshot_provider = parameter_snapshot_provider
        self.project_root = (project_root or _project_root()).resolve()
        self.listener_factory = listener_factory or (
            lambda callback, diagnostics_callback: _OfflineClearanceListener(
                callback, diagnostics_callback
            )
        )
        self.startup_delay_seconds = max(0.0, startup_delay_seconds)
        self.drain_delay_seconds = max(0.0, drain_delay_seconds)
        self.temp_root = self.recording_manager.root / ".offline"
        self.temp_root.mkdir(parents=True, exist_ok=True)

        self._lock = threading.RLock()
        self._stop_requested = threading.Event()
        self._generation = 0
        self._processes: dict[str, subprocess.Popen[bytes]] = {}
        self._listener: object | None = None
        self._monitor_thread: threading.Thread | None = None
        self._temp_dir: Path | None = None
        self._state = "idle"
        self._recording_id: str | None = None
        self._recording_path: Path | None = None
        self._duration_seconds: float | None = None
        self._started_at_ns: int | None = None
        self._started_monotonic: float | None = None
        self._finished_at_ns: int | None = None
        self._finished_monotonic: float | None = None
        self._last_error: str | None = None
        self._parameter_snapshot_complete: bool | None = None
        self._parameter_fallback_keys: list[str] = []
        self._log_paths: dict[str, Path] = {}
        self._diagnostics: dict[str, int | float | str | None] = {}
        self._reset_statistics_locked()

    def _reset_statistics_locked(self) -> None:
        self._processed_frames = 0
        self._valid_frames = 0
        self._invalid_frames = 0
        self._ransac_plane_total = 0
        self._ransac_plane_max = 0
        self._ransac_plane_last: int | None = None
        self._clearance_total = 0.0
        self._clearance_min: float | None = None
        self._clearance_max: float | None = None
        self._clearance_last: float | None = None
        self._latest_result_valid: bool | None = None
        self._processing_time_last: float | None = None
        self._invalid_reason = ""
        self._latest_stamp_ns: int | None = None
        self._diagnostics = {}

    @property
    def active(self) -> bool:
        with self._lock:
            return self._state in {"starting", "running", "stopping"}

    def uses_recording(self, recording_id: str) -> bool:
        with self._lock:
            return self.active and self._recording_id == recording_id

    def start(self, recording_id: str) -> dict[str, object]:
        with self._lock:
            if self.active:
                raise OfflineReplayError("已有离线算法检测正在运行")
        recording_status = self.recording_manager.status()
        if recording_status.get("active"):
            raise OfflineReplayError("原始点云正在保存，请先停止保存后再进行离线检测")
        try:
            record = self.recording_manager.get_recording(recording_id)
        except DevRecordingError as error:
            raise OfflineReplayError(str(error)) from error
        if record.get("profile") != "raw_cloud":
            raise OfflineReplayError("离线检测只接受测试页保存的原始点云样本")
        if record.get("active"):
            raise OfflineReplayError("正在保存的原始点云样本不能离线检测")
        if not record.get("replay_ready"):
            raise OfflineReplayError(
                "该样本缺少原始高频里程计，无法完整重放运动补偿链；请使用当前版本重新保存样本"
            )
        if shutil.which("ros2") is None:
            raise OfflineReplayError("未找到ros2命令，无法启动离线算法链")

        snapshot = self.parameter_snapshot_provider()
        overrides, fallback_keys = self._runtime_parameter_overrides(snapshot)
        recording_path = Path(str(record["path"])).resolve()
        if self.recording_manager.root not in recording_path.parents:
            raise OfflineReplayError("离线样本路径越界")

        with self._lock:
            if self.active:
                raise OfflineReplayError("已有离线算法检测正在运行")
            self._generation += 1
            generation = self._generation
            self._stop_requested.clear()
            self._state = "starting"
            self._recording_id = recording_id
            self._recording_path = recording_path
            duration = record.get("duration_seconds")
            self._duration_seconds = float(duration) if isinstance(duration, (int, float)) and duration > 0 else None
            self._started_at_ns = time.time_ns()
            self._started_monotonic = time.monotonic()
            self._finished_at_ns = None
            self._finished_monotonic = None
            self._last_error = None
            self._parameter_snapshot_complete = bool(snapshot.get("complete", False))
            self._parameter_fallback_keys = fallback_keys
            self._log_paths = {}
            self._reset_statistics_locked()

        try:
            temp_dir = Path(tempfile.mkdtemp(prefix="run-", dir=self.temp_root))
            with self._lock:
                self._temp_dir = temp_dir
            commands = self._build_commands(recording_path, temp_dir, overrides)
            try:
                listener = self.listener_factory(self._on_result, self._on_diagnostics)
            except TypeError:
                # Test/custom factories written for the older one-callback interface remain compatible.
                listener = self.listener_factory(self._on_result)
            with self._lock:
                self._listener = listener
            if not bool(listener.start()):
                raise OfflineReplayError(getattr(listener, "error", None) or "离线净空结果监听启动失败")

            for name in ("odometry", "motion", "clearance"):
                process = self._spawn(commands[name], temp_dir / f"{name}.log")
                with self._lock:
                    self._processes[name] = process
            if self.startup_delay_seconds:
                time.sleep(self.startup_delay_seconds)
            self._assert_nodes_alive()

            player = self._spawn(commands["player"], temp_dir / "player.log")
            with self._lock:
                self._processes["player"] = player
                self._state = "running"
            monitor = threading.Thread(
                target=self._monitor_player,
                args=(generation, player),
                name=f"dev-offline-monitor-{generation}",
                daemon=True,
            )
            with self._lock:
                self._monitor_thread = monitor
            monitor.start()
            return self.status()
        except Exception as error:
            self._fail_start(error)
            if isinstance(error, OfflineReplayError):
                raise
            raise OfflineReplayError(f"离线算法链启动失败：{error}") from error

    def stop(self) -> dict[str, object]:
        with self._lock:
            if not self.active:
                return self._status_locked()
            self._state = "stopping"
            self._stop_requested.set()
            # Freeze UI elapsed/progress immediately when stop is accepted. Process teardown
            # can take several seconds and must not make the displayed progress keep moving.
            self._finished_at_ns = time.time_ns()
            self._finished_monotonic = time.monotonic()
            processes = list(self._processes.values())
            listener = self._listener
        for process in reversed(processes):
            self._terminate_process(process)
        if listener is not None:
            try:
                listener.stop()
            except Exception:
                pass
        with self._lock:
            self._processes.clear()
            self._listener = None
            self._state = "stopped"
            self._cleanup_temp_locked()
            return self._status_locked()

    def stop_on_shutdown(self) -> None:
        try:
            self.stop()
        except Exception:
            pass

    def status(self) -> dict[str, object]:
        with self._lock:
            return self._status_locked()

    def _status_locked(self) -> dict[str, object]:
        elapsed = 0.0
        if self._started_monotonic is not None:
            end_monotonic = (
                self._finished_monotonic
                if self._finished_monotonic is not None
                else time.monotonic()
            )
            elapsed = max(0.0, end_monotonic - self._started_monotonic)
        if self._state == "completed":
            progress = 1.0
        elif self._duration_seconds and self._duration_seconds > 0:
            progress = min(0.99, max(0.0, elapsed / self._duration_seconds))
        else:
            progress = None
        ransac_mean = (
            self._ransac_plane_total / self._processed_frames if self._processed_frames else None
        )
        clearance_mean = (
            self._clearance_total / self._valid_frames if self._valid_frames else None
        )
        return {
            "active": self._state in {"starting", "running", "stopping"},
            "state": self._state,
            "recording_id": self._recording_id,
            "started_at_ns": self._started_at_ns,
            "finished_at_ns": self._finished_at_ns,
            "elapsed_seconds": round(elapsed, 1),
            "duration_seconds": self._duration_seconds,
            "progress": progress,
            "processed_frames": self._processed_frames,
            "valid_frames": self._valid_frames,
            "invalid_frames": self._invalid_frames,
            "ransac_plane_last": self._ransac_plane_last,
            "ransac_plane_mean": ransac_mean,
            "ransac_plane_max": self._ransac_plane_max if self._processed_frames else None,
            "lidar_to_top_last_m": self._clearance_last,
            "latest_result_valid": self._latest_result_valid,
            "lidar_to_top_min_m": self._clearance_min,
            "lidar_to_top_mean_m": clearance_mean,
            "lidar_to_top_max_m": self._clearance_max,
            "processing_time_ms_last": self._processing_time_last,
            "invalid_reason": self._invalid_reason,
            "latest_stamp_ns": self._latest_stamp_ns,
            "last_error": self._last_error,
            "parameter_snapshot_complete": self._parameter_snapshot_complete,
            "parameter_fallback_keys": list(self._parameter_fallback_keys),
            "diagnostics": dict(self._diagnostics),
            "topics": {
                "raw_cloud": OFFLINE_RAW_CLOUD_TOPIC,
                "raw_odometry": OFFLINE_RAW_ODOMETRY_TOPIC,
                "odometry": OFFLINE_ODOMETRY_TOPIC,
                "compensated_cloud": OFFLINE_COMPENSATED_CLOUD_TOPIC,
                "clearance": OFFLINE_CLEARANCE_TOPIC,
            },
        }

    def _runtime_parameter_overrides(
        self, snapshot: dict[str, object]
    ) -> tuple[dict[str, dict[str, object]], list[str]]:
        overrides: dict[str, dict[str, object]] = {
            "/odometry_timestamp_adapter_node": {},
            "/enu_cloud_transform_node": {},
            "/clearance_engine_node": {},
        }
        fallback_keys: list[str] = []
        parameters = snapshot.get("parameters")
        if not isinstance(parameters, list):
            return overrides, ["parameter_snapshot"]
        for item in parameters:
            if not isinstance(item, dict):
                continue
            node = item.get("node")
            if node not in overrides:
                continue
            key = str(item.get("key") or item.get("parameter") or "unknown")
            if item.get("available") is True and isinstance(item.get("parameter"), str):
                value = item.get("value")
                if isinstance(value, (bool, int, float, str)):
                    overrides[str(node)][str(item["parameter"])] = value
                    continue
            fallback_keys.append(key)
        return overrides, fallback_keys

    def _build_commands(
        self,
        recording_path: Path,
        temp_dir: Path,
        overrides: dict[str, dict[str, object]],
    ) -> dict[str, list[str]]:
        source_configs = {
            "odometry": self.project_root / "ros2_ws/src/motion_compensation/config/odometry_timestamp_adapter.yaml",
            "motion": self.project_root / "ros2_ws/src/motion_compensation/config/motion_compensation.yaml",
            "clearance": self.project_root / "ros2_ws/src/clearance_engine/config/clearance_engine_tunnel_4cm.yaml",
        }
        node_names = {
            "odometry": "offline_odometry_timestamp_adapter_node",
            "motion": "offline_enu_cloud_transform_node",
            "clearance": "offline_clearance_engine_node",
        }
        param_files: dict[str, Path] = {}
        for key, source in source_configs.items():
            destination = temp_dir / f"{key}.yaml"
            _rewrite_parameter_file(source.resolve(), destination, node_names[key])
            param_files[key] = destination

        odom_overrides = dict(overrides.get("/odometry_timestamp_adapter_node", {}))
        odom_overrides.update({
            "input_topic": OFFLINE_RAW_ODOMETRY_TOPIC,
            "output_topic": OFFLINE_ODOMETRY_TOPIC,
        })
        motion_overrides = dict(overrides.get("/enu_cloud_transform_node", {}))
        motion_overrides.update({
            "input_cloud_topic": OFFLINE_RAW_CLOUD_TOPIC,
            "odometry_topic": OFFLINE_ODOMETRY_TOPIC,
            "output_cloud_topic": OFFLINE_COMPENSATED_CLOUD_TOPIC,
            "diagnostics_topic": OFFLINE_DIAGNOSTICS_TOPIC,
        })
        clearance_overrides = dict(overrides.get("/clearance_engine_node", {}))
        clearance_overrides.update({
            "input_topic": OFFLINE_COMPENSATED_CLOUD_TOPIC,
            "output_topic": OFFLINE_CLEARANCE_TOPIC,
        })

        commands = {
            "odometry": self._node_command(
                "motion_compensation", "odometry_timestamp_adapter_node",
                node_names["odometry"], param_files["odometry"], odom_overrides,
            ),
            "motion": self._node_command(
                "motion_compensation", "enu_cloud_transform_node",
                node_names["motion"], param_files["motion"], motion_overrides,
            ),
            "clearance": self._node_command(
                "clearance_engine", "clearance_engine_node",
                node_names["clearance"], param_files["clearance"], clearance_overrides,
            ),
            "player": [
                "ros2", "bag", "play", "-s", "mcap", str(recording_path),
                # 综合测试样本还含RTK、IMU和诊断。离线算法只重放两个原始输入，
                # 禁止其余录制Topic回放到正式命名空间干扰实时监视。
                "--disable-keyboard-controls", "-d", "1.0", "--topics",
                _SOURCE_RAW_CLOUD_TOPIC, _SOURCE_RAW_ODOMETRY_TOPIC, "--remap",
                f"{_SOURCE_RAW_CLOUD_TOPIC}:={OFFLINE_RAW_CLOUD_TOPIC}",
                f"{_SOURCE_RAW_ODOMETRY_TOPIC}:={OFFLINE_RAW_ODOMETRY_TOPIC}",
            ],
        }
        return commands

    @staticmethod
    def _node_command(
        package: str,
        executable: str,
        node_name: str,
        params_file: Path,
        overrides: dict[str, object],
    ) -> list[str]:
        command = [
            "ros2", "run", package, executable, "--ros-args",
            "-r", f"__node:={node_name}", "--params-file", str(params_file),
        ]
        for name, value in overrides.items():
            command.extend(["-p", f"{name}:={_ros_literal(value)}"])
        return command

    def _spawn(self, command: list[str], log_path: Path) -> subprocess.Popen[bytes]:
        try:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_stream = log_path.open("ab", buffering=0)
            try:
                process = subprocess.Popen(
                    command,
                    stdin=subprocess.DEVNULL,
                    stdout=log_stream,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
            finally:
                log_stream.close()
            with self._lock:
                self._log_paths[log_path.stem] = log_path
            return process
        except OSError as error:
            raise OfflineReplayError(f"无法启动离线ROS进程：{error}") from error

    def _assert_nodes_alive(self) -> None:
        with self._lock:
            items = [(name, process) for name, process in self._processes.items() if name != "player"]
        for name, process in items:
            return_code = process.poll()
            if return_code is not None:
                raise OfflineReplayError(f"离线{name}节点启动失败：退出状态{return_code}")

    def _monitor_player(self, generation: int, player: subprocess.Popen[bytes]) -> None:
        try:
            while True:
                if self._stop_requested.is_set():
                    return
                failure = self._poll_algorithm_failure()
                if failure is not None:
                    self._complete_generation(generation, failed=failure)
                    return
                return_code = player.poll()
                if return_code is not None:
                    if return_code != 0:
                        self._complete_generation(
                            generation,
                            failed=self._failure_with_log(
                                "player", f"离线rosbag播放失败：退出状态{return_code}"
                            ),
                        )
                        return
                    break
                time.sleep(0.1)

            deadline = time.monotonic() + self.drain_delay_seconds
            while time.monotonic() < deadline:
                if self._stop_requested.is_set():
                    return
                failure = self._poll_algorithm_failure()
                if failure is not None:
                    self._complete_generation(generation, failed=failure)
                    return
                time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
            if self._stop_requested.is_set():
                return
            self._complete_generation(generation, failed=None)
        except Exception as error:
            self._complete_generation(generation, failed=f"离线进程监督失败：{error}")

    def _poll_algorithm_failure(self) -> str | None:
        with self._lock:
            items = [
                (name, process)
                for name, process in self._processes.items()
                if name in {"odometry", "motion", "clearance"}
            ]
        for name, process in items:
            return_code = process.poll()
            if return_code is not None:
                return self._failure_with_log(
                    name, f"离线{name}节点提前退出：退出状态{return_code}"
                )
        return None

    def _failure_with_log(self, name: str, summary: str) -> str:
        with self._lock:
            path = self._log_paths.get(name)
        tail = self._read_log_tail(path) if path is not None else ""
        if not tail:
            return summary
        return f"{summary}；日志末尾：{tail}"

    @staticmethod
    def _read_log_tail(path: Path, max_bytes: int = 8192, max_lines: int = 16) -> str:
        try:
            with path.open("rb") as stream:
                stream.seek(0, os.SEEK_END)
                size = stream.tell()
                stream.seek(max(0, size - max_bytes), os.SEEK_SET)
                text = stream.read().decode("utf-8", errors="replace")
        except OSError:
            return ""
        lines = [line.strip() for line in text.splitlines() if line.strip()]
        return " | ".join(lines[-max_lines:])[-3000:]

    def _complete_generation(self, generation: int, failed: str | None) -> None:
        with self._lock:
            if generation != self._generation:
                return
            named_processes = [(name, process) for name, process in self._processes.items() if name != "player"]
            if failed is None:
                exited = [(name, process.poll()) for name, process in named_processes if process.poll() is not None]
                if exited:
                    name, code = exited[0]
                    failed = self._failure_with_log(name, f"离线{name}节点提前退出：退出状态{code}")
                elif self._processed_frames == 0:
                    failed = "离线播放结束但未收到净空结果，请检查离线Topic、样本完整性和算法节点状态"
            processes = list(self._processes.values())
            listener = self._listener
        for process in reversed(processes):
            self._terminate_process(process)
        if listener is not None:
            try:
                listener.stop()
            except Exception:
                pass
        with self._lock:
            if generation != self._generation:
                return
            self._processes.clear()
            self._listener = None
            self._finished_at_ns = time.time_ns()
            self._finished_monotonic = time.monotonic()
            if failed:
                self._state = "failed"
                self._last_error = failed
            else:
                self._state = "completed"
            self._cleanup_temp_locked()

    def _fail_start(self, error: Exception) -> None:
        with self._lock:
            processes = list(self._processes.values())
            listener = self._listener
            self._stop_requested.set()
        for process in reversed(processes):
            self._terminate_process(process)
        if listener is not None:
            try:
                listener.stop()
            except Exception:
                pass
        with self._lock:
            self._processes.clear()
            self._listener = None
            self._state = "failed"
            self._finished_at_ns = time.time_ns()
            self._finished_monotonic = time.monotonic()
            self._last_error = str(error)
            self._cleanup_temp_locked()

    @staticmethod
    def _terminate_process(process: subprocess.Popen[bytes]) -> None:
        if process.poll() is not None:
            return
        try:
            os.killpg(process.pid, signal.SIGINT)
            process.wait(timeout=3.0)
            return
        except (ProcessLookupError, subprocess.TimeoutExpired):
            pass
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=2.0)
        except (ProcessLookupError, subprocess.TimeoutExpired, OSError):
            pass

    def _cleanup_temp_locked(self) -> None:
        path = self._temp_dir
        self._temp_dir = None
        if path is not None and path.is_dir() and self.temp_root in path.parents:
            shutil.rmtree(path, ignore_errors=True)

    def _on_result(self, message: object) -> None:
        header = getattr(message, "header", None)
        stamp = getattr(header, "stamp", None)
        stamp_ns = None
        if stamp is not None:
            try:
                stamp_ns = int(getattr(stamp, "sec")) * 1_000_000_000 + int(getattr(stamp, "nanosec"))
            except (TypeError, ValueError):
                stamp_ns = None
        ransac_planes = max(0, int(getattr(message, "ransac_plane_count", 0)))
        valid = bool(getattr(message, "valid", False))
        clearance_raw = getattr(message, "lidar_to_top_m", None)
        clearance = None
        try:
            candidate = float(clearance_raw)
            clearance = candidate if math.isfinite(candidate) else None
        except (TypeError, ValueError):
            pass
        processing_raw = getattr(message, "processing_time_ms", None)
        processing = None
        try:
            candidate = float(processing_raw)
            processing = candidate if math.isfinite(candidate) else None
        except (TypeError, ValueError):
            pass

        with self._lock:
            if self._state not in {"starting", "running"}:
                return
            self._processed_frames += 1
            self._ransac_plane_last = ransac_planes
            self._ransac_plane_total += ransac_planes
            self._ransac_plane_max = max(self._ransac_plane_max, ransac_planes)
            self._processing_time_last = processing
            self._invalid_reason = str(getattr(message, "invalid_reason", ""))
            self._latest_stamp_ns = stamp_ns
            if valid and clearance is not None:
                self._valid_frames += 1
                self._latest_result_valid = True
                self._clearance_last = clearance
                self._clearance_total += clearance
                self._clearance_min = clearance if self._clearance_min is None else min(self._clearance_min, clearance)
                self._clearance_max = clearance if self._clearance_max is None else max(self._clearance_max, clearance)
            else:
                self._invalid_frames += 1
                self._latest_result_valid = False

    def _on_diagnostics(self, message: object) -> None:
        statuses = getattr(message, "status", None)
        if not isinstance(statuses, (list, tuple)) or not statuses:
            return
        selected = statuses[0]
        values = getattr(selected, "values", None)
        if not isinstance(values, (list, tuple)):
            return
        parsed: dict[str, int | float | str | None] = {}
        integer_keys = {
            "pending_cloud_count",
            "pending_cloud_max_count",
            "clouds_received_total",
            "clouds_processed_total",
            "clouds_dropped_total",
            "pose_wait_count",
            "interpolation_failure_count",
        }
        float_keys = {
            "queue_wait_ms_last",
            "queue_wait_ms_mean",
            "queue_wait_ms_max",
            "processing_time_ms_last",
            "processing_time_ms_mean",
            "processing_time_ms_max",
        }
        for item in values:
            key = str(getattr(item, "key", ""))
            raw = str(getattr(item, "value", ""))
            if key in integer_keys:
                try:
                    parsed[key] = int(raw)
                except ValueError:
                    parsed[key] = raw
            elif key in float_keys:
                try:
                    parsed[key] = float(raw)
                except ValueError:
                    parsed[key] = raw
        if not parsed:
            return
        with self._lock:
            if self._state not in {"starting", "running"}:
                return
            self._diagnostics.update(parsed)
