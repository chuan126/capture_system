import time
import unittest

import launch
import launch_ros.actions
import launch_testing.actions
import pytest
import rclpy
from builtin_interfaces.msg import Time
from nav_msgs.msg import Odometry
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

    def test_applies_default_c0_for_identity_odometry_pose(self):
        reliable = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
        cloud_publisher = self.node.create_publisher(
            PointCloud2, "/capture/lidar/points_raw", reliable
        )
        odometry_publisher = self.node.create_publisher(
            Odometry, "/capture/odometry/high_rate", reliable
        )
        outputs = []
        self.node.create_subscription(
            PointCloud2,
            "/capture/lidar/points_compensated_enu",
            outputs.append,
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
        for delta_ns in (0, 10_000_000):
            odometry = Odometry()
            odometry.header.stamp = add_nanoseconds(base_stamp, delta_ns)
            odometry.pose.pose.orientation.w = 1.0
            odometry_publisher.publish(odometry)
            rclpy.spin_once(self.node, timeout_sec=0.05)

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
        while time.monotonic() < deadline and not outputs:
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
        self.assertAlmostEqual(float(points[0][0]), -1.0, places=6)
        self.assertAlmostEqual(float(points[0][1]), -2.0, places=6)
        self.assertAlmostEqual(float(points[0][2]), 3.0, places=6)
