from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description() -> LaunchDescription:
    default_parameters = (
        Path(get_package_share_directory("cloud_visualization"))
        / "config"
        / "cloud_visualization.yaml"
    )
    parameters_file = LaunchConfiguration("parameters_file")

    # 预览节点单独编排，关闭或故障不会影响雷达和核心测量节点。
    preview_node = Node(
        package="cloud_visualization",
        executable="cloud_visualization_node",
        name="cloud_visualization_node",
        output="screen",
        parameters=[parameters_file],
    )

    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "parameters_file",
                default_value=str(default_parameters),
                description="点云预览节点参数文件绝对路径",
            ),
            preview_node,
        ]
    )
