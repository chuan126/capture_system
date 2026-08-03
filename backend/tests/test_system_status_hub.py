from backend.protocols.system_status_v1 import DeviceStatus, SystemStatusSnapshot
from backend.websocket.system_status_hub import SystemStatusHub


def test_marks_diagnostic_stream_stale_instead_of_preserving_ok() -> None:
    hub = SystemStatusHub(stale_after_seconds=0.0)
    hub.set_ros_availability(True)
    ok = DeviceStatus("ok", "正常", {})
    hub.publish(SystemStatusSnapshot(1, 2, ok, ok, ok, ok))

    status = hub.current_status()

    assert status["state"] == "degraded"
    assert status["reason"] == "DIAGNOSTICS_STALE"
