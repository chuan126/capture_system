import importlib.util
from pathlib import Path


PACKAGE_ROOT = Path(__file__).resolve().parents[1]


def load_launch_module(filename):
    path = PACKAGE_ROOT / "launch" / filename
    spec = importlib.util.spec_from_file_location(filename.replace(".", "_"), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_driver_launch_declares_expected_topic_contract():
    module = load_launch_module("odin_driver.launch.py")

    assert module.TOPIC_REMAPPINGS == (
        ("cloud/raw", "/capture/lidar/points_raw"),
        ("cloud/slam", "/capture/lidar/points_slam"),
        ("imu", "/capture/imu/data_raw"),
        ("odometry_hf", "/capture/odometry/high_rate_raw"),
        ("odometry", "/capture/odometry/slam"),
    )
    assert module.DRIVER_EVENT_REMAPPINGS == (
        ("device_online", "/capture/lidar/device_online"),
        ("device_offline", "/capture/lidar/device_offline"),
    )
    assert module.CHANNEL_ARGUMENT_DEFAULTS == {
        "enable_raw_point": "true",
        "enable_slam_point": "false",
        "enable_image0": "false",
        "enable_image1": "false",
        "enable_imu": "true",
        "enable_odom": "true",
    }
    assert module.DRIVER_BEHAVIOR_ARGUMENT_DEFAULTS == {
        "enable_slam_odom_sync": "false",
    }
    assert module.generate_launch_description() is not None


def test_rviz_launch_is_loadable(monkeypatch):
    module = load_launch_module("odin_rviz.launch.py")
    monkeypatch.setattr(
        module, "get_package_share_directory", lambda package_name: str(PACKAGE_ROOT)
    )

    assert module.generate_launch_description() is not None
