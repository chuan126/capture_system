import os
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
    storage_data_path = LaunchConfiguration("storage_data_path")
    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "parameters_file",
                default_value=str(default_parameters),
                description="系统监控参数文件绝对路径",
            ),
            DeclareLaunchArgument(
                "storage_data_path",
                default_value=os.getenv("CAPTURE_DATA_ROOT", str(Path.cwd() / "runtime")),
                description="任务数据根目录，供磁盘容量和可写性诊断使用",
            ),
            Node(
                package="system_monitor",
                executable="system_monitor_node",
                name="system_monitor_node",
                output="screen",
                parameters=[parameters_file, {"storage_data_path": storage_data_path}],
            ),
        ]
    )
