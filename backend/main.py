from __future__ import annotations

import os
import asyncio
import logging
from contextlib import asynccontextmanager
from pathlib import Path
from typing import AsyncIterator, Callable

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

from backend.ros_bridge.clearance_bridge import ClearanceBridge
from backend.ros_bridge.cloud_preview_bridge import CloudPreviewBridge
from backend.ros_bridge.rtk_bridge import RtkBridge
from backend.ros_bridge.system_status_bridge import SystemStatusBridge
from backend.ros_bridge.task_control_bridge import TaskControlBridge
from backend.websocket.clearance_hub import ClearanceHub
from backend.websocket.cloud_preview_hub import CloudPreviewHub
from backend.websocket.rtk_hub import RtkHub
from backend.websocket.system_status_hub import SystemStatusHub
from backend.websocket.task_status_hub import TaskStatusHub
from backend.websocket.routes import router as websocket_router
from backend.measurements.repository import MeasurementRepository
from backend.measurements.query_coordinator import MeasurementQueryCoordinator
from backend.exports.routes import router as export_router
from backend.batches.routes import router as batch_router
from backend.exports.service import ReportExportService
from backend.tasks.repository import TaskRepository, TaskStorageError
from backend.tasks.routes import router as task_router
from backend.tasks.control_routes import router as task_control_router
from backend.amap.routes import router as amap_router
from backend.device_settings import DeviceSettingsError, DeviceSettingsStore
from backend.networking.manager import NetworkManagerWifi
from backend.networking.routes import router as wifi_router

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STATIC_DIR = PROJECT_ROOT / "frontend" / "out"
LOGGER = logging.getLogger(__name__)


def resolve_static_dir() -> Path:
    configured_path = os.getenv("CAPTURE_STATIC_DIR")
    if configured_path:
        path = Path(configured_path).expanduser()
        return (path if path.is_absolute() else PROJECT_ROOT / path).resolve()
    return DEFAULT_STATIC_DIR


def resolve_data_root() -> Path:
    configured_path = os.getenv("CAPTURE_DATA_ROOT")
    if configured_path:
        path = Path(configured_path).expanduser()
        return (path if path.is_absolute() else PROJECT_ROOT / path).resolve()
    return (PROJECT_ROOT / "runtime").resolve()


def resolve_devtools_enabled() -> bool:
    return os.getenv("CAPTURE_DEVTOOLS_ENABLED", "0").strip().lower() in {"1", "true", "yes", "on"}


def resolve_version() -> str:
    version_file = PROJECT_ROOT / "VERSION"
    try:
        return version_file.read_text(encoding="utf-8").strip() or "unknown"
    except OSError:
        return "unknown"


def create_app(
    static_dir: Path | None = None,
    *,
    task_data_root: Path | None = None,
    data_root: Path | None = None,
    task_database_path: Path | None = None,
    pdf_font_path: Path | None = None,
    start_ros_bridge: bool = True,
    bridge_factory: Callable[..., CloudPreviewBridge] = CloudPreviewBridge,
    rtk_bridge_factory: Callable[..., RtkBridge] = RtkBridge,
    system_status_bridge_factory: Callable[..., SystemStatusBridge] = SystemStatusBridge,
    clearance_bridge_factory: Callable[..., ClearanceBridge] = ClearanceBridge,
    task_control_bridge_factory: Callable[..., TaskControlBridge] = TaskControlBridge,
    devtools_enabled: bool | None = None,
    dev_telemetry_bridge_factory: Callable[[], object] | None = None,
    dev_raw_cloud_bridge_factory: Callable[..., object] | None = None,
) -> FastAPI:
    site_directory = (static_dir or resolve_static_dir()).resolve()
    if data_root is not None:
        runtime_data_root = data_root.resolve()
    elif task_data_root is not None:
        # 保留测试和旧调用方式，同时使数据库位于任务目录的上一级。
        runtime_data_root = task_data_root.resolve().parent
    else:
        runtime_data_root = resolve_data_root()
    tasks_directory = (task_data_root or runtime_data_root / "tasks").resolve()
    database_path = (task_database_path or runtime_data_root / "capture.db").resolve()
    task_repository = TaskRepository(database_path, tasks_directory)
    measurement_repository = MeasurementRepository(tasks_directory)
    measurement_query_coordinator = MeasurementQueryCoordinator()
    configured_pdf_font = os.getenv("CAPTURE_PDF_FONT_PATH")
    resolved_pdf_font_path = (
        pdf_font_path.resolve()
        if pdf_font_path is not None
        else Path(configured_pdf_font).expanduser().resolve() if configured_pdf_font else None
    )
    report_export_service = ReportExportService(
        runtime_data_root,
        task_repository,
        measurement_repository,
        pdf_font_path=resolved_pdf_font_path,
    )
    development_tools_enabled = resolve_devtools_enabled() if devtools_enabled is None else devtools_enabled
    device_settings_store = DeviceSettingsStore(runtime_data_root)
    wifi_manager = NetworkManagerWifi()
    dev_telemetry_bridge = None
    dev_raw_cloud_bridge = None
    dev_raw_cloud_hub = None
    dev_recording_manager = None
    dev_offline_replay_manager = None
    dev_parameter_service = None
    dev_parameter_bridge = None
    dev_system_metrics = None
    devtools_http_router = None
    devtools_ws_router = None
    if development_tools_enabled:
        from backend.devtools.offline_replay import OfflineReplayManager
        from backend.devtools.parameter_bridge import DevParameterBridge
        from backend.devtools.parameters import DevParameterService
        from backend.devtools.recording import RosbagRecordingManager
        from backend.devtools.routes import create_devtools_router, create_devtools_websocket_router
        from backend.devtools.telemetry_bridge import DevTelemetryBridge
        from backend.devtools.system_metrics import SystemMetricsSampler

        telemetry_factory = dev_telemetry_bridge_factory or DevTelemetryBridge
        dev_telemetry_bridge = telemetry_factory()
        dev_raw_cloud_hub = CloudPreviewHub()
        dev_parameter_bridge = DevParameterBridge()
        dev_parameter_service = DevParameterService(bridge=dev_parameter_bridge)
        dev_recording_manager = RosbagRecordingManager(
            runtime_data_root,
            parameter_snapshot_provider=dev_parameter_service.snapshot,
        )
        dev_offline_replay_manager = OfflineReplayManager(
            dev_recording_manager,
            parameter_snapshot_provider=dev_parameter_service.snapshot,
            project_root=PROJECT_ROOT,
        )
        dev_system_metrics = SystemMetricsSampler()
        devtools_http_router = create_devtools_router()
        devtools_ws_router = create_devtools_websocket_router()
    hub = CloudPreviewHub()
    rtk_hub = RtkHub()
    system_status_hub = SystemStatusHub()
    clearance_hub = ClearanceHub()
    task_status_hub = TaskStatusHub()

    @asynccontextmanager
    async def lifespan(application: FastAPI) -> AsyncIterator[None]:
        index_file = site_directory / "index.html"
        if not index_file.is_file():
            raise RuntimeError(
                "Frontend static export is missing. "
                f"Expected {index_file}; run scripts/build/build.sh web first."
            )

        runtime_data_root.mkdir(parents=True, exist_ok=True)
        (runtime_data_root / "tasks").mkdir(parents=True, exist_ok=True)
        (runtime_data_root / "reports").mkdir(parents=True, exist_ok=True)
        (runtime_data_root / "dev-tests").mkdir(parents=True, exist_ok=True)
        try:
            device_settings_store.initialize()
        except DeviceSettingsError as error:
            # 地图配置损坏不能阻断正式任务、记录和实时监视；地图接口会继续明确返回配置错误。
            LOGGER.error("%s", error)
        try:
            task_repository.initialize()
        except TaskStorageError as error:
            # 实时监视仍可启动，但任务接口会明确返回503，不能退化为浏览器临时任务。
            LOGGER.error("%s", error)

        rtk_bridge: RtkBridge | None = None
        system_status_bridge: SystemStatusBridge | None = None
        clearance_bridge: ClearanceBridge | None = None
        task_control_bridge: TaskControlBridge | None = None
        application.state.cloud_preview_bridge_lock = asyncio.Lock()
        application.state.dev_raw_cloud_bridge_lock = asyncio.Lock()
        if start_ros_bridge:
            loop = asyncio.get_running_loop()
            application.state.cloud_preview_bridge_factory = lambda: bridge_factory(
                lambda frame: loop.call_soon_threadsafe(hub.publish, frame)
            )
            hub.set_ros_availability(False, "等待点云预览客户端")
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
            clearance_bridge = clearance_bridge_factory(
                lambda snapshot: loop.call_soon_threadsafe(clearance_hub.publish, snapshot)
            )
            clearance_started = clearance_bridge.start()
            clearance_hub.set_ros_availability(clearance_started, clearance_bridge.error)
            task_control_bridge = task_control_bridge_factory(
                lambda snapshot: loop.call_soon_threadsafe(task_status_hub.publish, snapshot)
            )
            task_control_started = task_control_bridge.start()
            task_status_hub.set_ros_availability(
                task_control_started, task_control_bridge.error
            )
            application.state.task_control_bridge = task_control_bridge
            if development_tools_enabled and dev_parameter_bridge is not None and dev_parameter_service is not None:
                dev_parameter_bridge.start()
                dev_parameter_service.start()
            if development_tools_enabled and dev_raw_cloud_hub is not None:
                from backend.devtools.raw_cloud_bridge import DevRawCloudPreviewBridge

                raw_factory = dev_raw_cloud_bridge_factory or DevRawCloudPreviewBridge
                application.state.dev_raw_cloud_bridge_factory = lambda: raw_factory(
                    lambda frame: loop.call_soon_threadsafe(dev_raw_cloud_hub.publish, frame)
                )
                dev_raw_cloud_hub.set_ros_availability(False, "等待开发点云预览客户端")
        else:
            if development_tools_enabled and dev_parameter_bridge is not None and dev_parameter_service is not None:
                dev_parameter_bridge.start()
                dev_parameter_service.start()
            task_status_hub.set_ros_availability(False, "任务控制ROS桥未启动")
            if development_tools_enabled and dev_raw_cloud_hub is not None:
                dev_raw_cloud_hub.set_ros_availability(False, "开发ROS桥未启动")

        try:
            yield
        finally:
            cloud_preview_bridge = application.state.cloud_preview_bridge
            if cloud_preview_bridge is not None:
                cloud_preview_bridge.stop()
                application.state.cloud_preview_bridge = None
            if rtk_bridge is not None:
                rtk_bridge.stop()
            if system_status_bridge is not None:
                system_status_bridge.stop()
            if clearance_bridge is not None:
                clearance_bridge.stop()
            if task_control_bridge is not None:
                task_control_bridge.stop()
            active_dev_raw_cloud_bridge = application.state.dev_raw_cloud_bridge
            if active_dev_raw_cloud_bridge is not None:
                active_dev_raw_cloud_bridge.stop()
                application.state.dev_raw_cloud_bridge = None
            if dev_telemetry_bridge is not None:
                dev_telemetry_bridge.stop()
            if dev_offline_replay_manager is not None:
                dev_offline_replay_manager.stop_on_shutdown()
            if dev_recording_manager is not None:
                dev_recording_manager.stop_on_shutdown()
            if dev_parameter_service is not None:
                dev_parameter_service.stop()
            if dev_parameter_bridge is not None:
                dev_parameter_bridge.stop()

    application = FastAPI(
        title="Capture System Web API",
        version="0.1.0",
        lifespan=lifespan,
    )
    application.state.cloud_preview_hub = hub
    application.state.cloud_preview_bridge = None
    application.state.cloud_preview_bridge_factory = None
    application.state.cloud_preview_bridge_lock = None
    application.state.rtk_hub = rtk_hub
    application.state.system_status_hub = system_status_hub
    application.state.clearance_hub = clearance_hub
    application.state.task_status_hub = task_status_hub
    application.state.task_repository = task_repository
    application.state.measurement_repository = measurement_repository
    application.state.measurement_query_coordinator = measurement_query_coordinator
    application.state.report_export_service = report_export_service
    application.state.task_control_bridge = None
    application.state.runtime_data_root = runtime_data_root
    application.state.capture_version = resolve_version()
    application.state.devtools_enabled = development_tools_enabled
    application.state.dev_telemetry_bridge = dev_telemetry_bridge
    application.state.dev_ros_bridge_enabled = start_ros_bridge
    application.state.dev_raw_cloud_hub = dev_raw_cloud_hub
    application.state.dev_raw_cloud_bridge = None
    application.state.dev_raw_cloud_bridge_factory = None
    application.state.dev_raw_cloud_bridge_lock = None
    application.state.dev_recording_manager = dev_recording_manager
    application.state.dev_offline_replay_manager = dev_offline_replay_manager
    application.state.dev_parameter_service = dev_parameter_service
    application.state.dev_system_metrics = dev_system_metrics
    application.state.device_settings_store = device_settings_store
    application.state.wifi_manager = wifi_manager

    @application.get("/api/health", tags=["system"])
    async def health() -> dict[str, str]:
        return {"status": "ok"}

    # API与WebSocket路由必须在根路径静态文件挂载之前注册。
    application.include_router(batch_router)
    application.include_router(task_router)
    application.include_router(task_control_router)
    application.include_router(export_router)
    application.include_router(amap_router)
    application.include_router(wifi_router)
    application.include_router(websocket_router)

    if development_tools_enabled:
        assert devtools_http_router is not None and devtools_ws_router is not None
        application.include_router(devtools_http_router)
        application.include_router(devtools_ws_router)

    application.mount(
        "/",
        StaticFiles(directory=site_directory, html=True, check_dir=False),
        name="frontend",
    )
    return application


app = create_app()
