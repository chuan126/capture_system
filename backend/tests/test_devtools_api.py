from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text("<html><body>ok</body></html>", encoding="utf-8")


def test_customer_app_does_not_register_devtools_routes(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    with TestClient(create_app(static_dir, data_root=tmp_path / "data", start_ros_bridge=False, devtools_enabled=False)) as client:
        assert client.get("/api/dev/overview").status_code == 404
        assert client.get("/api/dev/recordings/status").status_code == 404


def test_development_app_registers_devtools_without_ros(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    with TestClient(create_app(static_dir, data_root=tmp_path / "data", start_ros_bridge=False, devtools_enabled=True)) as client:
        overview = client.get("/api/dev/overview")
        status = client.get("/api/dev/recordings/status")
        rtk = client.get("/api/dev/rtk/snapshot")
        offline = client.get("/api/dev/offline/status")

    assert overview.status_code == 200
    payload = overview.json()
    assert payload["build_variant"] == "development"
    assert payload["telemetry"]["bridge_available"] is False
    assert "/capture/lidar/points_raw" == payload["telemetry"]["topics"]["raw_cloud"]["topic"]
    assert status.status_code == 200
    assert status.json()["active"] is False
    assert rtk.status_code == 200
    assert rtk.json()["confirmed"] is False
    assert offline.status_code == 200
    assert offline.json()["active"] is False
    assert offline.json()["state"] == "idle"


def test_devtools_websocket_route_only_exists_in_development(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    customer = create_app(static_dir, data_root=tmp_path / "customer", start_ros_bridge=False, devtools_enabled=False)
    development = create_app(static_dir, data_root=tmp_path / "development", start_ros_bridge=False, devtools_enabled=True)
    customer_paths = {getattr(route, "path", None) for route in customer.routes}
    development_paths = {getattr(route, "path", None) for route in development.routes}
    assert "/ws/dev/raw-cloud-preview" not in customer_paths
    assert "/ws/dev/raw-cloud-preview" in development_paths
    assert "/api/dev/recordings/raw-sensor/start" not in customer_paths
    assert "/api/dev/recordings/raw-sensor/start" in development_paths
    assert "/api/dev/recordings/algorithm-debug/start" in development_paths
    assert "/api/dev/recordings/full-debug/start" in development_paths
    assert "/api/dev/offline/status" not in customer_paths
    assert "/api/dev/offline/status" in development_paths
    assert "/api/dev/offline/start" in development_paths
    assert "/api/dev/offline/stop" in development_paths


def test_runtime_parameter_change_is_blocked_while_offline_detection_is_active(tmp_path: Path) -> None:
    from types import SimpleNamespace

    static_dir = tmp_path / "site-offline-parameter-block"
    make_static_site(static_dir)
    application = create_app(
        static_dir,
        data_root=tmp_path / "data-offline-parameter-block",
        start_ros_bridge=False,
        devtools_enabled=True,
    )
    application.state.dev_offline_replay_manager = SimpleNamespace(active=True)
    with TestClient(application) as client:
        response = client.put(
            "/api/dev/parameters/clearance.max_candidate_planes",
            json={"value": 100},
        )

    assert response.status_code == 409
    assert "离线算法检测" in response.json()["detail"]
