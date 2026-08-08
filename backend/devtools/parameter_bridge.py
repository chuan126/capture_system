from __future__ import annotations

import queue
import threading
import time
from concurrent.futures import Future, TimeoutError as FutureTimeoutError
from dataclasses import dataclass
from typing import Any


class ParameterBridgeError(RuntimeError):
    pass


@dataclass(slots=True)
class _Request:
    operation: str
    node: str
    names: tuple[str, ...]
    value: object | None
    future: Future[object]


class DevParameterBridge:
    """常驻 rclpy 参数桥。所有参数访问都在独立 ROS 线程中完成。"""

    def __init__(self) -> None:
        self._thread: threading.Thread | None = None
        self._started = threading.Event()
        self._stop_requested = threading.Event()
        self._requests: queue.Queue[_Request] = queue.Queue(maxsize=64)
        self._executor: object | None = None
        self._node: object | None = None
        self._get_clients: dict[str, object] = {}
        self._set_clients: dict[str, object] = {}
        self.error: str | None = None

    @property
    def available(self) -> bool:
        return self.error is None and self._thread is not None and self._thread.is_alive()

    def start(self, timeout_seconds: float = 3.0) -> bool:
        if self._thread is not None:
            return self.available
        self._thread = threading.Thread(target=self._run, name="dev-parameter-ros", daemon=True)
        self._thread.start()
        if not self._started.wait(timeout_seconds):
            self.error = "开发参数ROS桥启动超时"
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

    def get_parameters(self, node: str, names: list[str] | tuple[str, ...], timeout_seconds: float = 1.5) -> dict[str, object]:
        result = self._invoke("get", node, tuple(names), None, timeout_seconds)
        if not isinstance(result, dict):
            raise ParameterBridgeError("ROS参数读取返回格式无效")
        return result

    def set_parameter(self, node: str, name: str, value: object, timeout_seconds: float = 1.5) -> object:
        return self._invoke("set", node, (name,), value, timeout_seconds)

    def _invoke(self, operation: str, node: str, names: tuple[str, ...], value: object | None, timeout_seconds: float) -> object:
        if not self.available:
            raise ParameterBridgeError(self.error or "开发参数ROS桥未启动")
        future: Future[object] = Future()
        request = _Request(operation=operation, node=node, names=names, value=value, future=future)
        try:
            self._requests.put(request, timeout=0.1)
        except queue.Full as error:
            raise ParameterBridgeError("开发参数ROS请求队列已满") from error
        try:
            return future.result(timeout=timeout_seconds + 0.5)
        except FutureTimeoutError as error:
            raise ParameterBridgeError("ROS参数请求超时") from error

    @staticmethod
    def _service_prefix(node: str) -> str:
        stripped = node.strip("/")
        return f"/{stripped}" if stripped else ""

    def _run(self) -> None:
        context = node = executor = None
        try:
            import rclpy
            from rclpy.context import Context
            from rclpy.executors import SingleThreadedExecutor
            from rclpy.node import Node

            context = Context()
            rclpy.init(context=context)
            node = Node("web_dev_parameter_bridge", context=context)
            executor = SingleThreadedExecutor(context=context)
            executor.add_node(node)
            self._node = node
            self._executor = executor
            self._started.set()
            while not self._stop_requested.is_set():
                executor.spin_once(timeout_sec=0.02)
                self._process_one(executor)
        except Exception as error:
            self.error = f"{type(error).__name__}: {error}"
            self._started.set()
        finally:
            self._executor = None
            self._node = None
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

    def _process_one(self, executor: Any) -> None:
        try:
            request = self._requests.get_nowait()
        except queue.Empty:
            return
        if request.future.done():
            return
        try:
            if request.operation == "get":
                request.future.set_result(self._get(request.node, request.names, executor))
            else:
                request.future.set_result(self._set(request.node, request.names[0], request.value, executor))
        except Exception as error:
            if not request.future.done():
                request.future.set_exception(ParameterBridgeError(str(error)))

    def _get_client(self, node_name: str):
        from rcl_interfaces.srv import GetParameters
        client = self._get_clients.get(node_name)
        if client is None:
            client = self._node.create_client(GetParameters, f"{self._service_prefix(node_name)}/get_parameters")
            self._get_clients[node_name] = client
        return client

    def _set_client(self, node_name: str):
        from rcl_interfaces.srv import SetParameters
        client = self._set_clients.get(node_name)
        if client is None:
            client = self._node.create_client(SetParameters, f"{self._service_prefix(node_name)}/set_parameters")
            self._set_clients[node_name] = client
        return client

    @staticmethod
    def _wait_future(future: Any, executor: Any, timeout_seconds: float = 1.0) -> Any:
        deadline = time.monotonic() + timeout_seconds
        while not future.done() and time.monotonic() < deadline:
            executor.spin_once(timeout_sec=0.02)
        if not future.done():
            raise ParameterBridgeError("ROS参数Service响应超时")
        exception = future.exception()
        if exception is not None:
            raise ParameterBridgeError(f"ROS参数Service失败：{exception}")
        return future.result()

    def _get(self, node_name: str, names: tuple[str, ...], executor: Any) -> dict[str, object]:
        from rcl_interfaces.srv import GetParameters
        client = self._get_client(node_name)
        if not client.wait_for_service(timeout_sec=0.5):
            raise ParameterBridgeError(f"ROS图中未发现参数Service：{node_name}")
        request = GetParameters.Request()
        request.names = list(names)
        response = self._wait_future(client.call_async(request), executor)
        return {name: self._parameter_value_to_python(value) for name, value in zip(names, response.values, strict=True)}

    def _set(self, node_name: str, name: str, value: object, executor: Any) -> object:
        from rcl_interfaces.msg import Parameter, ParameterType, ParameterValue
        from rcl_interfaces.srv import SetParameters
        client = self._set_client(node_name)
        if not client.wait_for_service(timeout_sec=0.5):
            raise ParameterBridgeError(f"ROS图中未发现参数Service：{node_name}")
        parameter_value = ParameterValue()
        if isinstance(value, bool):
            parameter_value.type = ParameterType.PARAMETER_BOOL
            parameter_value.bool_value = value
        elif isinstance(value, int):
            parameter_value.type = ParameterType.PARAMETER_INTEGER
            parameter_value.integer_value = value
        elif isinstance(value, float):
            parameter_value.type = ParameterType.PARAMETER_DOUBLE
            parameter_value.double_value = value
        else:
            raise ParameterBridgeError("不支持的参数类型")
        request = SetParameters.Request()
        request.parameters = [Parameter(name=name, value=parameter_value)]
        response = self._wait_future(client.call_async(request), executor)
        if not response.results or not response.results[0].successful:
            reason = response.results[0].reason if response.results else "无返回结果"
            raise ParameterBridgeError(f"参数设置失败：{reason}")
        return value

    @staticmethod
    def _parameter_value_to_python(value: Any) -> object | None:
        from rcl_interfaces.msg import ParameterType
        if value.type == ParameterType.PARAMETER_BOOL:
            return bool(value.bool_value)
        if value.type == ParameterType.PARAMETER_INTEGER:
            return int(value.integer_value)
        if value.type == ParameterType.PARAMETER_DOUBLE:
            return float(value.double_value)
        if value.type == ParameterType.PARAMETER_STRING:
            return str(value.string_value)
        if value.type == ParameterType.PARAMETER_NOT_SET:
            return None
        raise ParameterBridgeError(f"暂不支持的ROS参数类型：{value.type}")
