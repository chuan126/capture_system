import struct
from types import SimpleNamespace

from backend.devtools.raw_cloud_bridge import encode_raw_cloud_preview
from backend.protocols.cloud_preview_v1 import PCV1_HEADER_BYTES


def field(name: str, offset: int):
    return SimpleNamespace(name=name, offset=offset, datatype=7, count=1)


def make_cloud() -> SimpleNamespace:
    point_step = 20
    points = [
        (1.0, 2.0, 3.0),
        (4.0, 5.0, 6.0),
        (7.0, 8.0, 9.0),
    ]
    data = bytearray(point_step * len(points))
    for index, (x, y, z) in enumerate(points):
        struct.pack_into("<fff", data, index * point_step, x, y, z)
    return SimpleNamespace(
        header=SimpleNamespace(stamp=SimpleNamespace(sec=2, nanosec=5), frame_id="odin_sensor"),
        fields=[field("x", 0), field("y", 4), field("z", 8), field("intensity", 12)],
        is_bigendian=False,
        point_step=point_step,
        row_step=point_step * len(points),
        width=len(points),
        height=1,
        data=bytes(data),
    )


def test_raw_cloud_preview_extracts_xyz_without_using_browser_preview_topic() -> None:
    frame = encode_raw_cloud_preview(make_cloud(), sequence=9, max_points=10)
    assert frame.sequence == 9
    assert frame.sensor_stamp_ns == 2_000_000_005
    assert frame.frame_id == "odin_sensor"
    assert frame.coordinate_mode == "sensor"
    assert frame.point_count == 3
    assert len(frame.binary) == PCV1_HEADER_BYTES + 3 * 12
    assert struct.unpack_from("<fff", frame.binary, PCV1_HEADER_BYTES) == (1.0, 2.0, 3.0)


def test_raw_cloud_preview_uniformly_limits_points() -> None:
    frame = encode_raw_cloud_preview(make_cloud(), sequence=0, max_points=2)
    assert frame.point_count == 2
