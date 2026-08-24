from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description() -> LaunchDescription:
    default_parameters = (
        Path(get_package_share_directory("clearance_engine"))
        / "config"
        / "clearance_engine.yaml"
    )
    parameters_file = LaunchConfiguration("parameters_file")
    clearance_cpu_affinity = LaunchConfiguration("clearance_cpu_affinity")

    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "parameters_file",
                default_value=str(default_parameters),
                description="原始本体系最低可信点簇净空算法参数文件绝对路径",
            ),
            DeclareLaunchArgument(
                "clearance_cpu_affinity",
                default_value="6,7",
                description="原始点簇净空节点使用的RK3588大核编号",
            ),
            Node(
                package="clearance_engine",
                executable="clearance_engine_node",
                name="clearance_engine_node",
                output="screen",
                parameters=[parameters_file],
                prefix=["taskset -c ", clearance_cpu_affinity],
            ),
        ]
    )
