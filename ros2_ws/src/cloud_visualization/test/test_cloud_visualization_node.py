import time
import unittest

import launch
import launch_ros.actions
import launch_testing.actions
import pytest
import rclpy
from interfaces.msg import RawClearanceDiagnostics
from rclpy.qos import QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import PointCloud2
from sensor_msgs_py import point_cloud2
from std_msgs.msg import Header


@pytest.mark.launch_test
def generate_test_description():
    node = launch_ros.actions.Node(
        package="cloud_visualization",
        executable="cloud_visualization_node",
        output="screen",
        parameters=[{
            "publish_rate_hz": 10.0,
            "max_points": 500,
            "expected_frame_id": "lidar_local_enu",
            "input_topic": "/capture/test/cloud_visualization/input",
            "output_topic": "/capture/test/cloud_visualization/output",
            "diagnostics_topic": "/capture/test/cloud_visualization/diagnostics",
        }],
    )
    return launch.LaunchDescription(
        [node, launch_testing.actions.ReadyToTest()]
    ), {"cloud_visualization_node": node}


class TestCloudVisualizationNode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def setUp(self):
        self.node = rclpy.create_node("cloud_visualization_node_test")
        reliable = QoSProfile(depth=16, reliability=ReliabilityPolicy.RELIABLE)
        best_effort = QoSProfile(depth=16, reliability=ReliabilityPolicy.BEST_EFFORT)
        self.cloud_publisher = self.node.create_publisher(
            PointCloud2, "/capture/test/cloud_visualization/input", reliable
        )
        self.diagnostics_publisher = self.node.create_publisher(
            RawClearanceDiagnostics, "/capture/test/cloud_visualization/diagnostics", best_effort
        )
        self.outputs = []
        self.node.create_subscription(
            PointCloud2,
            "/capture/test/cloud_visualization/output",
            self.outputs.append,
            QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT),
        )

    def tearDown(self):
        self.node.destroy_node()

    @staticmethod
    def header(stamp_ns: int) -> Header:
        header = Header(frame_id="lidar_local_enu")
        header.stamp.nanosec = stamp_ns
        return header

    @classmethod
    def cloud(cls, stamp_ns: int) -> PointCloud2:
        return point_cloud2.create_cloud_xyz32(
            cls.header(stamp_ns),
            [(float(index + 1), float(index % 2), 0.25) for index in range(6)],
        )

    @classmethod
    def diagnostics(cls, stamp_ns: int, *, measurement_valid: bool = True):
        message = RawClearanceDiagnostics()
        message.header = cls.header(stamp_ns)
        message.input_point_count = 6
        message.roi_point_indices = [1, 2, 3]
        message.lowest_cluster_point_indices = [2, 3]
        message.lowest_cluster_valid = measurement_valid
        message.measurement_valid = measurement_valid
        message.lowest_cluster_median_x = 2.0
        return message

    @staticmethod
    def classifications(message: PointCloud2):
        assert message.point_step == 16
        return [message.data[index * message.point_step + 12] for index in range(message.width)]

    def wait_for_output(self, stamp_ns: int, timeout=5.0):
        deadline = time.monotonic() + timeout
        matched = []
        while time.monotonic() < deadline and not matched:
            rclpy.spin_once(self.node, timeout_sec=0.05)
            matched = [
                output for output in self.outputs
                if output.header.stamp.nanosec == stamp_ns
            ]
        self.assertTrue(matched, "预览节点未发布目标时间戳点云")
        return matched[-1]

    def publish_repeated(self, publisher, message, duration=0.4):
        discovery_deadline = time.monotonic() + 5.0
        while time.monotonic() < discovery_deadline and publisher.get_subscription_count() == 0:
            rclpy.spin_once(self.node, timeout_sec=0.05)
        self.assertGreater(publisher.get_subscription_count(), 0)
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            publisher.publish(message)
            rclpy.spin_once(self.node, timeout_sec=0.02)

    def publish_cloud_once_and_wait(self, message: PointCloud2):
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and self.cloud_publisher.get_subscription_count() == 0:
            rclpy.spin_once(self.node, timeout_sec=0.05)
        self.assertGreater(self.cloud_publisher.get_subscription_count(), 0)
        self.cloud_publisher.publish(message)
        return self.wait_for_output(message.header.stamp.nanosec)

    def test_exact_stamp_classification_survives_interleaved_diagnostics(self):
        # 先缓存目标帧诊断，再插入另一帧诊断，不能用“最新诊断”错误覆盖目标帧。
        self.publish_repeated(self.diagnostics_publisher, self.diagnostics(101), 0.2)
        self.publish_repeated(self.diagnostics_publisher, self.diagnostics(102), 0.2)
        output = self.publish_cloud_once_and_wait(self.cloud(101))

        self.assertEqual(self.classifications(output), [0, 1, 2, 2, 0, 0])

    def test_valid_lowest_cluster_is_always_red_and_invalid_cluster_is_not_red(self):
        self.publish_repeated(self.diagnostics_publisher, self.diagnostics(201), 0.2)
        valid = self.publish_cloud_once_and_wait(self.cloud(201))
        self.assertEqual(self.classifications(valid), [0, 1, 2, 2, 0, 0])

        self.publish_repeated(
            self.diagnostics_publisher,
            self.diagnostics(202, measurement_valid=False),
            0.2,
        )
        invalid = self.publish_cloud_once_and_wait(self.cloud(202))
        self.assertEqual(self.classifications(invalid), [0, 1, 1, 1, 0, 0])

    def test_late_diagnostics_recolors_latest_frame(self):
        first = self.publish_cloud_once_and_wait(self.cloud(301))
        self.assertEqual(self.classifications(first), [0, 0, 0, 0, 0, 0])

        previous_count = len(self.outputs)
        self.publish_repeated(self.diagnostics_publisher, self.diagnostics(301), 0.2)
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and len(self.outputs) <= previous_count:
            rclpy.spin_once(self.node, timeout_sec=0.05)
        self.assertGreater(len(self.outputs), previous_count)
        self.assertEqual(self.classifications(self.outputs[-1]), [0, 1, 2, 2, 0, 0])
