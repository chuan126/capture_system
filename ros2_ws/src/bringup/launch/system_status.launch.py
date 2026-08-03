from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description() -> LaunchDescription:
    default_parameters = (
        Path(get_package_share_directory("system_monitor"))
        / "config"
        / "system_monitor.yaml"
    )
    parameters_file = LaunchConfiguration("parameters_file")
    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "parameters_file",
                default_value=str(default_parameters),
                description="系统状态监控参数文件绝对路径",
            ),
            Node(
                package="system_monitor",
                executable="system_monitor_node",
                name="system_monitor_node",
                output="screen",
                parameters=[parameters_file],
            ),
        ]
    )
