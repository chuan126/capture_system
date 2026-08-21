import asyncio

from backend.protocols.clearance_v1 import ClearanceSnapshot
from backend.websocket.clearance_hub import ClearanceHub


def snapshot(sequence: int) -> ClearanceSnapshot:
    return ClearanceSnapshot(
        sequence=sequence,
        emitted_at_ns=1,
        stamp_ns=1,
        frame_id="lidar",
        valid=True,
        lidar_to_top_m=1.7,
        ransac_plane_count=1,
        surface_count=0,
        candidate_count=1,
        selected_inlier_count=100,
        selected_area_m2=1.0,
        selected_tilt_deg=1.0,
        residual_median_m=0.01,
        residual_p95_m=0.02,
        minimum_position_east_m=0.0,
        minimum_position_north_m=0.0,
        minimum_position_up_m=1.7,
        valid_point_ratio=0.5,
        invalid_reason="NONE",
        processing_time_ms=40.0,
    )


def test_slow_client_keeps_only_latest_snapshot() -> None:
    async def scenario() -> None:
        hub = ClearanceHub()
        session = hub.register()
        hub.publish(snapshot(1))
        hub.publish(snapshot(2))
        assert session.queue.get_nowait().sequence == 2
        assert hub.overwritten_snapshots == 1

    asyncio.run(scenario())


def test_status_changes_from_waiting_to_streaming() -> None:
    hub = ClearanceHub()
    hub.set_ros_availability(True)
    assert hub.current_status()["state"] == "waiting"
    hub.publish(snapshot(1))
    assert hub.current_status()["state"] == "streaming"
