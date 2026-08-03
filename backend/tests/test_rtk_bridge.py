from types import SimpleNamespace

from backend.ros_bridge.rtk_bridge import RtkBridge


def test_maps_byte_encoded_diagnostic_level_without_stopping_bridge() -> None:
    snapshots = []
    bridge = RtkBridge(snapshots.append)
    diagnostic = SimpleNamespace(
        status=[
            SimpleNamespace(
                name="rtk_driver/serial",
                level=b"\x00",
                message="串口已连接",
            )
        ]
    )

    bridge._on_diagnostics(diagnostic, b"\x00")

    assert len(snapshots) == 1
    assert snapshots[0].serial_connected is True
    assert snapshots[0].serial_message == "串口已连接"
