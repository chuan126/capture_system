#!/usr/bin/env python3

"""输出一次最低净空原始点及其对应的 IMU、里程计数据。"""

import argparse
from collections import deque
import math
import sys
import time
from typing import Deque, Optional, Tuple, TypeVar

import rclpy
from interfaces.msg import ClearanceResult
from nav_msgs.msg import Odometry
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import Imu, PointCloud2
from sensor_msgs_py import point_cloud2


MessageT = TypeVar("MessageT")
Nanoseconds = int


def stamp_to_nanoseconds(stamp) -> Nanoseconds:
    return int(stamp.sec) * 1_000_000_000 + int(stamp.nanosec)


class MinimumPointMotionInspector(Node):
    def __init__(self, max_time_difference_ms: float) -> None:
        super().__init__("minimum_point_motion_inspector")
        self.max_time_difference_ns = int(max_time_difference_ms * 1_000_000.0)
        self.done = False
        self.last_invalid_reason = "no clearance result received"

        # 点云只保留少量完整帧，IMU 和里程计缓存覆盖数秒，避免无界占用内存。
        self.cloud_cache: Deque[PointCloud2] = deque(maxlen=4)
        self.imu_cache: Deque[Imu] = deque(maxlen=2000)
        self.odom_cache: Deque[Odometry] = deque(maxlen=2000)
        self.pending_result: Optional[ClearanceResult] = None

        reliable_qos = QoSProfile(
            depth=10,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.VOLATILE,
        )
        cloud_qos = QoSProfile(
            depth=2,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.VOLATILE,
        )

        self.create_subscription(
            PointCloud2,
            "/capture/lidar/points_raw",
            self._cloud_callback,
            cloud_qos,
        )
        self.create_subscription(
            ClearanceResult,
            "/capture/clearance/result",
            self._clearance_callback,
            reliable_qos,
        )
        self.create_subscription(
            Imu,
            "/capture/imu/data",
            self._imu_callback,
            reliable_qos,
        )
        self.create_subscription(
            Odometry,
            "/capture/odometry/high_rate",
            self._odom_callback,
            reliable_qos,
        )

    def _cloud_callback(self, message: PointCloud2) -> None:
        self.cloud_cache.append(message)
        self._try_output()

    def _imu_callback(self, message: Imu) -> None:
        self.imu_cache.append(message)
        self._try_output()

    def _odom_callback(self, message: Odometry) -> None:
        self.odom_cache.append(message)
        self._try_output()

    def _clearance_callback(self, message: ClearanceResult) -> None:
        if not message.valid:
            self.last_invalid_reason = message.invalid_reason or "invalid clearance result"
            return

        fitted_position = (
            message.lidar_to_top_m,
            message.minimum_position_y_m,
            message.minimum_position_z_m,
        )
        if not all(math.isfinite(value) for value in fitted_position):
            self.last_invalid_reason = "clearance result contains non-finite coordinates"
            return

        self.pending_result = message
        self._try_output()

    @staticmethod
    def _find_cloud(
        cache: Deque[PointCloud2], target_stamp_ns: Nanoseconds
    ) -> Optional[PointCloud2]:
        # clearance_engine 会原样复制输入点云时间戳，因此这里要求精确匹配。
        for message in reversed(cache):
            if stamp_to_nanoseconds(message.header.stamp) == target_stamp_ns:
                return message
        return None

    @staticmethod
    def _find_nearest(
        cache: Deque[MessageT], target_stamp_ns: Nanoseconds
    ) -> Tuple[Optional[MessageT], Optional[int]]:
        if not cache:
            return None, None

        nearest = min(
            cache,
            key=lambda message: abs(
                stamp_to_nanoseconds(message.header.stamp) - target_stamp_ns
            ),
        )
        difference_ns = stamp_to_nanoseconds(nearest.header.stamp) - target_stamp_ns
        return nearest, difference_ns

    @staticmethod
    def _find_nearest_raw_point(
        cloud: PointCloud2, target: Tuple[float, float, float]
    ) -> Tuple[Optional[Tuple[float, float, float]], float]:
        nearest_point: Optional[Tuple[float, float, float]] = None
        nearest_distance_squared = math.inf
        target_x, target_y, target_z = target

        for point in point_cloud2.read_points(
            cloud, field_names=("x", "y", "z"), skip_nans=True
        ):
            x, y, z = float(point[0]), float(point[1]), float(point[2])
            if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(z)):
                continue
            if x == 0.0 and y == 0.0 and z == 0.0:
                continue

            distance_squared = (
                (x - target_x) ** 2
                + (y - target_y) ** 2
                + (z - target_z) ** 2
            )
            if distance_squared < nearest_distance_squared:
                nearest_distance_squared = distance_squared
                nearest_point = (x, y, z)

        return nearest_point, math.sqrt(nearest_distance_squared)

    def _try_output(self) -> None:
        if self.done or self.pending_result is None:
            return

        result = self.pending_result
        target_stamp_ns = stamp_to_nanoseconds(result.header.stamp)
        cloud = self._find_cloud(self.cloud_cache, target_stamp_ns)
        imu, imu_difference_ns = self._find_nearest(self.imu_cache, target_stamp_ns)
        odom, odom_difference_ns = self._find_nearest(self.odom_cache, target_stamp_ns)

        if cloud is None or imu is None or odom is None:
            return
        if imu_difference_ns is None or odom_difference_ns is None:
            return
        if abs(imu_difference_ns) > self.max_time_difference_ns:
            return
        if abs(odom_difference_ns) > self.max_time_difference_ns:
            return

        fitted_position = (
            float(result.lidar_to_top_m),
            float(result.minimum_position_y_m),
            float(result.minimum_position_z_m),
        )
        raw_point, _ = self._find_nearest_raw_point(cloud, fitted_position)
        if raw_point is None:
            self.last_invalid_reason = "matched cloud contains no valid XYZ point"
            self.pending_result = None
            return

        self._print_result(
            raw_point,
            imu,
            odom,
        )
        self.done = True

    @staticmethod
    def _print_result(
        raw_point: Tuple[float, float, float],
        imu: Imu,
        odom: Odometry,
    ) -> None:
        angular = imu.angular_velocity
        acceleration = imu.linear_acceleration
        quaternion = odom.pose.pose.orientation

        # 运行时标签仅使用 ASCII，避免远程终端缺少 CJK 字形时出现空白字符。
        print("\nMinimum raw point coordinates (m):")
        print(f"  x: {raw_point[0]:.6f}")
        print(f"  y: {raw_point[1]:.6f}")
        print(f"  z: {raw_point[2]:.6f}")
        print("\nIMU:")
        print("  angular_velocity rad/s:")
        print(f"    x: {angular.x:.6f}")
        print(f"    y: {angular.y:.6f}")
        print(f"    z: {angular.z:.6f}")
        print("  linear_acceleration m/s^2:")
        print(f"    x: {acceleration.x:.6f}")
        print(f"    y: {acceleration.y:.6f}")
        print(f"    z: {acceleration.z:.6f}")
        print("\nOdometry quaternion (x, y, z, w):")
        print(f"  x: {quaternion.x:.6f}")
        print(f"  y: {quaternion.y:.6f}")
        print(f"  z: {quaternion.z:.6f}")
        print(f"  w: {quaternion.w:.6f}")


def parse_arguments() -> Tuple[argparse.Namespace, list]:
    parser = argparse.ArgumentParser(
        description="Print one matched clearance, raw point, IMU and odometry sample"
    )
    parser.add_argument(
        "--timeout-sec",
        type=float,
        default=30.0,
        help="seconds to wait for a complete matched sample (default: 30)",
    )
    parser.add_argument(
        "--max-time-diff-ms",
        type=float,
        default=20.0,
        help="maximum IMU/odometry time difference in ms (default: 20)",
    )
    arguments, ros_arguments = parser.parse_known_args()
    if arguments.timeout_sec <= 0.0:
        parser.error("--timeout-sec must be greater than zero")
    if arguments.max_time_diff_ms <= 0.0:
        parser.error("--max-time-diff-ms must be greater than zero")
    return arguments, ros_arguments


def main() -> int:
    arguments, ros_arguments = parse_arguments()
    rclpy.init(args=ros_arguments)
    node = MinimumPointMotionInspector(arguments.max_time_diff_ms)
    deadline = time.monotonic() + arguments.timeout_sec

    try:
        while rclpy.ok() and not node.done and time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.1)
    except KeyboardInterrupt:
        print("\nInterrupted by user.", file=sys.stderr)
        return_code = 130
    else:
        if node.done:
            return_code = 0
        else:
            print(f"Timed out. Last status: {node.last_invalid_reason}", file=sys.stderr)
            return_code = 1
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()

    return return_code


if __name__ == "__main__":
    sys.exit(main())
