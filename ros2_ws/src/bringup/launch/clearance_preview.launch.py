from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description() -> LaunchDescription:
    motion_parameters = (
        Path(get_package_share_directory("motion_compensation"))
        / "config"
        / "motion_compensation.yaml"
    )
    default_parameters = (
        Path(get_package_share_directory("clearance_engine"))
        / "config"
        / "clearance_engine.yaml"
    )
    parameters_file = LaunchConfiguration("parameters_file")

    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "parameters_file",
                default_value=str(default_parameters),
                description="首版净空算法参数文件绝对路径",
            ),
            Node(
                package="motion_compensation",
                executable="enu_cloud_transform_node",
                name="enu_cloud_transform_node",
                output="screen",
                parameters=[str(motion_parameters)],
            ),
            Node(
                package="clearance_engine",
                executable="clearance_engine_node",
                name="clearance_engine_node",
                output="screen",
                parameters=[parameters_file],
            ),
        ]
    )
