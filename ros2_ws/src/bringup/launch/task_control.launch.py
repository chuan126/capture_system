import os
from pathlib import Path
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description() -> LaunchDescription:
    data_root = LaunchConfiguration("data_root")
    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "data_root",
                default_value=os.getenv("CAPTURE_DATA_ROOT", str(Path.cwd() / "runtime")),
                description="任务索引和每任务测量文件的设备端数据目录",
            ),
            Node(
                package="data_recorder",
                executable="data_recorder_node",
                name="data_recorder_node",
                output="screen",
                parameters=[
                    {
                        "data_root": data_root,
                        "clearance_topic": "/capture/clearance/result",
                        "rtk_fix_topic": "/capture/rtk/fix",
                        "rtk_status_topic": "/capture/rtk/status",
                        "sample_rate_hz": 50.0,
                        "source_timeout_ms": 250.0,
                        "endpoint_rtk_max_age_ms": 2000.0,
                        "transaction_batch_size": 100,
                        "software_version": "0.2.0",
                        "algorithm_version": "clearance_engine-current",
                        "config_version": "clearance_engine_small_board_1cm.yaml",
                    }
                ],
            ),
            Node(
                package="task_manager",
                executable="task_manager_node",
                name="task_manager_node",
                output="screen",
                parameters=[
                    {
                        "data_root": data_root,
                        "recorder_prepare_service": "/capture/recording/prepare",
                        "recorder_control_service": "/capture/recording/control",
                        "recorder_service_timeout_ms": 10000,
                    }
                ],
            ),
        ]
    )
