from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app
from backend.protocols.clearance_v1 import ClearanceSnapshot


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text(
        "<!doctype html><html lang='zh-CN'><body>Capture System</body></html>",
        encoding="utf-8",
    )


def test_websocket_sends_latest_clearance_snapshot(tmp_path: Path) -> None:
    site = tmp_path / "site"
    make_static_site(site)
    application = create_app(site, start_ros_bridge=False)
    application.state.clearance_hub.set_ros_availability(True)
    application.state.clearance_hub.publish(
        ClearanceSnapshot(
            sequence=9,
            emitted_at_ns=1,
            stamp_ns=2,
            frame_id="lidar",
            valid=True,
            lidar_to_top_m=1.723,
            ransac_plane_count=3,
            candidate_count=4,
            selected_inlier_count=1234,
            selected_area_m2=1.1,
            selected_tilt_deg=2.3,
            residual_median_m=0.01,
            residual_p95_m=0.03,
            minimum_position_east_m=-0.4,
            minimum_position_north_m=0.2,
            minimum_position_up_m=1.723,
            valid_point_ratio=0.51,
            invalid_reason="NONE",
            processing_time_ms=46.8,
        )
    )

    with TestClient(application) as client:
        with client.websocket_connect(
            "/ws/v1/clearance", headers={"origin": "http://testserver"}
        ) as websocket:
            status = websocket.receive_json()
            result = websocket.receive_json()

    assert status["state"] == "streaming"
    assert result["type"] == "clearance_snapshot"
    assert result["valid"] is True
    assert result["lidar_to_top_m"] == 1.723


def test_websocket_rejects_cross_origin_client(tmp_path: Path) -> None:
    site = tmp_path / "site"
    make_static_site(site)
    application = create_app(site, start_ros_bridge=False)

    with TestClient(application) as client:
        with client.websocket_connect(
            "/ws/v1/clearance", headers={"origin": "http://other-host"}
        ) as websocket:
            message = websocket.receive()

    assert message["type"] == "websocket.close"
    assert message["code"] == 1008
