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
PCV2_MAGIC = b"PCV2"
PCV2_VERSION = 2
PCV2_POINT_STRIDE = 16

_HEADER_STRUCT = struct.Struct("<4sHHIQI")


@dataclass(frozen=True, slots=True)
class CloudPreviewFrame:
    """已经完成一次编码、可供多个浏览器共享的PCV1或PCV2帧。"""

    sequence: int
    sensor_stamp_ns: int
    point_count: int
    frame_id: str
    binary: bytes
    coordinate_mode: str = "local_enu"
    max_points: int = PCV1_MAX_POINTS
    protocol: str = "PCV1"
    version: int = PCV1_VERSION
    point_stride: int = PCV1_POINT_STRIDE
    point_format: str = "xyz_float32_le"
    color_mode: str = "single"

    @property
    def stream_key(self) -> tuple[str, str, int]:
        """标识需要重新发送流描述的协议语义。"""

        return self.protocol, self.frame_id, self.max_points

    def stream_info(self) -> dict[str, object]:
        return {
            "type": "stream_info",
            "protocol": self.protocol,
            "version": self.version,
            "header_bytes": PCV1_HEADER_BYTES,
            "point_format": self.point_format,
            "point_stride": self.point_stride,
            "max_points": self.max_points,
            "frame_id": self.frame_id,
            "coordinate_mode": self.coordinate_mode,
            "sensor_clock": "device_boot",
            "color_mode": self.color_mode,
        }


def encode_cloud_preview(message: Any, sequence: int) -> CloudPreviewFrame:
    """将固定ROS预览布局封装为向后兼容PCV1或带设备分类的PCV2。"""

    sensor_stamp_ns = (
        int(message.header.stamp.sec) * 1_000_000_000
        + int(message.header.stamp.nanosec)
    )
    point_count = int(message.width)
    sequence_u32 = sequence & 0xFFFFFFFF
    fields = {field.name: field for field in getattr(message, "fields", ())}
    classification_field = fields.get("classification")
    is_pcv2 = (
        classification_field is not None
        and int(classification_field.offset) == 12
        and int(classification_field.datatype) == 2
        and int(classification_field.count) == 1
        and int(message.point_step) == PCV2_POINT_STRIDE
    )
    point_step = int(getattr(message, "point_step", PCV1_POINT_STRIDE))
    if is_pcv2:
        for name, offset in (("x", 0), ("y", 4), ("z", 8)):
            field = fields.get(name)
            if (
                field is None
                or int(field.offset) != offset
                or int(field.datatype) != 7
                or int(field.count) != 1
            ):
                raise ValueError("PCV2要求x/y/z为offset 0/4/8的FLOAT32字段")
        if bool(getattr(message, "is_bigendian", False)):
            raise ValueError("PCV2只接受小端ROS点云布局")
        if int(getattr(message, "height", 1)) != 1:
            raise ValueError("PCV2只接受height=1的无组织点云")
        if int(getattr(message, "row_step", point_count * point_step)) != point_count * point_step:
            raise ValueError("PCV2要求row_step等于width乘point_step")
    if not is_pcv2 and point_step != PCV1_POINT_STRIDE:
        raise ValueError("点云预览ROS布局既不是XYZ PCV1也不是XYZ+classification PCV2")
    expected_payload_bytes = point_count * point_step
    if len(message.data) != expected_payload_bytes:
        raise ValueError("点云预览ROS负载长度与point_step不一致")
    version = PCV2_VERSION if is_pcv2 else PCV1_VERSION
    header = _HEADER_STRUCT.pack(
        PCV2_MAGIC if is_pcv2 else PCV1_MAGIC,
        version,
        PCV1_FLAG_SENSOR_STAMP_VALID,
        sequence_u32,
        sensor_stamp_ns,
        point_count,
    )

    # ROS预览节点已经生成连续XYZ或XYZ+分类数据；此处不重复业务分类。
    binary = header + bytes(message.data)
    return CloudPreviewFrame(
        sequence=sequence_u32,
        sensor_stamp_ns=sensor_stamp_ns,
        point_count=point_count,
        frame_id=str(message.header.frame_id),
        binary=binary,
        protocol="PCV2" if is_pcv2 else "PCV1",
        version=version,
        point_stride=PCV2_POINT_STRIDE if is_pcv2 else PCV1_POINT_STRIDE,
        point_format="xyz_float32_class_uint8_le" if is_pcv2 else "xyz_float32_le",
        color_mode="classification" if is_pcv2 else "single",
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
