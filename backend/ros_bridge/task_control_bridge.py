from __future__ import annotations

import queue
import threading
import time
from concurrent.futures import Future, TimeoutError as FutureTimeoutError
from dataclasses import dataclass
from typing import Any, Callable, Literal

from backend.protocols.task_status_v1 import TaskStatusSnapshot, from_ros_message

TaskCommandName = Literal["start", "pause", "resume", "stop", "recover"]


@dataclass(frozen=True, slots=True)
class TaskControlResult:
    command_id: str
    accepted: bool
    task_id: str
    status: str
    operation_phase: str
    status_revision: int
    message: str
    error_code: str | None


@dataclass(slots=True)
class _QueuedCommand:
    command: TaskCommandName
    task_id: str
    command_id: str
    expected_revision: int
    travel_direction: str | None
    lane_side: str | None
    lane: str | None
    lidar_mount_height_m: float | None
    clearance_threshold_m: float | None
    clearance_upper_limit_m: float | None
    future: Future[TaskControlResult]


class TaskControlBridge:
    """FastAPI与ROS 2任务控制Service之间的线程隔离桥。"""

    def __init__(
        self,
        snapshot_sink: Callable[[TaskStatusSnapshot], None],
        *,
        status_topic: str = "/capture/task/status",
        start_service: str = "/capture/task/start",
        pause_service: str = "/capture/task/pause",
        resume_service: str = "/capture/task/resume",
        stop_service: str = "/capture/task/stop",
        recover_service: str = "/capture/task/recover",
    ) -> None:
        self._snapshot_sink = snapshot_sink
        self._status_topic = status_topic
        self._service_names = {
            "start": start_service,
            "pause": pause_service,
            "resume": resume_service,
            "stop": stop_service,
            "recover": recover_service,
        }
        self._thread: threading.Thread | None = None
        self._started = threading.Event()
        self._stop_requested = threading.Event()
        self._commands: queue.Queue[_QueuedCommand] = queue.Queue(maxsize=32)
        self._executor: object | None = None
        self._clients: dict[str, object] = {}
        self._start_request_type: object | None = None
        self._command_request_type: object | None = None
        self._service_ready: dict[str, bool] = {name: False for name in self._service_names}
        self._service_ready_lock = threading.Lock()
        self.error: str | None = None

    @property
    def available(self) -> bool:
        return self.error is None and self._thread is not None and self._thread.is_alive()

    @property
    def service_availability(self) -> dict[str, bool]:
        if not self.available:
            return {name: False for name in self._service_names}
        with self._service_ready_lock:
            return dict(self._service_ready)

    @property
    def control_services_ready(self) -> bool:
        states = self.service_availability
        return bool(states) and all(states.values())

    def is_service_ready(self, command: TaskCommandName) -> bool:
        return self.service_availability.get(command, False)

    def _set_service_availability(self, values: dict[str, bool]) -> None:
        with self._service_ready_lock:
            self._service_ready = {
                name: bool(values.get(name, False))
                for name in self._service_names
            }

    def start(self, timeout_seconds: float = 3.0) -> bool:
        if self._thread is not None:
            return self.error is None
        self._thread = threading.Thread(
            target=self._run,
            name="task-control-ros",
            daemon=True,
        )
        self._thread.start()
        if not self._started.wait(timeout_seconds):
            self.error = "任务控制ROS桥启动超时"
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
            if self._thread.is_alive() and self.error is None:
                self.error = "任务控制ROS桥线程未在限定时间内退出"
        while True:
            try:
                command = self._commands.get_nowait()
            except queue.Empty:
                break
            if not command.future.done():
                command.future.set_exception(RuntimeError("任务控制ROS桥已经停止"))

    def invoke(
        self,
        command: TaskCommandName,
        *,
        task_id: str,
        command_id: str,
        expected_revision: int,
        travel_direction: str | None = None,
        lane_side: str | None = None,
        lane: str | None = None,
        lidar_mount_height_m: float | None = None,
        clearance_threshold_m: float | None = None,
        clearance_upper_limit_m: float | None = None,
        timeout_seconds: float = 15.0,
    ) -> TaskControlResult:
        if self.error is not None or self._thread is None or not self._thread.is_alive():
            raise RuntimeError(self.error or "任务控制ROS桥未启动")
        future: Future[TaskControlResult] = Future()
        queued = _QueuedCommand(
            command=command,
            task_id=task_id,
            command_id=command_id,
            expected_revision=expected_revision,
            travel_direction=travel_direction,
            lane_side=lane_side,
            lane=lane,
            lidar_mount_height_m=lidar_mount_height_m,
            clearance_threshold_m=clearance_threshold_m,
            clearance_upper_limit_m=clearance_upper_limit_m,
            future=future,
        )
        try:
            self._commands.put(queued, timeout=0.2)
        except queue.Full as error:
            raise RuntimeError("任务控制请求队列已满") from error
        try:
            return future.result(timeout=timeout_seconds)
        except FutureTimeoutError as error:
            raise TimeoutError("任务控制ROS请求超时") from error

    def _run(self) -> None:
        context = node = executor = None
        try:
            import rclpy
            from interfaces.msg import TaskStatus
            from interfaces.srv import StartTask, TaskCommand
            from rclpy.context import Context
            from rclpy.executors import SingleThreadedExecutor
            from rclpy.node import Node
            from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy

            context = Context()
            rclpy.init(context=context)
            node = Node("web_task_control_bridge", context=context)
            qos = QoSProfile(
                reliability=ReliabilityPolicy.RELIABLE,
                durability=DurabilityPolicy.TRANSIENT_LOCAL,
                history=HistoryPolicy.KEEP_LAST,
                depth=10,
            )
            node.create_subscription(TaskStatus, self._status_topic, self._on_status, qos)
            self._clients = {
                "start": node.create_client(StartTask, self._service_names["start"]),
                "pause": node.create_client(TaskCommand, self._service_names["pause"]),
                "resume": node.create_client(TaskCommand, self._service_names["resume"]),
                "stop": node.create_client(TaskCommand, self._service_names["stop"]),
                "recover": node.create_client(TaskCommand, self._service_names["recover"]),
            }
            self._start_request_type = StartTask.Request
            self._command_request_type = TaskCommand.Request
            executor = SingleThreadedExecutor(context=context)
            executor.add_node(node)
            self._executor = executor
            self._started.set()

            while not self._stop_requested.is_set():
                executor.spin_once(timeout_sec=0.02)
                self._set_service_availability({
                    name: client.service_is_ready()
                    for name, client in self._clients.items()
                })
                self._process_one_command(executor)
        except Exception as exception:
            self.error = f"{type(exception).__name__}: {exception}"
            self._started.set()
        finally:
            self._set_service_availability({})
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

    def _process_one_command(self, executor: Any) -> None:
        try:
            command = self._commands.get_nowait()
        except queue.Empty:
            return
        if command.future.done():
            return
        try:
            client = self._clients[command.command]
            if not client.wait_for_service(timeout_sec=0.25):
                raise RuntimeError(f"任务控制Service不可用：{self._service_names[command.command]}")
            if command.command == "start":
                request = self._start_request_type()
                request.task_id = command.task_id
                request.command_id = command.command_id
                request.expected_revision = command.expected_revision
                resolved_lane = command.lane_side or command.lane or "right"
                request.lane = resolved_lane
                request.travel_direction = command.travel_direction or ""
                request.lane_side = resolved_lane
                request.lidar_mount_height_m = float(command.lidar_mount_height_m or 0.0)
                request.clearance_threshold_m = float(command.clearance_threshold_m or 0.0)
                request.clearance_upper_limit_m = float(
                    20.0 if command.clearance_upper_limit_m is None else command.clearance_upper_limit_m
                )
            else:
                request = self._command_request_type()
                request.task_id = command.task_id
                request.command_id = command.command_id
                request.expected_revision = command.expected_revision
            response_future = client.call_async(request)
            deadline = time.monotonic() + 14.0
            while not response_future.done() and time.monotonic() < deadline:
                executor.spin_once(timeout_sec=0.02)
            if not response_future.done():
                raise TimeoutError("任务控制Service响应超时")
            response = response_future.result()
            command.future.set_result(
                TaskControlResult(
                    command_id=command.command_id,
                    accepted=bool(response.accepted),
                    task_id=command.task_id,
                    status=str(response.status),
                    operation_phase=str(response.operation_phase),
                    status_revision=int(response.status_revision),
                    message=str(response.message),
                    error_code=str(response.error_code).strip() or None,
                )
            )
        except Exception as error:
            command.future.set_exception(error)

    def _on_status(self, message: object) -> None:
        self._snapshot_sink(from_ros_message(message))
