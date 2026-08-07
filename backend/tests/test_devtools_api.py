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

    assert overview.status_code == 200
    payload = overview.json()
    assert payload["build_variant"] == "development"
    assert payload["telemetry"]["bridge_available"] is False
    assert "/capture/lidar/points_raw" == payload["telemetry"]["topics"]["raw_cloud"]["topic"]
    assert status.status_code == 200
    assert status.json()["active"] is False
    assert rtk.status_code == 200
    assert rtk.json()["confirmed"] is False


def test_devtools_websocket_route_only_exists_in_development(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    customer = create_app(static_dir, data_root=tmp_path / "customer", start_ros_bridge=False, devtools_enabled=False)
    development = create_app(static_dir, data_root=tmp_path / "development", start_ros_bridge=False, devtools_enabled=True)
    customer_paths = {getattr(route, "path", None) for route in customer.routes}
    development_paths = {getattr(route, "path", None) for route in development.routes}
    assert "/ws/dev/raw-cloud-preview" not in customer_paths
    assert "/ws/dev/raw-cloud-preview" in development_paths
