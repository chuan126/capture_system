import struct
from types import SimpleNamespace

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
        data=bytes(range(24)),
    )


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


def test_stream_info_declares_slam_world_coordinates() -> None:
    stream_info = encode_cloud_preview(make_message(), 1).stream_info()

    assert stream_info["coordinate_mode"] == "sensor_local"
    assert stream_info["frame_id"] == "device0/odom"
    assert stream_info["point_format"] == "xyz_float32_le"
    assert stream_info["max_points"] == 10_000
