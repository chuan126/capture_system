from __future__ import annotations

import os
import asyncio
from contextlib import asynccontextmanager
from pathlib import Path
from typing import AsyncIterator, Callable

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from backend.ros_bridge.cloud_preview_bridge import CloudPreviewBridge
from backend.ros_bridge.rtk_bridge import RtkBridge
from backend.ros_bridge.system_status_bridge import SystemStatusBridge
from backend.websocket.cloud_preview_hub import CloudPreviewHub
from backend.websocket.rtk_hub import RtkHub
from backend.websocket.system_status_hub import SystemStatusHub
from backend.websocket.routes import router as websocket_router

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STATIC_DIR = PROJECT_ROOT / "frontend" / "out"
DEFAULT_TASK_DATA_ROOT = PROJECT_ROOT / "data" / "tasks"
REPORT_EXPORT_TEST_TASK_ID = "browser-download-test"


def resolve_static_dir() -> Path:
    configured_path = os.getenv("CAPTURE_STATIC_DIR")
    if configured_path:
        return Path(configured_path).expanduser().resolve()
    return DEFAULT_STATIC_DIR


def create_app(
    static_dir: Path | None = None,
    *,
    task_data_root: Path | None = None,
    start_ros_bridge: bool = True,
    bridge_factory: Callable[..., CloudPreviewBridge] = CloudPreviewBridge,
    rtk_bridge_factory: Callable[..., RtkBridge] = RtkBridge,
    system_status_bridge_factory: Callable[..., SystemStatusBridge] = SystemStatusBridge,
) -> FastAPI:
    site_directory = (static_dir or resolve_static_dir()).resolve()
    tasks_directory = (task_data_root or DEFAULT_TASK_DATA_ROOT).resolve()
    report_test_file = (
        tasks_directory / REPORT_EXPORT_TEST_TASK_ID / "exports" / "test.txt"
    )
    hub = CloudPreviewHub()
    rtk_hub = RtkHub()
    system_status_hub = SystemStatusHub()

    @asynccontextmanager
    async def lifespan(application: FastAPI) -> AsyncIterator[None]:
        index_file = site_directory / "index.html"
        if not index_file.is_file():
            raise RuntimeError(
                "Frontend static export is missing. "
                f"Expected {index_file}; run scripts/build/build_web.sh first."
            )

        bridge: CloudPreviewBridge | None = None
        rtk_bridge: RtkBridge | None = None
        system_status_bridge: SystemStatusBridge | None = None
        if start_ros_bridge:
            loop = asyncio.get_running_loop()
            bridge = bridge_factory(
                lambda frame: loop.call_soon_threadsafe(hub.publish, frame)
            )
            started = bridge.start()
            hub.set_ros_availability(started, bridge.error)
            rtk_bridge = rtk_bridge_factory(
                lambda snapshot: loop.call_soon_threadsafe(rtk_hub.publish, snapshot)
            )
            rtk_started = rtk_bridge.start()
            rtk_hub.set_ros_availability(rtk_started, rtk_bridge.error)
            system_status_bridge = system_status_bridge_factory(
                lambda snapshot: loop.call_soon_threadsafe(system_status_hub.publish, snapshot)
            )
            system_started = system_status_bridge.start()
            system_status_hub.set_ros_availability(system_started, system_status_bridge.error)
        else:
            application.state.cloud_preview_hub = hub
            application.state.rtk_hub = rtk_hub
            application.state.system_status_hub = system_status_hub

        try:
            yield
        finally:
            if bridge is not None:
                bridge.stop()
            if rtk_bridge is not None:
                rtk_bridge.stop()
            if system_status_bridge is not None:
                system_status_bridge.stop()

    application = FastAPI(
        title="Capture System Web API",
        version="0.1.0",
        lifespan=lifespan,
    )
    application.state.cloud_preview_hub = hub
    application.state.rtk_hub = rtk_hub
    application.state.system_status_hub = system_status_hub

    @application.get("/api/health", tags=["system"])
    async def health() -> dict[str, str]:
        return {"status": "ok"}

    @application.get("/api/v1/report-export-test", tags=["report-test"])
    async def report_export_test() -> dict[str, object]:
        """返回浏览器下载链路的固定测试数据，不表达真实测量结果。"""
        if not report_test_file.is_file():
            raise HTTPException(status_code=404, detail="测试TXT文件不存在")

        return {
            "task_id": REPORT_EXPORT_TEST_TASK_ID,
            "task_name": "浏览器下载测试任务",
            "tunnel_name": "测试隧道",
            "lane": "测试车道",
            "inspection_time": "2026-08-03 16:00:00",
            "distance_m": 100.0,
            "minimum_clearance_m": 4.82,
            "valid_points": 8,
            "quality_status": "模拟数据",
            "file_name": report_test_file.name,
            "file_size_bytes": report_test_file.stat().st_size,
            "download_url": "/api/v1/report-export-test/download",
            "clearance_points": [
                {"distance_m": 0.0, "clearance_m": 5.36},
                {"distance_m": 15.0, "clearance_m": 5.28},
                {"distance_m": 30.0, "clearance_m": 5.12},
                {"distance_m": 45.0, "clearance_m": 4.96},
                {"distance_m": 60.0, "clearance_m": 4.82},
                {"distance_m": 75.0, "clearance_m": 5.01},
                {"distance_m": 90.0, "clearance_m": 5.18},
                {"distance_m": 100.0, "clearance_m": 5.25},
            ],
        }

    @application.get("/api/v1/report-export-test/download", tags=["report-test"])
    async def download_report_export_test() -> FileResponse:
        """下载固定测试TXT，仅用于验证局域网浏览器下载链路。"""
        if not report_test_file.is_file():
            raise HTTPException(status_code=404, detail="测试TXT文件不存在")

        return FileResponse(
            report_test_file,
            media_type="text/plain; charset=utf-8",
            filename=report_test_file.name,
        )

    # WebSocket路由必须在根路径静态文件挂载之前注册。
    application.include_router(websocket_router)

    application.mount(
        "/",
        StaticFiles(directory=site_directory, html=True, check_dir=False),
        name="frontend",
    )
    return application


app = create_app()
