from __future__ import annotations

import asyncio
import time
from urllib.parse import urlsplit

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from backend.websocket.clearance_hub import (
    ClearanceClientLimitReachedError,
    ClearanceHub,
)
from backend.websocket.cloud_preview_hub import (
    ClientLimitReachedError,
    CloudPreviewHub,
)
from backend.websocket.rtk_hub import (
    RtkClientLimitReachedError,
    RtkHub,
)
from backend.websocket.system_status_hub import (
    SystemStatusClientLimitReachedError,
    SystemStatusHub,
)
from backend.websocket.task_status_hub import (
    TaskStatusClientLimitReachedError,
    TaskStatusHub,
)

router = APIRouter()


def _is_same_origin(websocket: WebSocket) -> bool:
    origin = websocket.headers.get("origin")
    host = websocket.headers.get("host")
    if not origin or not host:
        return False

    parsed_origin = urlsplit(origin)
    return parsed_origin.scheme in {"http", "https"} and parsed_origin.netloc == host


async def _ensure_cloud_preview_bridge(websocket: WebSocket, hub: CloudPreviewHub) -> None:
    factory = getattr(websocket.app.state, "cloud_preview_bridge_factory", None)
    lock = getattr(websocket.app.state, "cloud_preview_bridge_lock", None)
    if factory is None or lock is None:
        return

    async with lock:
        bridge = websocket.app.state.cloud_preview_bridge
        if bridge is not None:
            return
        hub.clear_latest_frame()
        try:
            bridge = factory()
            websocket.app.state.cloud_preview_bridge = bridge
            started = await asyncio.to_thread(bridge.start)
            hub.set_ros_availability(started, bridge.error)
        except Exception as exception:
            websocket.app.state.cloud_preview_bridge = None
            hub.set_ros_availability(
                False,
                f"{type(exception).__name__}: {exception}",
            )


async def _release_cloud_preview_bridge(websocket: WebSocket, hub: CloudPreviewHub) -> None:
    lock = getattr(websocket.app.state, "cloud_preview_bridge_lock", None)
    if lock is None or hub.client_count != 0:
        return

    async with lock:
        if hub.client_count != 0:
            return
        bridge = websocket.app.state.cloud_preview_bridge
        websocket.app.state.cloud_preview_bridge = None
        stop_error: str | None = None
        if bridge is not None:
            try:
                await asyncio.to_thread(bridge.stop)
            except Exception as exception:
                stop_error = f"{type(exception).__name__}: {exception}"
        if hub.client_count == 0:
            hub.clear_latest_frame()
            hub.set_ros_availability(False, stop_error or "等待点云预览客户端")


@router.websocket("/ws/v1/cloud-preview")
async def cloud_preview_socket(websocket: WebSocket) -> None:
    await websocket.accept()

    if not _is_same_origin(websocket):
        await websocket.close(code=1008, reason="仅允许同源浏览器连接")
        return

    hub: CloudPreviewHub = websocket.app.state.cloud_preview_hub
    try:
        session = hub.register()
    except ClientLimitReachedError:
        await websocket.send_json(
            {
                "type": "status",
                "state": "degraded",
                "reason": "CLIENT_LIMIT_REACHED",
                "detail": "点云预览最多允许四个浏览器客户端",
            }
        )
        await websocket.close(code=1013, reason="点云客户端数量已达到上限")
        return

    await _ensure_cloud_preview_bridge(websocket, hub)

    last_stream_key: tuple[str, int] | None = None
    last_status_state: str | None = None
    last_status_sent = 0.0
    consecutive_timeouts = 0
    disconnect_task = asyncio.create_task(websocket.receive())

    try:
        while True:
            if disconnect_task.done():
                message = disconnect_task.result()
                if message.get("type") == "websocket.disconnect":
                    return
                disconnect_task = asyncio.create_task(websocket.receive())

            now = time.monotonic()
            status = hub.current_status()
            if (
                status["state"] != last_status_state
                or now - last_status_sent >= 5.0
            ):
                await websocket.send_json(status)
                last_status_state = status["state"]
                last_status_sent = now

            try:
                frame = await asyncio.wait_for(session.queue.get(), timeout=0.25)
            except asyncio.TimeoutError:
                continue

            if frame.stream_key != last_stream_key:
                await websocket.send_json(frame.stream_info())
                last_stream_key = frame.stream_key

            try:
                await asyncio.wait_for(websocket.send_bytes(frame.binary), timeout=0.5)
                consecutive_timeouts = 0
            except asyncio.TimeoutError:
                consecutive_timeouts += 1
                if consecutive_timeouts >= 3:
                    await websocket.close(code=1013, reason="点云客户端发送持续超时")
                    return
    except WebSocketDisconnect:
        pass
    except RuntimeError:
        # Starlette在连接已经关闭时可能以RuntimeError报告，清理路径与正常断开相同。
        pass
    finally:
        disconnect_task.cancel()
        try:
            await disconnect_task
        except asyncio.CancelledError:
            pass
        hub.unregister(session)
        await _release_cloud_preview_bridge(websocket, hub)


@router.websocket("/ws/v1/rtk")
async def rtk_socket(websocket: WebSocket) -> None:
    await websocket.accept()

    if not _is_same_origin(websocket):
        await websocket.close(code=1008, reason="仅允许同源浏览器连接")
        return

    hub: RtkHub = websocket.app.state.rtk_hub
    try:
        session = hub.register()
    except RtkClientLimitReachedError:
        await websocket.send_json(
            {
                "type": "status",
                "state": "degraded",
                "reason": "CLIENT_LIMIT_REACHED",
                "detail": "RTK状态最多允许四个浏览器客户端",
            }
        )
        await websocket.close(code=1013, reason="RTK客户端数量已达到上限")
        return

    last_status_state: str | None = None
    last_status_sent = 0.0
    next_snapshot_sent = 0.0

    try:
        while True:
            now = time.monotonic()
            status = hub.current_status()
            if status["state"] != last_status_state or now - last_status_sent >= 5.0:
                await websocket.send_json(status)
                last_status_state = status["state"]
                last_status_sent = now

            try:
                snapshot = await asyncio.wait_for(session.queue.get(), timeout=0.25)
            except asyncio.TimeoutError:
                continue

            delay = next_snapshot_sent - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            while not session.queue.empty():
                snapshot = session.queue.get_nowait()

            await asyncio.wait_for(
                websocket.send_json(snapshot.to_message()),
                timeout=0.5,
            )
            next_snapshot_sent = time.monotonic() + 0.2
    except WebSocketDisconnect:
        pass
    except (asyncio.TimeoutError, RuntimeError):
        pass
    finally:
        hub.unregister(session)


@router.websocket("/ws/v1/system-status")
async def system_status_socket(websocket: WebSocket) -> None:
    await websocket.accept()
    if not _is_same_origin(websocket):
        await websocket.close(code=1008, reason="仅允许同源浏览器连接")
        return

    hub: SystemStatusHub = websocket.app.state.system_status_hub
    try:
        session = hub.register()
    except SystemStatusClientLimitReachedError:
        await websocket.send_json({"type": "status", "state": "degraded", "reason": "CLIENT_LIMIT_REACHED", "detail": "系统状态最多允许四个浏览器客户端"})
        await websocket.close(code=1013, reason="系统状态客户端数量已达到上限")
        return

    last_state: str | None = None
    last_status_sent = 0.0
    try:
        while True:
            now = time.monotonic()
            status = hub.current_status()
            if status["state"] != last_state or now - last_status_sent >= 1.0:
                await websocket.send_json(status)
                last_state = status["state"]
                last_status_sent = now
            try:
                snapshot = await asyncio.wait_for(session.queue.get(), timeout=0.25)
            except asyncio.TimeoutError:
                continue
            while not session.queue.empty():
                snapshot = session.queue.get_nowait()
            await asyncio.wait_for(websocket.send_json(snapshot.to_message()), timeout=0.5)
    except WebSocketDisconnect:
        pass
    except (asyncio.TimeoutError, RuntimeError):
        pass
    finally:
        hub.unregister(session)


@router.websocket("/ws/v1/clearance")
async def clearance_socket(websocket: WebSocket) -> None:
    await websocket.accept()
    if not _is_same_origin(websocket):
        await websocket.close(code=1008, reason="仅允许同源浏览器连接")
        return

    hub: ClearanceHub = websocket.app.state.clearance_hub
    try:
        session = hub.register()
    except ClearanceClientLimitReachedError:
        await websocket.send_json(
            {
                "type": "status",
                "state": "degraded",
                "reason": "CLIENT_LIMIT_REACHED",
                "detail": "净空结果最多允许四个浏览器客户端",
            }
        )
        await websocket.close(code=1013, reason="净空结果客户端数量已达到上限")
        return

    last_state: str | None = None
    last_status_sent = 0.0
    next_snapshot_sent = 0.0
    try:
        while True:
            now = time.monotonic()
            status = hub.current_status()
            if status["state"] != last_state or now - last_status_sent >= 1.0:
                await websocket.send_json(status)
                last_state = status["state"]
                last_status_sent = now
            try:
                snapshot = await asyncio.wait_for(session.queue.get(), timeout=0.1)
            except asyncio.TimeoutError:
                continue

            delay = next_snapshot_sent - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            while not session.queue.empty():
                snapshot = session.queue.get_nowait()
            await asyncio.wait_for(websocket.send_json(snapshot.to_message()), timeout=0.5)
            next_snapshot_sent = time.monotonic() + 0.1
    except WebSocketDisconnect:
        pass
    except (asyncio.TimeoutError, RuntimeError):
        pass
    finally:
        hub.unregister(session)


@router.websocket("/ws/v1/task-status")
async def task_status_socket(websocket: WebSocket) -> None:
    await websocket.accept()
    if not _is_same_origin(websocket):
        await websocket.close(code=1008, reason="仅允许同源浏览器连接")
        return

    hub: TaskStatusHub = websocket.app.state.task_status_hub
    try:
        session = hub.register()
    except TaskStatusClientLimitReachedError:
        await websocket.send_json(
            {
                "type": "status",
                "state": "degraded",
                "reason": "CLIENT_LIMIT_REACHED",
                "detail": "任务状态最多允许四个浏览器客户端",
            }
        )
        await websocket.close(code=1013, reason="任务状态客户端数量已达到上限")
        return

    last_state: str | None = None
    last_status_sent = 0.0
    try:
        while True:
            now = time.monotonic()
            status_message = hub.current_status()
            if status_message["state"] != last_state or now - last_status_sent >= 1.0:
                await websocket.send_json(status_message)
                last_state = status_message["state"]
                last_status_sent = now
            try:
                snapshot = await asyncio.wait_for(session.queue.get(), timeout=0.25)
            except asyncio.TimeoutError:
                continue
            while not session.queue.empty():
                snapshot = session.queue.get_nowait()
            await asyncio.wait_for(websocket.send_json(snapshot.to_message()), timeout=0.5)
    except WebSocketDisconnect:
        pass
    except (asyncio.TimeoutError, RuntimeError):
        pass
    finally:
        hub.unregister(session)
