import asyncio

import pytest

from backend.protocols.rtk_v1 import RtkSnapshot
from backend.websocket.rtk_hub import RtkClientLimitReachedError, RtkHub


def test_slow_client_keeps_only_latest_snapshot() -> None:
    async def scenario() -> None:
        hub = RtkHub()
        hub.set_ros_availability(True)
        session = hub.register()
        hub.publish(RtkSnapshot(sequence=1))
        hub.publish(RtkSnapshot(sequence=2))

        snapshot = session.queue.get_nowait()
        assert snapshot.sequence == 2
        assert hub.overwritten_snapshots == 1

    asyncio.run(scenario())


def test_status_changes_from_waiting_to_streaming() -> None:
    hub = RtkHub()
    hub.set_ros_availability(True)
    assert hub.current_status()["state"] == "waiting"

    hub.publish(RtkSnapshot(sequence=1))
    assert hub.current_status()["state"] == "streaming"


def test_default_limit_allows_four_clients_and_rejects_fifth() -> None:
    hub = RtkHub()
    sessions = [hub.register() for _ in range(4)]

    with pytest.raises(RtkClientLimitReachedError):
        hub.register()

    for session in sessions:
        hub.unregister(session)
