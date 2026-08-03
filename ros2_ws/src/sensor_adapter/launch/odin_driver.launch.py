from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


TOPIC_REMAPPINGS = (
    ("cloud/raw", "/capture/lidar/points_raw"),
    ("cloud/slam", "/capture/lidar/points_slam"),
    ("imu", "/capture/imu/data"),
    ("odometry_hf", "/capture/odometry/high_rate"),
    ("odometry", "/capture/odometry/slam"),
)

DRIVER_EVENT_REMAPPINGS = (
    ("device_online", "/capture/lidar/device_online"),
    ("device_offline", "/capture/lidar/device_offline"),
)


def generate_launch_description():
    vendor_device_prefix = LaunchConfiguration("vendor_device_prefix")
    topic_prefix = LaunchConfiguration("topic_prefix")
    image_width = LaunchConfiguration("image_width")
    image_height = LaunchConfiguration("image_height")
    image_fps = LaunchConfiguration("image_fps")
    image_format = LaunchConfiguration("image_format")

    # remap 直接作用于厂商发布器，不创建中继节点，也不复制点云消息。
    remappings = [
        ([vendor_device_prefix, "/", source_suffix], target_topic)
        for source_suffix, target_topic in TOPIC_REMAPPINGS
    ]
    remappings.extend(
        (
            ["/", topic_prefix, "/driver/", source_suffix],
            target_topic,
        )
        for source_suffix, target_topic in DRIVER_EVENT_REMAPPINGS
    )

    driver = Node(
        package="odin_ros_driver_rev1",
        executable="odin_ros_driver_node",
        output="screen",
        parameters=[
            {
                "operating_mode": "normal",
                "image_width": image_width,
                "image_height": image_height,
                "image_fps": image_fps,
                "image_format": image_format,
                "topic_prefix": topic_prefix,
            }
        ],
        remappings=remappings,
    )

    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "vendor_device_prefix",
                default_value="/manifold/ODIN2/device0",
                description="厂商设备 Topic 前缀，不包含末尾斜杠",
            ),
            DeclareLaunchArgument(
                "topic_prefix",
                default_value="manifold",
                description="传递给 ODIN 驱动的顶层 Topic 前缀",
            ),
            DeclareLaunchArgument("image_width", default_value="0"),
            DeclareLaunchArgument("image_height", default_value="0"),
            DeclareLaunchArgument("image_fps", default_value="0"),
            DeclareLaunchArgument("image_format", default_value="mjpeg"),
            driver,
        ]
    )
