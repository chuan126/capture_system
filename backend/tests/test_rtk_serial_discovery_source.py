from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_rtk_defaults_to_safe_auto_discovery_with_explicit_override() -> None:
    config = (ROOT / "ros2_ws/src/rtk_driver/config/rtk_driver.yaml").read_text(encoding="utf-8")
    node = (ROOT / "ros2_ws/src/rtk_driver/src/rtk_driver_node.cpp").read_text(encoding="utf-8")
    discovery = (ROOT / "ros2_ws/src/rtk_driver/src/serial_discovery.cpp").read_text(encoding="utf-8")
    assert 'device: "auto"' in config
    assert not any(
        line.lstrip().startswith("auto_preferred_tokens:")
        for line in config.splitlines()
    )
    assert 'declare_parameter<std::string>("device", "auto")' in node
    assert 'declare_parameter<std::vector<std::string>>(' in node
    assert '"auto_preferred_tokens", std::vector<std::string>{}' in node
    assert 'device_ == "auto"' in node
    assert "/dev/serial/by-id" in discovery
    assert "ttyUSB" in discovery and "ttyACM" in discovery
    assert "多个串口" in discovery
    assert "不进行逐口探测" in discovery
    assert "contains_gnss_stream_signature" in discovery
    assert "NMEA0183/gnss_nmea.c" not in discovery


def test_rtk_discovery_diagnostics_expose_actual_selected_device() -> None:
    node = (ROOT / "ros2_ws/src/rtk_driver/src/rtk_driver_node.cpp").read_text(encoding="utf-8")
    for key in [
        "configured_device",
        "active_device",
        "auto_discovery",
        "serial_connected",
        "discovery_candidate_count",
        "discovery_detail",
    ]:
        assert key in node


def test_rtk_identity_probe_rejects_unsupported_nmea_types() -> None:
    discovery = (ROOT / "ros2_ws/src/rtk_driver/src/serial_discovery.cpp").read_text(encoding="utf-8")
    for supported in ["$GPRMC,", "$GNRMC,", "$GPGGA,", "$GNGGA,", "$GPGSA,", "$GNGSA,", "#BESTPOSA,"]:
        assert supported in discovery
    for unsupported in ["$GNVTG", "#BESTPOSB"]:
        assert unsupported not in discovery
