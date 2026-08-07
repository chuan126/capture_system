from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Any

PCV1_MAGIC = b"PCV1"
PCV1_VERSION = 1
PCV1_HEADER_BYTES = 24
PCV1_POINT_STRIDE = 12
PCV1_MAX_POINTS = 10_000
PCV1_FLAG_SENSOR_STAMP_VALID = 0x0002

_HEADER_STRUCT = struct.Struct("<4sHHIQI")


@dataclass(frozen=True, slots=True)
class CloudPreviewFrame:
    """已经完成一次编码、可供多个浏览器共享的PCV1帧。"""

    sequence: int
    sensor_stamp_ns: int
    point_count: int
    frame_id: str
    binary: bytes
    coordinate_mode: str = "local_enu"
    max_points: int = PCV1_MAX_POINTS

    @property
    def stream_key(self) -> tuple[str, int]:
        """标识需要重新发送流描述的协议语义。"""

        return self.frame_id, self.max_points

    def stream_info(self) -> dict[str, object]:
        return {
            "type": "stream_info",
            "protocol": "PCV1",
            "version": PCV1_VERSION,
            "header_bytes": PCV1_HEADER_BYTES,
            "point_format": "xyz_float32_le",
            "point_stride": PCV1_POINT_STRIDE,
            "max_points": self.max_points,
            "frame_id": self.frame_id,
            "coordinate_mode": self.coordinate_mode,
            "sensor_clock": "device_boot",
            "color_mode": "single",
        }


def encode_cloud_preview(message: Any, sequence: int) -> CloudPreviewFrame:
    """按首版固定ROS输出契约封装PCV1，不解析或检查XYZ负载。"""

    sensor_stamp_ns = (
        int(message.header.stamp.sec) * 1_000_000_000
        + int(message.header.stamp.nanosec)
    )
    point_count = int(message.width)
    sequence_u32 = sequence & 0xFFFFFFFF
    header = _HEADER_STRUCT.pack(
        PCV1_MAGIC,
        PCV1_VERSION,
        PCV1_FLAG_SENSOR_STAMP_VALID,
        sequence_u32,
        sensor_stamp_ns,
        point_count,
    )

    # ROS预览节点已经生成连续XYZ数据；此处只固化为不可变字节并添加协议头。
    binary = header + bytes(message.data)
    return CloudPreviewFrame(
        sequence=sequence_u32,
        sensor_stamp_ns=sensor_stamp_ns,
        point_count=point_count,
        frame_id=str(message.header.frame_id),
        binary=binary,
    )


def status_message(
    state: str,
    reason: str,
    detail: str,
) -> dict[str, str]:
    return {
        "type": "status",
        "state": state,
        "reason": reason,
        "detail": detail,
    }
