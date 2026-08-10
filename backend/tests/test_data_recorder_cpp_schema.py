from __future__ import annotations

import re
import sqlite3
from pathlib import Path


def test_data_recorder_cpp_schema_executes_without_duplicate_columns() -> None:
    source = (
        Path(__file__).resolve().parents[2]
        / "ros2_ws"
        / "src"
        / "data_recorder"
        / "src"
        / "data_recorder_node.cpp"
    ).read_text(encoding="utf-8")
    match = re.search(
        r"void create_schema\(\).*?execute\(database_, R\"SQL\((.*?)\)SQL\"\);",
        source,
        re.DOTALL,
    )
    assert match is not None

    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(match.group(1))
        metadata_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(recording_metadata)")
        }
        sample_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(clearance_samples)")
        }
    finally:
        connection.close()

    assert "lidar_mount_height_m" in metadata_columns
    assert "clearance_threshold_m" in metadata_columns
    assert {"travel_direction", "lane_side"}.issubset(metadata_columns)
    assert {
        "source_sequence",
        "source_age_ms",
        "is_repeated",
        "repeat_index",
    }.issubset(sample_columns)
    assert "insert_source_frame(source);" in source
    assert "DELETE FROM clearance_samples WHERE recorded_timestamp_ns > ?" in source



def test_data_recorder_stores_mount_adjusted_clearance_and_keeps_raw_algorithm_value() -> None:
    source = (
        Path(__file__).resolve().parents[2]
        / "ros2_ws"
        / "src"
        / "data_recorder"
        / "src"
        / "data_recorder_node.cpp"
    ).read_text(encoding="utf-8")

    assert "VALUES (1, 5, ?, 'recorded'" in source
    assert "clearance_height = *value + lidar_mount_height_m_" in source
    assert "bind_nullable_double(statement, 5, value);" in source
    assert "bind_nullable_double(statement, 6, clearance_height);" in source
    assert "request->lidar_mount_height_m < 0.0" in source
    assert "request->clearance_threshold_m < 0.0" in source
