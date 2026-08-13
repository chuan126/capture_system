from __future__ import annotations

import asyncio
import time
from pathlib import Path
from types import SimpleNamespace

from fastapi.testclient import TestClient

from backend.devtools.routes import (
    _ensure_dev_raw_cloud_bridge,
    _release_dev_raw_cloud_bridge,
)
from backend.devtools.telemetry_bridge import DevTelemetryBridge
from backend.main import create_app
from backend.websocket.cloud_preview_hub import CloudPreviewHub
from backend.websocket.routes import (
    _ensure_cloud_preview_bridge,
    _release_cloud_preview_bridge,
)


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text("<html><body>ok</body></html>", encoding="utf-8")


class PassiveBridge:
    def __init__(self, *_args, **_kwargs) -> None:
        self.error = None
        self.started = False
        self.stopped = False

    def start(self) -> bool:
        self.started = True
        return True

    def stop(self) -> None:
        self.stopped = True


class FakeTelemetry:
    def __init__(self) -> None:
        self.touch_calls = 0
        self.stop_calls = 0

    def touch(self) -> bool:
        self.touch_calls += 1
        return True

    def stop(self) -> None:
        self.stop_calls += 1

    def snapshot(self) -> dict[str, object]:
        return {
            "bridge_available": self.touch_calls > 0,
            "bridge_error": None,
            "emitted_at_ns": 1,
            "topics": {
                "raw_cloud": {
                    "key": "raw_cloud",
                    "topic": "/capture/lidar/points_raw",
                    "message_type": "sensor_msgs/msg/PointCloud2",
                    "received_count": 0,
                    "rate_hz": 0.0,
                    "last_received_ns": None,
                    "last_sensor_stamp_ns": None,
                    "age_ms": None,
                    "state": "waiting",
                }
            },
        }


class FakeWebSocket:
    def __init__(self, state: SimpleNamespace) -> None:
        self.app = SimpleNamespace(state=state)




def test_formal_cloud_bridge_is_not_started_during_app_lifespan(tmp_path: Path) -> None:
    site = tmp_path / "site"
    make_static_site(site)
    created: list[PassiveBridge] = []

    def cloud_factory(_sink):
        bridge = PassiveBridge()
        created.append(bridge)
        return bridge

    app = create_app(
        site,
        data_root=tmp_path / "runtime",
        start_ros_bridge=True,
        bridge_factory=cloud_factory,
        rtk_bridge_factory=PassiveBridge,
        system_status_bridge_factory=PassiveBridge,
        clearance_bridge_factory=PassiveBridge,
        task_control_bridge_factory=PassiveBridge,
        devtools_enabled=False,
    )

    with TestClient(app) as client:
        assert client.get("/api/health").status_code == 200
        assert created == []
        assert app.state.cloud_preview_bridge is None


def test_formal_cloud_bridge_helpers_start_and_stop_on_client_demand() -> None:
    async def scenario() -> None:
        hub = CloudPreviewHub()
        created: list[PassiveBridge] = []

        def factory() -> PassiveBridge:
            bridge = PassiveBridge()
            created.append(bridge)
            return bridge

        state = SimpleNamespace(
            cloud_preview_bridge_factory=factory,
            cloud_preview_bridge_lock=asyncio.Lock(),
            cloud_preview_bridge=None,
        )
        websocket = FakeWebSocket(state)
        session = hub.register()

        await _ensure_cloud_preview_bridge(websocket, hub)
        assert len(created) == 1
        assert created[0].started is True
        assert state.cloud_preview_bridge is created[0]

        hub.unregister(session)
        await _release_cloud_preview_bridge(websocket, hub)
        assert created[0].stopped is True
        assert state.cloud_preview_bridge is None
        assert hub.current_status()["state"] == "ros_unavailable"

    asyncio.run(scenario())


def test_dev_raw_cloud_bridge_helpers_start_and_stop_on_client_demand() -> None:
    async def scenario() -> None:
        hub = CloudPreviewHub()
        created: list[PassiveBridge] = []

        def factory() -> PassiveBridge:
            bridge = PassiveBridge()
            created.append(bridge)
            return bridge

        state = SimpleNamespace(
            dev_raw_cloud_bridge_factory=factory,
            dev_raw_cloud_bridge_lock=asyncio.Lock(),
            dev_raw_cloud_bridge=None,
        )
        websocket = FakeWebSocket(state)
        session = hub.register()

        await _ensure_dev_raw_cloud_bridge(websocket, hub)
        assert len(created) == 1
        assert created[0].started is True
        assert state.dev_raw_cloud_bridge is created[0]

        hub.unregister(session)
        await _release_dev_raw_cloud_bridge(websocket, hub)
        assert created[0].stopped is True
        assert state.dev_raw_cloud_bridge is None
        assert hub.current_status()["state"] == "ros_unavailable"

    asyncio.run(scenario())


def test_development_lifespan_keeps_high_rate_dev_bridges_idle_until_requested(tmp_path: Path) -> None:
    site = tmp_path / "site"
    make_static_site(site)
    fake = FakeTelemetry()
    raw_created: list[PassiveBridge] = []

    def raw_factory(_sink):
        bridge = PassiveBridge()
        raw_created.append(bridge)
        return bridge

    app = create_app(
        site,
        data_root=tmp_path / "runtime",
        start_ros_bridge=True,
        bridge_factory=lambda _sink: PassiveBridge(),
        rtk_bridge_factory=PassiveBridge,
        system_status_bridge_factory=PassiveBridge,
        clearance_bridge_factory=PassiveBridge,
        task_control_bridge_factory=PassiveBridge,
        devtools_enabled=True,
        dev_telemetry_bridge_factory=lambda: fake,
        dev_raw_cloud_bridge_factory=raw_factory,
    )

    with TestClient(app) as client:
        assert client.get("/api/health").status_code == 200
        assert fake.touch_calls == 0
        assert raw_created == []
        assert app.state.cloud_preview_bridge is None
        overview = client.get("/api/dev/overview")
        assert overview.status_code == 200
        assert fake.touch_calls == 1
        assert raw_created == []


def test_dev_overview_touches_telemetry_only_when_ros_bridge_is_enabled(tmp_path: Path) -> None:
    site = tmp_path / "site"
    make_static_site(site)
    fake = FakeTelemetry()
    app = create_app(
        site,
        data_root=tmp_path / "runtime",
        start_ros_bridge=False,
        devtools_enabled=True,
        dev_telemetry_bridge_factory=lambda: fake,
    )

    with TestClient(app) as client:
        assert client.get("/api/dev/overview").status_code == 200
        assert fake.touch_calls == 0
        app.state.dev_ros_bridge_enabled = True
        response = client.get("/api/dev/overview")
        assert response.status_code == 200
        assert response.json()["telemetry"]["bridge_available"] is True
        assert fake.touch_calls == 1

    assert fake.stop_calls == 1


class LeaseOnlyTelemetry(DevTelemetryBridge):
    def _run(self) -> None:
        self._started.set()
        while not self._stop_requested.is_set():
            deadline = self._idle_deadline_monotonic
            if deadline is not None and time.monotonic() >= deadline:
                break
            time.sleep(0.01)


def test_dev_telemetry_lease_stops_after_idle_and_can_restart() -> None:
    bridge = LeaseOnlyTelemetry(idle_timeout_seconds=0.1)
    assert bridge.touch(timeout_seconds=0.5) is True
    assert bridge.active is True
    time.sleep(0.16)
    assert bridge.active is False

    assert bridge.touch(timeout_seconds=0.5) is True
    assert bridge.active is True
    bridge.stop(timeout_seconds=0.5)
    assert bridge.active is False


def test_clearing_preview_cache_requires_a_fresh_frame_after_restart() -> None:
    from backend.protocols.cloud_preview_v1 import CloudPreviewFrame

    hub = CloudPreviewHub()
    hub.set_ros_availability(True)
    hub.publish(
        CloudPreviewFrame(
            sequence=1,
            sensor_stamp_ns=1,
            point_count=1,
            frame_id="lidar_local_enu",
            binary=b"old-frame",
        )
    )
    assert hub.current_status()["state"] == "streaming"
    hub.clear_latest_frame()
    assert hub.current_status()["state"] == "waiting"
