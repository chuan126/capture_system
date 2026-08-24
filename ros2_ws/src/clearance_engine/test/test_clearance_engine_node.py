import math
import time
import unittest

import launch
import launch_ros.actions
import launch_testing.actions
import pytest
import rclpy
from interfaces.msg import ClearanceResult, RawClearanceDiagnostics
from rcl_interfaces.srv import SetParameters
from rclpy.parameter import Parameter
from rclpy.qos import QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import PointCloud2, PointField
from sensor_msgs_py import point_cloud2
from std_msgs.msg import Header


@pytest.mark.launch_test
def generate_test_description():
    node = launch_ros.actions.Node(
        package="clearance_engine",
        executable="clearance_engine_node",
        output="screen",
        parameters=[{
            "input_topic": "/capture/test/clearance_engine/input",
            "output_topic": "/capture/test/clearance_engine/output",
            "diagnostics_topic": "/capture/test/clearance_engine/diagnostics",
            "raw_cluster.min_detection_x_m": 0.2,
            "raw_cluster.max_detection_x_m": 10.0,
            "raw_cluster.detection_radius_m": 1.0,
            "raw_cluster.support_height_band_m": 0.05,
            "raw_cluster.min_support_points": 4,
            "raw_cluster.spatial_grid_size_m": 0.1,
            "raw_cluster.min_occupied_cells": 3,
            "raw_cluster.min_spatial_span_m": 0.2,
        }],
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
        self.qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
        self.publisher = self.node.create_publisher(
            PointCloud2, "/capture/test/clearance_engine/input", self.qos
        )
        self.results = []
        self.diagnostics = []
        self.node.create_subscription(
            ClearanceResult, "/capture/test/clearance_engine/output", self.results.append, self.qos
        )
        self.node.create_subscription(
            RawClearanceDiagnostics,
            "/capture/test/clearance_engine/diagnostics",
            self.diagnostics.append,
            QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT),
        )

    def tearDown(self):
        self.node.destroy_node()

    @staticmethod
    def cluster(height: float, y_offset: float = 0.0):
        support = (
            (0.01, 0.01), (0.12, 0.01), (0.23, 0.01),
            (0.01, 0.12), (0.12, 0.12), (0.23, 0.12),
        )
        return [
            (height + (index % 3) * 0.01, y + y_offset, z)
            for index, (y, z) in enumerate(support)
        ]

    @staticmethod
    def cloud(points, *, frame_id="device0/odom", stamp_ns=1):
        header = Header(frame_id=frame_id)
        header.stamp.nanosec = stamp_ns
        return point_cloud2.create_cloud_xyz32(header, points)

    def publish_until_result(self, message: PointCloud2, timeout=5.0):
        target = (message.header.stamp.sec, message.header.stamp.nanosec)
        deadline = time.monotonic() + timeout
        matched = []
        while time.monotonic() < deadline and not matched:
            self.publisher.publish(message)
            rclpy.spin_once(self.node, timeout_sec=0.05)
            matched = [
                result for result in self.results
                if (result.header.stamp.sec, result.header.stamp.nanosec) == target
            ]
        self.assertTrue(matched, "节点未发布目标时间戳的净空结果")
        return matched[-1]

    def set_radius(self, radius: float):
        client = self.node.create_client(
            SetParameters, "/clearance_engine_node/set_parameters"
        )
        self.assertTrue(client.wait_for_service(timeout_sec=5.0))
        request = SetParameters.Request()
        request.parameters = [
            Parameter("raw_cluster.detection_radius_m", value=radius).to_parameter_msg()
        ]
        future = client.call_async(request)
        rclpy.spin_until_future_complete(self.node, future, timeout_sec=5.0)
        self.assertTrue(future.done())
        self.assertTrue(future.result().results[0].successful)

    def test_valid_raw_cluster_publishes_median_and_original_indices(self):
        points = [(1.0, 0.0, 0.0), *self.cluster(2.0)]
        result = self.publish_until_result(self.cloud(points, stamp_ns=11))

        self.assertTrue(result.valid, result.invalid_reason)
        self.assertAlmostEqual(result.lidar_to_top_m, 2.01, delta=1e-5)
        self.assertEqual(result.ransac_plane_count, 0)
        self.assertEqual(result.surface_count, 0)
        self.assertEqual(result.candidate_count, 1)
        self.assertEqual(result.selected_inlier_count, 6)
        self.assertTrue(math.isnan(result.minimum_position_east_m))
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline and not self.diagnostics:
            rclpy.spin_once(self.node, timeout_sec=0.05)
        self.assertTrue(self.diagnostics)
        diagnostic = self.diagnostics[-1]
        self.assertEqual(diagnostic.header.stamp.nanosec, 11)
        self.assertEqual(diagnostic.input_point_count, 7)
        self.assertEqual(diagnostic.radius_roi_point_count, 7)
        self.assertEqual(list(diagnostic.lowest_cluster_point_indices), list(range(1, 7)))
        self.assertTrue(diagnostic.measurement_valid)

    def test_invalid_layouts_and_frame_publish_explicit_invalid_result(self):
        cases = []
        cases.append(self.cloud(self.cluster(2.0), frame_id="", stamp_ns=20))

        missing_z = point_cloud2.create_cloud(
            Header(frame_id="device0/odom"),
            [
                PointField(name="x", offset=0, datatype=PointField.FLOAT32, count=1),
                PointField(name="y", offset=4, datatype=PointField.FLOAT32, count=1),
            ],
            [(2.0, 0.0)],
        )
        missing_z.header.stamp.nanosec = 21
        cases.append(missing_z)

        big_endian = self.cloud(self.cluster(2.0), stamp_ns=22)
        big_endian.is_bigendian = True
        cases.append(big_endian)

        corrupt = self.cloud(self.cluster(2.0), stamp_ns=23)
        corrupt.data = corrupt.data[:-1]
        cases.append(corrupt)

        expected_reasons = [
            "INVALID_POINT_CLOUD_FRAME",
            "INVALID_POINT_CLOUD_LAYOUT",
            "INVALID_POINT_CLOUD_LAYOUT",
            "INVALID_POINT_CLOUD_LAYOUT",
        ]
        for message, reason in zip(cases, expected_reasons, strict=True):
            result = self.publish_until_result(message)
            self.assertFalse(result.valid)
            self.assertEqual(result.invalid_reason, reason)
            self.assertTrue(math.isnan(result.lidar_to_top_m))

    def test_dynamic_radius_changes_roi_and_invalid_frame_does_not_reuse(self):
        points = self.cluster(2.0, y_offset=0.60)
        self.set_radius(1.0)
        valid = self.publish_until_result(self.cloud(points, stamp_ns=31))
        self.assertTrue(valid.valid, valid.invalid_reason)

        self.set_radius(0.5)
        invalid = self.publish_until_result(self.cloud(points, stamp_ns=32))
        self.assertFalse(invalid.valid)
        self.assertEqual(invalid.invalid_reason, "NO_POINTS_IN_CYLINDRICAL_ROI")
        self.assertTrue(math.isnan(invalid.lidar_to_top_m))
        self.assertEqual(invalid.selected_inlier_count, 0)
        self.set_radius(1.0)
