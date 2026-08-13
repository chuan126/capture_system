from __future__ import annotations

import asyncio
import shutil
import time
from urllib.parse import urlsplit
from pathlib import Path

from fastapi import APIRouter, HTTPException, Request, WebSocket, WebSocketDisconnect, status
from pydantic import BaseModel, Field

from backend.devtools.parameters import DevParameterError, DevParameterService
from backend.devtools.recording import (
    ALGORITHM_DEBUG_PROFILE,
    DIAGNOSTIC_PROFILE,
    FULL_DEBUG_PROFILE,
    RAW_CLOUD_PROFILE,
    RAW_SENSOR_PROFILE,
    DevRecordingError,
    RosbagRecordingManager,
)
from backend.tasks.control_routes import task_control_readiness
from backend.websocket.cloud_preview_hub import ClientLimitReachedError, CloudPreviewHub


class RecordingStartRequest(BaseModel):
    duration_seconds: int | None = Field(default=None)


class ParameterSetRequest(BaseModel):
    value: bool | int | float


async def _ensure_dev_raw_cloud_bridge(websocket: WebSocket, hub: CloudPreviewHub) -> None:
    factory = getattr(websocket.app.state, "dev_raw_cloud_bridge_factory", None)
    lock = getattr(websocket.app.state, "dev_raw_cloud_bridge_lock", None)
    if factory is None or lock is None:
        return

    async with lock:
        bridge = websocket.app.state.dev_raw_cloud_bridge
        if bridge is not None:
            return
        hub.clear_latest_frame()
        try:
            bridge = factory()
            websocket.app.state.dev_raw_cloud_bridge = bridge
            started = await asyncio.to_thread(bridge.start)
            hub.set_ros_availability(started, bridge.error)
        except Exception as exception:
            websocket.app.state.dev_raw_cloud_bridge = None
            hub.set_ros_availability(
                False,
                f"{type(exception).__name__}: {exception}",
            )


async def _release_dev_raw_cloud_bridge(websocket: WebSocket, hub: CloudPreviewHub) -> None:
    lock = getattr(websocket.app.state, "dev_raw_cloud_bridge_lock", None)
    if lock is None or hub.client_count != 0:
        return

    async with lock:
        if hub.client_count != 0:
            return
        bridge = websocket.app.state.dev_raw_cloud_bridge
        websocket.app.state.dev_raw_cloud_bridge = None
        stop_error: str | None = None
        if bridge is not None:
            try:
                await asyncio.to_thread(bridge.stop)
            except Exception as exception:
                stop_error = f"{type(exception).__name__}: {exception}"
        if hub.client_count == 0:
            hub.clear_latest_frame()
            hub.set_ros_availability(False, stop_error or "等待开发点云预览客户端")


def create_devtools_router() -> APIRouter:
    router = APIRouter(prefix="/api/dev", tags=["development-tools"])

    @router.get("/overview")
    def overview(request: Request) -> dict[str, object]:
        telemetry_bridge = request.app.state.dev_telemetry_bridge
        if request.app.state.dev_ros_bridge_enabled:
            telemetry_bridge.touch()
        telemetry = telemetry_bridge.snapshot()
        data_root: Path = request.app.state.runtime_data_root
        usage = shutil.disk_usage(data_root)
        return {
            "type": "dev_overview",
            "build_variant": "development",
            "version": request.app.state.capture_version,
            "emitted_at_ns": time.time_ns(),
            "data_root": str(data_root),
            "storage": {
                "total_bytes": usage.total,
                "used_bytes": usage.used,
                "free_bytes": usage.free,
            },
            "system": request.app.state.dev_system_metrics.snapshot(),
            "telemetry": telemetry,
        }

    @router.get("/task-control")
    def dev_task_control(request: Request) -> dict[str, object]:
        return task_control_readiness(request).model_dump()

    @router.get("/rtk/snapshot")
    def rtk_snapshot(request: Request) -> dict[str, object]:
        snapshot = request.app.state.rtk_hub.latest_snapshot
        if snapshot is None:
            return {
                "confirmed": False,
                "detail": "RTK坐标未确认",
                "captured_at_ns": time.time_ns(),
                "snapshot": None,
            }
        confirmed = (
            snapshot.fix_status is not None
            and snapshot.fix_status >= 0
            and snapshot.latitude is not None
            and snapshot.longitude is not None
        )
        return {
            "confirmed": confirmed,
            "detail": "RTK坐标已记录" if confirmed else "RTK坐标未确认",
            "captured_at_ns": time.time_ns(),
            "snapshot": snapshot.to_message(),
        }

    @router.get("/parameters")
    def parameters(request: Request) -> dict[str, object]:
        service: DevParameterService = request.app.state.dev_parameter_service
        return {"parameters": service.list_parameters(ui_only=True)}

    @router.put("/parameters/{key:path}")
    def set_parameter(key: str, payload: ParameterSetRequest, request: Request) -> dict[str, object]:
        _ensure_no_formal_task_active(request)
        service: DevParameterService = request.app.state.dev_parameter_service
        try:
            return service.set_parameter(key, payload.value)
        except DevParameterError as error:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error

    def _recording_manager(request: Request) -> RosbagRecordingManager:
        return request.app.state.dev_recording_manager

    def _ensure_no_formal_task_active(request: Request) -> None:
        readiness = task_control_readiness(request)
        if readiness.active_task_id is not None:
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail="正式采集任务处于活动状态，开发录制和临时调参已禁用",
            )

    @router.get("/recordings/status")
    def recording_status(request: Request) -> dict[str, object]:
        return _recording_manager(request).status()

    @router.get("/recordings")
    def recordings(request: Request) -> dict[str, object]:
        return {"recordings": _recording_manager(request).list_recordings()}

    def _start_profile(profile, payload: RecordingStartRequest, request: Request) -> dict[str, object]:
        _ensure_no_formal_task_active(request)
        try:
            return _recording_manager(request).start(profile, payload.duration_seconds)
        except DevRecordingError as error:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error

    @router.post("/recordings/raw-sensor/start", status_code=status.HTTP_202_ACCEPTED)
    def start_raw_sensor(payload: RecordingStartRequest, request: Request) -> dict[str, object]:
        return _start_profile(RAW_SENSOR_PROFILE, payload, request)

    @router.post("/recordings/algorithm-debug/start", status_code=status.HTTP_202_ACCEPTED)
    def start_algorithm_debug(payload: RecordingStartRequest, request: Request) -> dict[str, object]:
        return _start_profile(ALGORITHM_DEBUG_PROFILE, payload, request)

    @router.post("/recordings/full-debug/start", status_code=status.HTTP_202_ACCEPTED)
    def start_full_debug(payload: RecordingStartRequest, request: Request) -> dict[str, object]:
        return _start_profile(FULL_DEBUG_PROFILE, payload, request)

    # 兼容旧 development 接口。正式前端不再使用这两个入口。
    @router.post("/recordings/raw-cloud/start", status_code=status.HTTP_202_ACCEPTED)
    def start_raw_cloud(payload: RecordingStartRequest, request: Request) -> dict[str, object]:
        return _start_profile(RAW_CLOUD_PROFILE, payload, request)

    @router.post("/recordings/diagnostic/start", status_code=status.HTTP_202_ACCEPTED)
    def start_diagnostic(payload: RecordingStartRequest, request: Request) -> dict[str, object]:
        return _start_profile(DIAGNOSTIC_PROFILE, payload, request)

    @router.post("/recordings/stop")
    def stop_recording(request: Request) -> dict[str, object]:
        try:
            return _recording_manager(request).stop()
        except DevRecordingError as error:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error

    @router.delete("/recordings/{recording_id}", status_code=status.HTTP_204_NO_CONTENT)
    def delete_recording(recording_id: str, request: Request) -> None:
        try:
            _recording_manager(request).delete(recording_id)
        except DevRecordingError as error:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error

    return router


def create_devtools_websocket_router() -> APIRouter:
    router = APIRouter()

    @router.websocket("/ws/dev/raw-cloud-preview")
    async def raw_cloud_preview(websocket: WebSocket) -> None:
        await websocket.accept()
        origin = websocket.headers.get("origin")
        host = websocket.headers.get("host")
        try:
            origin_host = urlsplit(origin).netloc if origin else ""
        except ValueError:
            origin_host = ""
        if not origin_host or not host or origin_host != host:
            await websocket.close(code=1008, reason="仅允许同源浏览器连接")
            return
        hub: CloudPreviewHub = websocket.app.state.dev_raw_cloud_hub
        try:
            session = hub.register()
        except ClientLimitReachedError:
            await websocket.send_json({
                "type": "status",
                "state": "degraded",
                "reason": "CLIENT_LIMIT_REACHED",
                "detail": "原始点云开发预览最多允许四个浏览器客户端",
            })
            await websocket.close(code=1013, reason="点云客户端数量已达到上限")
            return
        await _ensure_dev_raw_cloud_bridge(websocket, hub)
        last_stream_key: tuple[str, int] | None = None
        last_status_sent = 0.0
        disconnect_task = asyncio.create_task(websocket.receive())
        try:
            while True:
                if disconnect_task.done():
                    message = disconnect_task.result()
                    if message.get("type") == "websocket.disconnect":
                        return
                    disconnect_task = asyncio.create_task(websocket.receive())

                now = time.monotonic()
                if now - last_status_sent >= 5.0:
                    await websocket.send_json(hub.current_status())
                    last_status_sent = now
                try:
                    frame = await asyncio.wait_for(session.queue.get(), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                if frame.stream_key != last_stream_key:
                    await websocket.send_json(frame.stream_info())
                    last_stream_key = frame.stream_key
                await websocket.send_bytes(frame.binary)
        except (WebSocketDisconnect, RuntimeError):
            pass
        finally:
            disconnect_task.cancel()
            hub.unregister(session)
            await _release_dev_raw_cloud_bridge(websocket, hub)

    return router
