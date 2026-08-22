import time
import unittest

import launch
import launch_ros.actions
import launch_testing.actions
import pytest
import rclpy
from builtin_interfaces.msg import Time
from diagnostic_msgs.msg import DiagnosticArray
from interfaces.msg import DebugFrameContext
from nav_msgs.msg import Odometry
from rcl_interfaces.srv import DescribeParameters, GetParameters
from rclpy.qos import QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import PointCloud2, PointField
from sensor_msgs_py import point_cloud2
from std_msgs.msg import Header


@pytest.mark.launch_test
def generate_test_description():
    node = launch_ros.actions.Node(
        package="motion_compensation",
        executable="enu_cloud_transform_node",
        output="screen",
    )
    return launch.LaunchDescription(
        [node, launch_testing.actions.ReadyToTest()]
    ), {"transform_node": node}


def add_nanoseconds(stamp: Time, delta_ns: int) -> Time:
    total = stamp.sec * 1_000_000_000 + stamp.nanosec + delta_ns
    return Time(sec=total // 1_000_000_000, nanosec=total % 1_000_000_000)


class TestEnuCloudTransformNode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def setUp(self):
        self.node = rclpy.create_node("enu_cloud_transform_node_test")

    def tearDown(self):
        self.node.destroy_node()

    def test_identity_odometry_pose_preserves_radar_coordinates(self):
        reliable = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
        cloud_publisher = self.node.create_publisher(
            PointCloud2, "/capture/lidar/points_raw", reliable
        )
        odometry_publisher = self.node.create_publisher(
            Odometry, "/capture/odometry/high_rate", reliable
        )
        outputs = []
        contexts = []
        self.node.create_subscription(
            PointCloud2,
            "/capture/lidar/points_compensated_enu",
            outputs.append,
            reliable,
        )
        self.node.create_subscription(
            DebugFrameContext,
            "/capture/debug/frame_context",
            contexts.append,
            reliable,
        )

        discovery_deadline = time.monotonic() + 5.0
        while time.monotonic() < discovery_deadline and (
            cloud_publisher.get_subscription_count() == 0
            or odometry_publisher.get_subscription_count() == 0
        ):
            rclpy.spin_once(self.node, timeout_sec=0.05)
        self.assertGreater(cloud_publisher.get_subscription_count(), 0)
        self.assertGreater(odometry_publisher.get_subscription_count(), 0)

        base_stamp = self.node.get_clock().now().to_msg()
        # 发布完整帧所需的位姿覆盖；RECOVERING状态本身不再阻塞点云。
        for delta_ns in range(0, 131_000_000, 10_000_000):
            odometry = Odometry()
            odometry.header.stamp = add_nanoseconds(base_stamp, delta_ns)
            odometry.pose.pose.orientation.w = 1.0
            odometry_publisher.publish(odometry)
            time.sleep(0.002)

        fields = [
            PointField(name="x", offset=0, datatype=PointField.FLOAT32, count=1),
            PointField(name="y", offset=4, datatype=PointField.FLOAT32, count=1),
            PointField(name="z", offset=8, datatype=PointField.FLOAT32, count=1),
            PointField(
                name="offset_time", offset=12, datatype=PointField.FLOAT32, count=1
            ),
        ]
        cloud = point_cloud2.create_cloud(
            Header(stamp=base_stamp, frame_id="lidar_raw"),
            fields,
            [(1.0, 2.0, 3.0, 0.005)],
        )

        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline and (not outputs or not contexts):
            cloud_publisher.publish(cloud)
            rclpy.spin_once(self.node, timeout_sec=0.1)

        self.assertTrue(outputs, "节点未发布补偿后的ENU点云")
        self.assertEqual(outputs[-1].header.frame_id, "lidar_local_enu")
        points = list(
            point_cloud2.read_points(
                outputs[-1], field_names=("x", "y", "z"), skip_nans=False
            )
        )
        self.assertEqual(len(points), 1)
        self.assertAlmostEqual(float(points[0][0]), 1.0, places=6)
        self.assertAlmostEqual(float(points[0][1]), 2.0, places=6)
        self.assertAlmostEqual(float(points[0][2]), 3.0, places=6)
        self.assertTrue(contexts, "节点未发布帧级调试上下文")
        context = contexts[-1]
        self.assertGreater(context.cloud_sequence, 0)
        self.assertTrue(context.valid)
        self.assertFalse(context.partial)
        self.assertEqual(context.compensation_mode, DebugFrameContext.MODE_FULL_SE3)
        self.assertEqual(context.raw_point_count, 1)
        self.assertEqual(context.transformed_point_count, 1)
        self.assertEqual(context.invalid_reason, "")

    def test_default_poll_interval_and_diagnostics_fields(self):
        parameter_client = self.node.create_client(
            GetParameters, "/enu_cloud_transform_node/get_parameters"
        )
        self.assertTrue(parameter_client.wait_for_service(timeout_sec=5.0))
        request = GetParameters.Request(
            names=["processing_poll_interval_ms"]
        )
        future = parameter_client.call_async(request)
        rclpy.spin_until_future_complete(self.node, future, timeout_sec=5.0)
        self.assertIsNotNone(future.result())
        self.assertEqual(future.result().values[0].integer_value, 10)

        descriptor_client = self.node.create_client(
            DescribeParameters,
            "/enu_cloud_transform_node/describe_parameters",
        )
        self.assertTrue(descriptor_client.wait_for_service(timeout_sec=5.0))
        descriptor_future = descriptor_client.call_async(
            DescribeParameters.Request(names=["processing_poll_interval_ms"])
        )
        rclpy.spin_until_future_complete(
            self.node, descriptor_future, timeout_sec=5.0
        )
        descriptor = descriptor_future.result().descriptors[0]
        self.assertTrue(descriptor.read_only)
        self.assertEqual(descriptor.integer_range[0].from_value, 1)
        self.assertEqual(descriptor.integer_range[0].to_value, 100)
        self.assertEqual(descriptor.integer_range[0].step, 1)

        diagnostics = []
        self.node.create_subscription(
            DiagnosticArray, "/diagnostics", diagnostics.append, 10
        )
        deadline = time.monotonic() + 5.0
        matching_status = None
        while time.monotonic() < deadline and matching_status is None:
            rclpy.spin_once(self.node, timeout_sec=0.1)
            for message in diagnostics:
                matching_status = next(
                    (
                        status
                        for status in message.status
                        if status.name == "motion_compensation/enu_cloud_transform"
                    ),
                    None,
                )
                if matching_status is not None:
                    break
        self.assertIsNotNone(matching_status)
        values = {item.key: item.value for item in matching_status.values}
        expected_fields = {
            "motion_state",
            "state_reason",
            "processing_poll_interval_ms",
            "max_cloud_wait_ms",
            "pose_stream_age_ms",
            "continuous_pose_duration_ms",
            "max_pose_gap_ms",
            "cloud_start_stamp_ns",
            "cloud_end_stamp_ns",
            "newest_pose_stamp_ns",
            "cloud_pose_lag_ms",
            "cloud_wait_ms_last",
            "pending_cloud_count",
            "pending_oldest_age_ms",
            "pending_cloud_max_count",
            "clouds_received_total",
            "clouds_processed_total",
            "clouds_dropped_total",
            "clouds_dropped_pose_gap_total",
            "clouds_dropped_timeout_total",
            "pose_gap_count",
            "recovery_count",
            "full_se3_frames_total",
            "rotation_only_frames_total",
            "pose_wait_count",
            "interpolation_failure_count",
            "queue_wait_ms_last",
            "queue_wait_ms_mean",
            "queue_wait_ms_max",
            "processing_time_ms_last",
            "processing_time_ms_mean",
            "processing_time_ms_max",
        }
        self.assertTrue(expected_fields.issubset(values))
        self.assertEqual(values["processing_poll_interval_ms"], "10")
