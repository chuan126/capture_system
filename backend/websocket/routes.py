from __future__ import annotations

import asyncio
import time
from urllib.parse import urlsplit

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from backend.websocket.cloud_preview_hub import (
    ClientLimitReachedError,
    CloudPreviewHub,
)

router = APIRouter()


def _is_same_origin(websocket: WebSocket) -> bool:
    origin = websocket.headers.get("origin")
    host = websocket.headers.get("host")
    if not origin or not host:
        return False

    parsed_origin = urlsplit(origin)
    return parsed_origin.scheme in {"http", "https"} and parsed_origin.netloc == host


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
                "detail": "点云预览最多允许两个浏览器客户端",
            }
        )
        await websocket.close(code=1013, reason="点云客户端数量已达到上限")
        return

    last_stream_key: tuple[str, int] | None = None
    last_status_state: str | None = None
    last_status_sent = 0.0
    consecutive_timeouts = 0

    try:
        while True:
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
        hub.unregister(session)
