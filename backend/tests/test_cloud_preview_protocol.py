import struct
from types import SimpleNamespace

import pytest

from backend.protocols.cloud_preview_v1 import (
    PCV1_FLAG_SENSOR_STAMP_VALID,
    PCV1_HEADER_BYTES,
    encode_cloud_preview,
)


def make_message() -> SimpleNamespace:
    return SimpleNamespace(
        header=SimpleNamespace(
            stamp=SimpleNamespace(sec=12, nanosec=345),
            frame_id="device0/odom",
        ),
        width=2,
        point_step=12,
        fields=[],
        data=bytes(range(24)),
    )


def make_classified_message() -> SimpleNamespace:
    message = make_message()
    message.point_step = 16
    message.fields = [
        SimpleNamespace(name="x", offset=0, datatype=7, count=1),
        SimpleNamespace(name="y", offset=4, datatype=7, count=1),
        SimpleNamespace(name="z", offset=8, datatype=7, count=1),
        SimpleNamespace(name="classification", offset=12, datatype=2, count=1),
    ]
    message.height = 1
    message.row_step = 32
    message.is_bigendian = False
    message.data = bytes(range(32))
    return message


def test_encodes_fixed_header_and_unchanged_xyz_payload() -> None:
    message = make_message()
    frame = encode_cloud_preview(message, 0x1_0000_0002)

    magic, version, flags, sequence, stamp_ns, point_count = struct.unpack(
        "<4sHHIQI",
        frame.binary[:PCV1_HEADER_BYTES],
    )

    assert magic == b"PCV1"
    assert version == 1
    assert flags == PCV1_FLAG_SENSOR_STAMP_VALID
    assert sequence == 2
    assert stamp_ns == 12_000_000_345
    assert point_count == 2
    assert frame.frame_id == "device0/odom"
    assert frame.binary[PCV1_HEADER_BYTES:] == message.data


def test_stream_info_declares_raw_sensor_coordinates() -> None:
    stream_info = encode_cloud_preview(make_message(), 1).stream_info()

    assert stream_info["coordinate_mode"] == "sensor"
    assert stream_info["frame_id"] == "device0/odom"
    assert stream_info["point_format"] == "xyz_float32_le"
    assert stream_info["max_points"] == 10_000


def test_encodes_classified_layout_as_pcv2_without_rewriting_payload() -> None:
    message = make_classified_message()
    frame = encode_cloud_preview(message, 9)
    magic, version, flags, sequence, stamp_ns, point_count = struct.unpack(
        "<4sHHIQI", frame.binary[:PCV1_HEADER_BYTES]
    )

    assert magic == b"PCV2"
    assert version == 2
    assert flags == PCV1_FLAG_SENSOR_STAMP_VALID
    assert sequence == 9
    assert stamp_ns == 12_000_000_345
    assert point_count == 2
    assert frame.binary[PCV1_HEADER_BYTES:] == message.data
    assert frame.stream_info()["point_format"] == "xyz_float32_class_uint8_le"
    assert frame.stream_info()["point_stride"] == 16
    assert frame.stream_info()["color_mode"] == "classification"


def test_rejects_malformed_or_truncated_classification_layout() -> None:
    wrong_offset = make_classified_message()
    wrong_offset.fields[-1].offset = 13
    with pytest.raises(ValueError, match="既不是XYZ PCV1也不是"):
        encode_cloud_preview(wrong_offset, 1)

    truncated = make_classified_message()
    truncated.data = truncated.data[:-1]
    with pytest.raises(ValueError, match="负载长度"):
        encode_cloud_preview(truncated, 1)


@pytest.mark.parametrize(
    ("mutate", "reason"),
    [
        (lambda message: setattr(message.fields[0], "offset", 4), "x/y/z"),
        (lambda message: setattr(message.fields[1], "datatype", 2), "x/y/z"),
        (lambda message: setattr(message, "is_bigendian", True), "小端"),
        (lambda message: setattr(message, "height", 2), "height=1"),
        (lambda message: setattr(message, "row_step", 31), "row_step"),
    ],
)
def test_rejects_unsafe_pcv2_ros_layout(mutate, reason: str) -> None:
    message = make_classified_message()
    mutate(message)

    with pytest.raises(ValueError, match=reason):
        encode_cloud_preview(message, 1)
