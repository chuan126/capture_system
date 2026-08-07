from backend.ros_bridge.task_control_bridge import TaskControlBridge


class _AliveThread:
    def is_alive(self) -> bool:
        return True


def _bridge() -> TaskControlBridge:
    bridge = TaskControlBridge(lambda _snapshot: None)
    bridge._thread = _AliveThread()  # type: ignore[assignment]
    return bridge


def test_service_availability_is_tracked_per_command() -> None:
    bridge = _bridge()
    bridge._set_service_availability({
        "start": True,
        "pause": True,
        "resume": True,
        "stop": True,
        "recover": False,
    })

    assert bridge.available is True
    assert bridge.service_availability == {
        "start": True,
        "pause": True,
        "resume": True,
        "stop": True,
        "recover": False,
    }
    assert bridge.is_service_ready("stop") is True
    assert bridge.is_service_ready("recover") is False
    assert bridge.control_services_ready is False


def test_unavailable_bridge_reports_all_services_unavailable() -> None:
    bridge = TaskControlBridge(lambda _snapshot: None)
    bridge._set_service_availability({"start": True, "stop": True})

    assert bridge.available is False
    assert bridge.service_availability == {
        "start": False,
        "pause": False,
        "resume": False,
        "stop": False,
        "recover": False,
    }
