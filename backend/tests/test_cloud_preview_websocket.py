from pathlib import Path
import time

from fastapi.testclient import TestClient

from backend.main import create_app
from backend.protocols.cloud_preview_v1 import CloudPreviewFrame


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text(
        "<!doctype html><html lang='zh-CN'><body>Capture System</body></html>",
        encoding="utf-8",
    )


def test_websocket_sends_status_stream_info_and_binary_frame(
    tmp_path: Path,
) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    application = create_app(static_dir, start_ros_bridge=False)

    frame = CloudPreviewFrame(
        sequence=7,
        sensor_stamp_ns=123,
        point_count=1,
        frame_id="device0/odom",
        binary=b"PCV1-binary-frame",
    )
    application.state.cloud_preview_hub.set_ros_availability(True)
    application.state.cloud_preview_hub.publish(frame)

    with TestClient(application) as client:
        with client.websocket_connect(
            "/ws/v1/cloud-preview",
            headers={"origin": "http://testserver"},
        ) as websocket:
            status = websocket.receive_json()
            stream_info = websocket.receive_json()
            binary = websocket.receive_bytes()

    assert status["state"] == "streaming"
    assert stream_info["coordinate_mode"] == "slam_odom"
    assert stream_info["frame_id"] == "device0/odom"
    assert binary == frame.binary


def test_websocket_rejects_cross_origin_client(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    application = create_app(static_dir, start_ros_bridge=False)

    with TestClient(application) as client:
        with client.websocket_connect(
            "/ws/v1/cloud-preview",
            headers={"origin": "http://other-host"},
        ) as websocket:
            message = websocket.receive()

    assert message["type"] == "websocket.close"
    assert message["code"] == 1008


def test_websocket_remains_open_while_waiting_for_first_frame(
    tmp_path: Path,
) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    application = create_app(static_dir, start_ros_bridge=False)
    application.state.cloud_preview_hub.set_ros_availability(True)
    frame = CloudPreviewFrame(
        sequence=1,
        sensor_stamp_ns=1,
        point_count=1,
        frame_id="device0/odom",
        binary=b"first-frame",
    )

    with TestClient(application) as client:
        with client.websocket_connect(
            "/ws/v1/cloud-preview",
            headers={"origin": "http://testserver"},
        ) as websocket:
            initial_status = websocket.receive_json()
            # 等待时间超过路由内部队列超时，确认Python 3.10超时不会关闭连接。
            time.sleep(0.35)
            assert client.portal is not None
            client.portal.call(application.state.cloud_preview_hub.publish, frame)
            stream_info = websocket.receive_json()
            binary = websocket.receive_bytes()

    assert initial_status["state"] == "waiting"
    assert stream_info["type"] == "stream_info"
    assert binary == frame.binary
