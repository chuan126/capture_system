import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    package_share = get_package_share_directory("sensor_adapter")
    driver_launch = os.path.join(package_share, "launch", "odin_driver.launch.py")
    default_rviz_config = os.path.join(
        package_share, "rviz", "capture_odin_preview.rviz"
    )

    vendor_device_prefix = LaunchConfiguration("vendor_device_prefix")
    topic_prefix = LaunchConfiguration("topic_prefix")
    image_width = LaunchConfiguration("image_width")
    image_height = LaunchConfiguration("image_height")
    image_fps = LaunchConfiguration("image_fps")
    image_format = LaunchConfiguration("image_format")
    enable_raw_point = LaunchConfiguration("enable_raw_point")
    enable_slam_point = LaunchConfiguration("enable_slam_point")
    enable_image0 = LaunchConfiguration("enable_image0")
    enable_image1 = LaunchConfiguration("enable_image1")
    enable_imu = LaunchConfiguration("enable_imu")
    enable_odom = LaunchConfiguration("enable_odom")
    enable_slam_odom_sync = LaunchConfiguration("enable_slam_odom_sync")
    rviz_config = LaunchConfiguration("rviz_config")

    driver = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(driver_launch),
        launch_arguments={
            "vendor_device_prefix": vendor_device_prefix,
            "topic_prefix": topic_prefix,
            "image_width": image_width,
            "image_height": image_height,
            "image_fps": image_fps,
            "image_format": image_format,
            "enable_raw_point": enable_raw_point,
            "enable_slam_point": enable_slam_point,
            "enable_image0": enable_image0,
            "enable_image1": enable_image1,
            "enable_imu": enable_imu,
            "enable_odom": enable_odom,
            "enable_slam_odom_sync": enable_slam_odom_sync,
        }.items(),
    )

    rviz = Node(
        package="rviz2",
        executable="rviz2",
        name="capture_odin_rviz",
        arguments=["-d", rviz_config],
        output="screen",
    )

    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "vendor_device_prefix",
                default_value="/manifold/ODIN2/device0",
                description="厂商设备 Topic 前缀，不包含末尾斜杠",
            ),
            DeclareLaunchArgument("topic_prefix", default_value="manifold"),
            DeclareLaunchArgument("image_width", default_value="0"),
            DeclareLaunchArgument("image_height", default_value="0"),
            DeclareLaunchArgument("image_fps", default_value="0"),
            DeclareLaunchArgument("image_format", default_value="mjpeg"),
            DeclareLaunchArgument("enable_raw_point", default_value="true"),
            DeclareLaunchArgument("enable_slam_point", default_value="true"),
            DeclareLaunchArgument("enable_image0", default_value="false"),
            DeclareLaunchArgument("enable_image1", default_value="false"),
            DeclareLaunchArgument("enable_imu", default_value="true"),
            DeclareLaunchArgument("enable_odom", default_value="true"),
            DeclareLaunchArgument("enable_slam_odom_sync", default_value="false"),
            DeclareLaunchArgument(
                "rviz_config",
                default_value=default_rviz_config,
                description="RViz2 配置文件",
            ),
            driver,
            rviz,
        ]
    )
