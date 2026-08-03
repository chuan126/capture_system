from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app
from backend.protocols.rtk_v1 import RtkSnapshot


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text(
        "<!doctype html><html lang='zh-CN'><body>Capture System</body></html>",
        encoding="utf-8",
    )


def test_websocket_sends_latest_rtk_snapshot(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    application = create_app(static_dir, start_ros_bridge=False)
    application.state.rtk_hub.set_ros_availability(True)
    application.state.rtk_hub.publish(
        RtkSnapshot(
            sequence=7,
            serial_connected=True,
            serial_message="串口已连接",
            gps_state=0,
            satellite_count=0,
            hdop=0.0,
            fix_status=-1,
            latitude=0.0,
            longitude=0.0,
            altitude=0.0,
        )
    )

    with TestClient(application) as client:
        with client.websocket_connect(
            "/ws/v1/rtk",
            headers={"origin": "http://testserver"},
        ) as websocket:
            status = websocket.receive_json()
            snapshot = websocket.receive_json()

    assert status["state"] == "streaming"
    assert snapshot["type"] == "rtk_snapshot"
    assert snapshot["sequence"] == 7
    assert snapshot["serial_connected"] is True
    assert snapshot["gps_state"] == 0
    assert snapshot["fix_status"] == -1
    assert "quality" not in snapshot


def test_websocket_rejects_cross_origin_client(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    application = create_app(static_dir, start_ros_bridge=False)

    with TestClient(application) as client:
        with client.websocket_connect(
            "/ws/v1/rtk",
            headers={"origin": "http://other-host"},
        ) as websocket:
            message = websocket.receive()

    assert message["type"] == "websocket.close"
    assert message["code"] == 1008
