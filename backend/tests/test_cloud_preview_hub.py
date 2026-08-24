import asyncio

import pytest

from backend.protocols.cloud_preview_v1 import CloudPreviewFrame
from backend.websocket.cloud_preview_hub import (
    ClientLimitReachedError,
    CloudPreviewHub,
)


def make_frame(sequence: int) -> CloudPreviewFrame:
    return CloudPreviewFrame(
        sequence=sequence,
        sensor_stamp_ns=sequence,
        point_count=1,
        frame_id="device0/odom",
        binary=b"frame-" + bytes([sequence]),
    )


def test_slow_client_keeps_only_latest_frame() -> None:
    async def scenario() -> None:
        hub = CloudPreviewHub()
        hub.set_ros_availability(True)
        session = hub.register()

        hub.publish(make_frame(1))
        hub.publish(make_frame(2))

        frame = session.queue.get_nowait()
        assert frame.sequence == 2
        assert hub.overwritten_frames == 1

    asyncio.run(scenario())


def test_status_changes_from_waiting_to_streaming() -> None:
    hub = CloudPreviewHub()
    hub.set_ros_availability(True)
    assert hub.current_status()["state"] == "waiting"

    hub.publish(make_frame(1))
    assert hub.current_status()["state"] == "streaming"


def test_default_limit_allows_four_clients_and_rejects_fifth() -> None:
    hub = CloudPreviewHub()

    sessions = [hub.register() for _ in range(4)]
    assert hub.client_count == 4

    with pytest.raises(ClientLimitReachedError):
        hub.register()

    for session in sessions:
        hub.unregister(session)
    assert hub.client_count == 0
