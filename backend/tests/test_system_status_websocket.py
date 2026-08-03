from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app
from backend.protocols.system_status_v1 import DeviceStatus, SystemStatusSnapshot


def test_websocket_sends_latest_system_snapshot(tmp_path: Path) -> None:
    site = tmp_path / "site"
    site.mkdir()
    (site / "index.html").write_text("<html></html>", encoding="utf-8")
    application = create_app(site, start_ros_bridge=False)
    application.state.system_status_hub.set_ros_availability(True)
    ok = DeviceStatus("ok", "正常", {"age_ms": "10"})
    application.state.system_status_hub.publish(
        SystemStatusSnapshot(3, 4, ok, ok, ok, ok)
    )
    with TestClient(application) as client:
        with client.websocket_connect(
            "/ws/v1/system-status", headers={"origin": "http://testserver"}
        ) as websocket:
            status = websocket.receive_json()
            snapshot = websocket.receive_json()
    assert status["state"] == "streaming"
    assert snapshot["type"] == "system_status_snapshot"
    assert snapshot["storage"]["message"] == "正常"
