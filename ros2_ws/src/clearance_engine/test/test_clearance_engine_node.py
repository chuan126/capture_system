import time
import unittest

import launch
import launch_ros.actions
import launch_testing.actions
import pytest
import rclpy
from interfaces.msg import ClearanceResult
from rclpy.qos import QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import PointCloud2
from sensor_msgs_py import point_cloud2
from std_msgs.msg import Header


@pytest.mark.launch_test
def generate_test_description():
    node = launch_ros.actions.Node(
        package="clearance_engine",
        executable="clearance_engine_node",
        output="screen",
    )
    return launch.LaunchDescription(
        [node, launch_testing.actions.ReadyToTest()]
    ), {"clearance_node": node}


class TestClearanceEngineNode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def setUp(self):
        self.node = rclpy.create_node("clearance_engine_node_test")

    def tearDown(self):
        self.node.destroy_node()

    def test_publishes_valid_result_for_synthetic_plane(self):
        qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
        publisher = self.node.create_publisher(
            PointCloud2, "/capture/lidar/points_compensated_enu", qos
        )
        results = []
        self.node.create_subscription(
            ClearanceResult,
            "/capture/clearance/result",
            results.append,
            qos,
        )

        points = []
        for east_index in range(-20, 21):
            for north_index in range(-20, 21):
                points.append((east_index * 0.05, north_index * 0.05, 2.0))
        message = point_cloud2.create_cloud_xyz32(
            Header(frame_id="lidar_local_enu"), points
        )

        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline and not results:
            publisher.publish(message)
            rclpy.spin_once(self.node, timeout_sec=0.1)

        self.assertTrue(results, "节点未在10秒内发布净空结果")
        self.assertTrue(results[-1].valid, results[-1].invalid_reason)
        self.assertAlmostEqual(results[-1].lidar_to_top_m, 2.0, delta=0.03)
        self.assertGreaterEqual(results[-1].ransac_plane_count, 1)
        self.assertGreaterEqual(results[-1].surface_count, 0)
        self.assertGreaterEqual(results[-1].candidate_count, 1)
