from types import SimpleNamespace

from backend.ros_bridge.system_status_bridge import SystemStatusBridge


def test_maps_byte_encoded_diagnostic_levels_and_values() -> None:
    snapshots = []
    bridge = SystemStatusBridge(snapshots.append)
    diagnostic = SimpleNamespace(
        status=[
            SimpleNamespace(
                name="system_monitor/rtk",
                level=b"\x00",
                message="串口已连接",
                values=[SimpleNamespace(key="age_ms", value="12.0")],
            )
        ]
    )
    bridge._on_diagnostics(diagnostic)
    assert len(snapshots) == 1
    assert snapshots[0].rtk.state == "ok"
    assert snapshots[0].rtk.message == "串口已连接"
    assert snapshots[0].rtk.values == {"age_ms": "12.0"}
    assert snapshots[0].lidar.state == "unknown"
