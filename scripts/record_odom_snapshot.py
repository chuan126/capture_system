#!/usr/bin/env python3
"""记录一次当前里程计坐标快照。

用法:
  python3 scripts/record_odom_snapshot.py

选项:
  --topic TOPIC   订阅的里程计话题（默认 /capture/odometry/high_rate）
  --timeout SEC   等待超时秒数（默认 5.0）
  --no-save       只打印到终端，不写入文件
"""

import argparse
import os
import sys
import time

import rclpy
from nav_msgs.msg import Odometry
from rclpy.node import Node


class OdometrySnapshot(Node):
    """订阅里程计话题，收到第一条消息后记录并退出。"""

    def __init__(self, topic: str, timeout: float, save_to_file: bool) -> None:
        super().__init__("odometry_snapshot")
        self._timeout = timeout
        self._save_to_file = save_to_file
        self._received = False
        self._msg: Odometry | None = None

        self._sub = self.create_subscription(
            Odometry,
            topic,
            self._on_odometry,
            rclpy.qos.QoSProfile(
                depth=1,
                reliability=rclpy.qos.ReliabilityPolicy.BEST_EFFORT,
                durability=rclpy.qos.DurabilityPolicy.VOLATILE,
            ),
        )
        self.get_logger().info(f"等待里程计消息（话题: {topic}，超时: {timeout}s）…")

    def _on_odometry(self, msg: Odometry) -> None:
        if self._received:
            return
        self._received = True
        self._msg = msg

    def run(self) -> Odometry | None:
        deadline = time.monotonic() + self._timeout
        while rclpy.ok() and not self._received:
            rclpy.spin_once(self, timeout_sec=0.1)
            if time.monotonic() > deadline:
                self.get_logger().error(f"超时：{self._timeout}s 内未收到里程计消息")
                return None
        return self._msg


def format_snapshot(msg: Odometry, topic: str) -> str:
    """把一条 Odometry 消息格式化为可读文本。"""
    pos = msg.pose.pose.position
    ori = msg.pose.pose.orientation
    stamp_sec = msg.header.stamp.sec + msg.header.stamp.nanosec * 1e-9

    lines = [
        f"话题:      {topic}",
        f"时间戳:    {stamp_sec:.9f}",
        f"frame_id:  {msg.header.frame_id}",
        f"child_frame_id: {msg.child_frame_id}",
        "",
        f"位置 x:    {pos.x:.6f}",
        f"位置 y:    {pos.y:.6f}",
        f"位置 z:    {pos.z:.6f}",
        "",
        f"姿态 x:    {ori.x:.6f}",
        f"姿态 y:    {ori.y:.6f}",
        f"姿态 z:    {ori.z:.6f}",
        f"姿态 w:    {ori.w:.6f}",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="记录一次当前里程计坐标快照")
    parser.add_argument(
        "--topic",
        default="/capture/odometry/high_rate",
        help="里程计话题名（默认 %(default)s）",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="等待超时秒数（默认 %(default)s）",
    )
    parser.add_argument(
        "--no-save",
        action="store_true",
        help="只打印，不写文件",
    )
    args = parser.parse_args()

    rclpy.init(args=sys.argv[1:])
    node = OdometrySnapshot(
        topic=args.topic,
        timeout=args.timeout,
        save_to_file=not args.no_save,
    )

    try:
        msg = node.run()
        if msg is None:
            sys.exit(1)

        text = format_snapshot(msg, args.topic)
        print(text)

        if not args.no_save:
            project_root = os.environ.get(
                "CAPTURE_PROJECT_ROOT",
                os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            )
            data_dir = os.path.join(project_root, "data")
            os.makedirs(data_dir, exist_ok=True)

            ts = time.strftime("%Y%m%d_%H%M%S")
            filename = f"odom_snapshot_{ts}.txt"
            filepath = os.path.join(data_dir, filename)

            with open(filepath, "w", encoding="utf-8") as f:
                f.write(text)
                f.write("\n")

            print(f"\n已保存到: {filepath}")

    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
