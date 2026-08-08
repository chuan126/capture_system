import os
from pathlib import Path
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description() -> LaunchDescription:
    data_root = LaunchConfiguration("data_root")
    return LaunchDescription([
        DeclareLaunchArgument(
            "data_root",
            default_value=os.getenv("CAPTURE_DATA_ROOT", str(Path.cwd() / "runtime")),
        ),
        Node(
            package="task_manager",
            executable="task_manager_node",
            name="task_manager_node",
            output="screen",
            parameters=[{
                "data_root": data_root,
                "recorder_prepare_service": "/capture/recording/prepare",
                "recorder_control_service": "/capture/recording/control",
                "recorder_service_timeout_ms": 10000,
            }],
        ),
    ])
