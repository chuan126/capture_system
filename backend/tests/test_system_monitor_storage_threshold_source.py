from pathlib import Path


def test_storage_capacity_alert_starts_at_five_gib_and_severe_level_at_one_gib() -> None:
    root = Path(__file__).resolve().parents[2]
    node = (root / "ros2_ws/src/system_monitor/src/system_monitor_node.cpp").read_text(
        encoding="utf-8"
    )
    config = (root / "ros2_ws/src/system_monitor/config/system_monitor.yaml").read_text(
        encoding="utf-8"
    )

    assert 'gibibytes("storage_warn_available_gib", 5.0)' in node
    assert 'gibibytes("storage_error_available_gib", 1.0)' in node
    assert "storage_warn_available_gib: 5.0" in config
    assert "storage_error_available_gib: 1.0" in config
