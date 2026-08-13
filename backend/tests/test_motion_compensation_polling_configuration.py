from pathlib import Path

import yaml


PROJECT_ROOT = Path(__file__).resolve().parents[2]
MOTION_ROOT = PROJECT_ROOT / "ros2_ws" / "src" / "motion_compensation"


def test_motion_compensation_yaml_uses_formal_ten_millisecond_default() -> None:
    payload = yaml.safe_load(
        (MOTION_ROOT / "config" / "motion_compensation.yaml").read_text(
            encoding="utf-8"
        )
    )
    parameters = payload["enu_cloud_transform_node"]["ros__parameters"]
    assert parameters["processing_poll_interval_ms"] == 10
    assert "processing_period_ms" not in parameters
    assert parameters["diagnostics_topic"] == "/diagnostics"


def test_formal_bringup_loads_motion_compensation_package_yaml() -> None:
    launch_source = (
        PROJECT_ROOT
        / "ros2_ws"
        / "src"
        / "bringup"
        / "launch"
        / "clearance_preview.launch.py"
    ).read_text(encoding="utf-8")
    assert 'get_package_share_directory("motion_compensation")' in launch_source
    assert '"motion_compensation.yaml"' in launch_source
    assert "parameters=[str(motion_parameters)]" in launch_source


def test_development_binding_documents_poll_interval_as_restart_only() -> None:
    bindings = yaml.safe_load(
        (
            PROJECT_ROOT
            / "ros2_ws"
            / "src"
            / "bringup"
            / "config"
            / "dev_parameter_bindings.yaml"
        ).read_text(encoding="utf-8")
    )
    binding = next(
        item
        for item in bindings["parameters"]
        if item["key"] == "motion.processing_poll_interval_ms"
    )
    assert binding["parameter"] == "processing_poll_interval_ms"
    assert binding["minimum"] == 1
    assert binding["maximum"] == 100
    assert binding["step"] == 1
    assert binding["unit"] == "ms"
    assert binding["writable"] is False
    assert binding["ui_visible"] is False
