from __future__ import annotations

import os
import asyncio
from contextlib import asynccontextmanager
from pathlib import Path
from typing import AsyncIterator, Callable

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

from backend.ros_bridge.cloud_preview_bridge import CloudPreviewBridge
from backend.ros_bridge.rtk_bridge import RtkBridge
from backend.websocket.cloud_preview_hub import CloudPreviewHub
from backend.websocket.rtk_hub import RtkHub
from backend.websocket.routes import router as websocket_router

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STATIC_DIR = PROJECT_ROOT / "frontend" / "out"


def resolve_static_dir() -> Path:
    configured_path = os.getenv("CAPTURE_STATIC_DIR")
    if configured_path:
        return Path(configured_path).expanduser().resolve()
    return DEFAULT_STATIC_DIR


def create_app(
    static_dir: Path | None = None,
    *,
    start_ros_bridge: bool = True,
    bridge_factory: Callable[..., CloudPreviewBridge] = CloudPreviewBridge,
    rtk_bridge_factory: Callable[..., RtkBridge] = RtkBridge,
) -> FastAPI:
    site_directory = (static_dir or resolve_static_dir()).resolve()
    hub = CloudPreviewHub()
    rtk_hub = RtkHub()

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
        else:
            application.state.cloud_preview_hub = hub
            application.state.rtk_hub = rtk_hub

        try:
            yield
        finally:
            if bridge is not None:
                bridge.stop()
            if rtk_bridge is not None:
                rtk_bridge.stop()

    application = FastAPI(
        title="Capture System Web API",
        version="0.1.0",
        lifespan=lifespan,
    )
    application.state.cloud_preview_hub = hub
    application.state.rtk_hub = rtk_hub

    @application.get("/api/health", tags=["system"])
    async def health() -> dict[str, str]:
        return {"status": "ok"}

    # WebSocket路由必须在根路径静态文件挂载之前注册。
    application.include_router(websocket_router)

    application.mount(
        "/",
        StaticFiles(directory=site_directory, html=True, check_dir=False),
        name="frontend",
    )
    return application


app = create_app()
