from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app
from backend.networking.manager import WifiManagerError, WifiNetwork


class FakeWifiManager:
    def __init__(self) -> None:
        self.connected = None
        self.last_password = None

    def status(self):
        return {"available": True, "connected": self.connected is not None, "connected_ssid": self.connected, "detail": ""}

    def list_networks(self, *, rescan=False):
        return [
            WifiNetwork("Tunnel-Lab", 82, "WPA2", self.connected == "Tunnel-Lab"),
            WifiNetwork("Open-Net", 45, "--", self.connected == "Open-Net"),
        ]

    def connect(self, ssid, password):
        self.last_password = password
        self.connected = ssid
        return {"connected": True, "connected_ssid": ssid}


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text("<html><body>ok</body></html>", encoding="utf-8")


def test_wifi_routes_are_available_in_customer_build_and_return_only_ssid_after_connect(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    app = create_app(static_dir, data_root=tmp_path / "data", start_ros_bridge=False, devtools_enabled=False)
    manager = FakeWifiManager()
    app.state.wifi_manager = manager
    with TestClient(app) as client:
        status = client.get("/api/v1/network/wifi/status")
        networks = client.post("/api/v1/network/wifi/rescan")
        connected = client.post("/api/v1/network/wifi/connect", json={"ssid": "Tunnel-Lab", "password": "secret123"})
        final_status = client.get("/api/v1/network/wifi/status")

    assert status.status_code == 200
    assert networks.status_code == 200
    assert connected.json() == {"connected": True, "connected_ssid": "Tunnel-Lab"}
    assert final_status.json()["connected_ssid"] == "Tunnel-Lab"
    assert "password" not in connected.text.lower()
    assert manager.last_password == "secret123"


def test_wifi_rescan_returns_explicit_service_error_when_active_scan_is_denied(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    app = create_app(static_dir, data_root=tmp_path / "data", start_ros_bridge=False, devtools_enabled=False)

    class DeniedWifiManager(FakeWifiManager):
        def list_networks(self, *, rescan=False):
            if rescan:
                raise WifiManagerError(
                    "Wi-Fi扫描权限不足，请检查NetworkManager的org.freedesktop.NetworkManager.wifi.scan授权"
                )
            return super().list_networks(rescan=rescan)

    app.state.wifi_manager = DeniedWifiManager()
    with TestClient(app) as client:
        response = client.post("/api/v1/network/wifi/rescan")

    assert response.status_code == 503
    assert "wifi.scan" in response.json()["detail"]
