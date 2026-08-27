from __future__ import annotations

import json
import math
import sqlite3
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable


MAX_SUPPORTED_DATABASE_BYTES = 2 * 1024 * 1024 * 1024
LOWEST_FRAME_LIMIT = 20
CANDIDATE_LIMIT = 3
CANDIDATE_CONTEXT_RADIUS = 5


class DeepSeekPayloadError(RuntimeError):
    """任务数据库无法转换为有界的大模型审计数据包。"""


@dataclass(frozen=True, slots=True)
class BoundedAnalysisPayload:
    json_text: str
    byte_count: int
    database_size_bytes: int
    source_frame_count: int
    valid_source_frame_count: int


def build_bounded_analysis_payload(
    database_path: Path,
    task_summary: dict[str, object],
    maximum_bytes: int,
    *,
    progress: Callable[[str, float], None] | None = None,
) -> BoundedAnalysisPayload:
    """只读扫描最多2 GiB数据库，输出大小不随高频明细行数增长的审计包。"""
    database_size = _database_size(database_path)
    if database_size > MAX_SUPPORTED_DATABASE_BYTES:
        raise DeepSeekPayloadError(
            "measurements.db超过当前支持的2 GiB上限："
            f"{database_size}字节"
        )
    connection = _open_readonly(database_path)
    try:
        tables = _table_names(connection)
        frame_query, frame_parameters, frame_source = _frame_query(connection, tables)
        valid_before_height_filter = int(
            connection.execute(f"SELECT COUNT(*) FROM ({frame_query})", frame_parameters).fetchone()[0]
        )
        lower_bound, upper_bound = _task_height_bounds(task_summary)
        frame_query = f"SELECT * FROM ({frame_query}) WHERE height_m BETWEEN ? AND ?"
        frame_parameters = (*frame_parameters, lower_bound, upper_bound)
        if progress:
            progress("扫描真实净空源帧", 0.34)
        source_summary, lowest_frames, candidates = _analyze_source_frames(
            connection,
            frame_query,
            frame_parameters,
            frame_source,
        )
        source_summary["valid_frames_before_height_filter"] = valid_before_height_filter
        source_summary["height_range_excluded_frames"] = max(
            0, valid_before_height_filter - int(source_summary["valid_frames"])
        )
        source_summary["invalid_frames"] = max(
            0, int(source_summary["total_frames"]) - valid_before_height_filter
        )
        if progress:
            progress("提取最低候选连续证据", 0.42)
        candidate_contexts = _candidate_contexts(
            connection,
            frame_query,
            frame_parameters,
            candidates,
        )
        _attach_candidate_snapshots(connection, tables, lowest_frames, candidate_contexts)
        if progress:
            progress("汇总IMU与RTK数据质量", 0.48)
        data_quality = _data_quality(connection, tables, source_summary)
        payload = {
            "format": "capture-clearance-audit-v2",
            "policy": {
                "database_read_only": True,
                "raw_database_uploaded": False,
                "high_rate_rows_uploaded": False,
                "source_frame_deduplication": "clearance_source_frames或is_repeated=0",
                "lowest_frame_limit": LOWEST_FRAME_LIMIT,
                "candidate_limit": CANDIDATE_LIMIT,
                "candidate_context_radius_frames": CANDIDATE_CONTEXT_RADIUS,
                "height_range_filter": "clearance_threshold_m <= height_m <= clearance_upper_limit_m",
            },
            "database": {
                "file_name": database_path.name,
                "size_bytes": database_size,
                "maximum_supported_bytes": MAX_SUPPORTED_DATABASE_BYTES,
            },
            "task": _safe_task_summary(task_summary),
            "source_frame_statistics": source_summary,
            "lowest_20_real_frames": lowest_frames,
            "candidate_contexts": candidate_contexts,
            "data_quality": data_quality,
            "recent_task_events": _recent_task_events(connection, tables),
        }
        json_text = json.dumps(
            payload,
            ensure_ascii=False,
            separators=(",", ":"),
            allow_nan=False,
        )
        byte_count = len(json_text.encode("utf-8"))
        if byte_count > maximum_bytes:
            raise DeepSeekPayloadError(
                "本地有界审计包超过当前DeepSeek请求上限："
                f"{byte_count}字节 > {maximum_bytes}字节"
            )
        return BoundedAnalysisPayload(
            json_text=json_text,
            byte_count=byte_count,
            database_size_bytes=database_size,
            source_frame_count=int(source_summary["total_frames"]),
            valid_source_frame_count=int(source_summary["valid_frames"]),
        )
    except sqlite3.Error as error:
        raise DeepSeekPayloadError(f"读取measurements.db失败：{error}") from error
    finally:
        connection.close()


def _database_size(path: Path) -> int:
    try:
        size = path.stat().st_size
        wal = path.with_name(path.name + "-wal")
        if wal.is_file():
            size += wal.stat().st_size
        return size
    except OSError as error:
        raise DeepSeekPayloadError(f"读取measurements.db大小失败：{error}") from error


def _open_readonly(path: Path) -> sqlite3.Connection:
    try:
        connection = sqlite3.connect(
            f"file:{path.as_posix()}?mode=ro",
            uri=True,
            timeout=30.0,
        )
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA query_only=ON")
        connection.execute("PRAGMA busy_timeout=30000")
        connection.execute("PRAGMA temp_store=FILE")
        return connection
    except sqlite3.Error as error:
        raise DeepSeekPayloadError(f"无法只读打开measurements.db：{error}") from error


def _table_names(connection: sqlite3.Connection) -> set[str]:
    return {
        str(row[0])
        for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"
        )
    }


def _columns(connection: sqlite3.Connection, table: str) -> set[str]:
    return {str(row[1]) for row in connection.execute(f'PRAGMA table_info("{table}")')}


def _column_or_null(columns: set[str], name: str, alias: str | None = None) -> str:
    output = alias or name
    return f'"{name}" AS "{output}"' if name in columns else f'NULL AS "{output}"'


def _frame_query(
    connection: sqlite3.Connection,
    tables: set[str],
) -> tuple[str, tuple[object, ...], str]:
    if "clearance_source_frames" in tables:
        columns = _columns(connection, "clearance_source_frames")
        required = {"source_sequence", "source_timestamp_ns", "valid", "lidar_to_top_m"}
        frame_count = int(
            connection.execute("SELECT COUNT(*) FROM clearance_source_frames").fetchone()[0]
        )
        if required.issubset(columns) and frame_count > 0:
            mount_height = 0.0
            if "recording_metadata" in tables:
                metadata_columns = _columns(connection, "recording_metadata")
                if "lidar_mount_height_m" in metadata_columns:
                    row = connection.execute(
                        "SELECT lidar_mount_height_m FROM recording_metadata WHERE id=1"
                    ).fetchone()
                    if row is not None and _finite(row[0]) is not None:
                        mount_height = float(row[0])
            query = f"""
                SELECT source_sequence AS seq,
                       source_timestamp_ns AS timestamp_ns,
                       lidar_to_top_m + ? AS height_m,
                       {_column_or_null(columns, 'quality_score')},
                       {_column_or_null(columns, 'selected_inlier_count')},
                       {_column_or_null(columns, 'candidate_region_count')},
                       {_column_or_null(columns, 'selected_grid_area_m2')},
                       {_column_or_null(columns, 'selected_residual_p95_m')},
                       {_column_or_null(columns, 'processing_time_ms')}
                FROM clearance_source_frames
                WHERE valid=1 AND source_timestamp_ns IS NOT NULL
                      AND lidar_to_top_m IS NOT NULL AND lidar_to_top_m > 0
            """
            return query, (mount_height,), "clearance_source_frames"
    if "clearance_samples" not in tables:
        raise DeepSeekPayloadError("measurements.db缺少净空源帧表")
    columns = _columns(connection, "clearance_samples")
    required = {"sample_index", "source_timestamp_ns", "valid", "clearance_height_m"}
    if not required.issubset(columns):
        raise DeepSeekPayloadError("clearance_samples缺少净空审计所需字段")
    sequence = (
        "CASE WHEN source_sequence > 0 THEN source_sequence ELSE sample_index END"
        if "source_sequence" in columns else "sample_index"
    )
    repeated_filter = " AND is_repeated=0" if "is_repeated" in columns else ""
    query = f"""
        SELECT {sequence} AS seq,
               source_timestamp_ns AS timestamp_ns,
               clearance_height_m AS height_m,
               {_column_or_null(columns, 'quality_score')},
               NULL AS selected_inlier_count,
               NULL AS candidate_region_count,
               NULL AS selected_grid_area_m2,
               NULL AS selected_residual_p95_m,
               NULL AS processing_time_ms
        FROM clearance_samples
        WHERE valid=1 AND source_timestamp_ns IS NOT NULL
              AND clearance_height_m IS NOT NULL AND clearance_height_m > 0
              {repeated_filter}
    """
    return query, (), "clearance_samples"


def _analyze_source_frames(
    connection: sqlite3.Connection,
    frame_query: str,
    parameters: tuple[object, ...],
    frame_source: str,
) -> tuple[dict[str, object], list[dict[str, object]], list[dict[str, object]]]:
    valid_count = int(connection.execute(
        f"SELECT COUNT(*) FROM ({frame_query})", parameters
    ).fetchone()[0])
    total_count = _source_total_count(connection, frame_source, valid_count)
    if valid_count <= 0:
        # 区间内没有样本本身就是需要进入报告的异常事实。保留任务、RTK和
        # 数据质量信息，最低值相关统计明确置空，不把整份报告误判为不可生成。
        return ({
            "total_frames": total_count,
            "valid_frames": 0,
            "invalid_frames": max(0, total_count),
            "duration_s": 0.0,
            "actual_frequency_hz": None,
            "raw_min_m": None,
            "median_m": None,
            "mad_m": None,
            "p01_m": None,
            "p05_m": None,
            "rolling3_median_min_m": None,
            "rolling5_median_min_m": None,
            "first_timestamp_ns": 0,
            "last_timestamp_ns": 0,
        }, [], [])
    bounds = connection.execute(
        f"SELECT MIN(timestamp_ns), MAX(timestamp_ns) FROM ({frame_query})",
        parameters,
    ).fetchone()
    first_timestamp = int(bounds[0]) if bounds and bounds[0] is not None else 0
    last_timestamp = int(bounds[1]) if bounds and bounds[1] is not None else first_timestamp
    duration_s = max(0.0, (last_timestamp - first_timestamp) / 1_000_000_000.0)
    frequency_hz = (
        (valid_count - 1) / duration_s if valid_count > 1 and duration_s > 0 else None
    )

    percentiles = (0.01, 0.05, 0.5)
    targets = {
        percentile: _percentile_indices(valid_count, percentile)
        for percentile in percentiles
    }
    target_indices = {index for pair in targets.values() for index in pair}
    target_values: dict[int, float] = {}
    lowest: list[dict[str, object]] = []
    max_target = max(target_indices)
    cursor = connection.execute(
        f"SELECT * FROM ({frame_query}) ORDER BY height_m ASC, seq ASC",
        parameters,
    )
    for index, row in enumerate(cursor):
        if index < LOWEST_FRAME_LIMIT:
            lowest.append(_frame_dict(row))
        if index in target_indices:
            target_values[index] = float(row["height_m"])
        if index >= max(max_target, LOWEST_FRAME_LIMIT - 1):
            break
    quantiles = {
        percentile: _interpolate_percentile(valid_count, percentile, target_values)
        for percentile in percentiles
    }
    median = quantiles[0.5]
    mad_targets = _percentile_indices(valid_count, 0.5)
    mad_values: dict[int, float] = {}
    for index, row in enumerate(connection.execute(
        f"SELECT ABS(height_m - ?) AS deviation FROM ({frame_query}) "
        "ORDER BY deviation ASC",
        (median, *parameters),
    )):
        if index in mad_targets:
            mad_values[index] = float(row["deviation"])
        if index >= max(mad_targets):
            break
    mad = _interpolate_percentile(valid_count, 0.5, mad_values)

    rolling3_min: float | None = None
    rolling5_min: float | None = None
    window: deque[dict[str, object]] = deque(maxlen=5)
    candidate_pool: list[dict[str, object]] = []
    for row in connection.execute(
        f"SELECT * FROM ({frame_query}) ORDER BY seq ASC",
        parameters,
    ):
        frame = _frame_dict(row)
        window.append(frame)
        if len(window) >= 3:
            rolling3 = _median([float(item["height_m"]) for item in list(window)[-3:]])
            rolling3_min = rolling3 if rolling3_min is None else min(rolling3_min, rolling3)
        if len(window) == 5:
            rolling5 = _median([float(item["height_m"]) for item in window])
            rolling5_min = rolling5 if rolling5_min is None else min(rolling5_min, rolling5)
            representative = list(window)[2]
            candidate_pool.append({
                "rolling5_median_m": _rounded(rolling5, 4),
                "seq": int(representative["seq"]),
                "timestamp_ns": int(representative["timestamp_ns"]),
                "height_m": representative["height_m"],
            })
            candidate_pool.sort(key=lambda item: (float(item["rolling5_median_m"]), int(item["seq"])))
            del candidate_pool[30:]
    candidates: list[dict[str, object]] = []
    for candidate in candidate_pool:
        if any(
            abs(int(candidate["seq"]) - int(selected["seq"])) <= CANDIDATE_CONTEXT_RADIUS
            for selected in candidates
        ):
            continue
        candidates.append(candidate)
        if len(candidates) >= CANDIDATE_LIMIT:
            break
    if not candidates and lowest:
        candidates = [{
            "rolling5_median_m": lowest[0]["height_m"],
            "seq": lowest[0]["seq"],
            "timestamp_ns": lowest[0]["timestamp_ns"],
            "height_m": lowest[0]["height_m"],
        }]

    summary = {
        "total_frames": total_count,
        "valid_frames": valid_count,
        "invalid_frames": max(0, total_count - valid_count),
        "duration_s": _rounded(duration_s, 3),
        "actual_frequency_hz": _rounded(frequency_hz, 3),
        "raw_min_m": lowest[0]["height_m"] if lowest else None,
        "median_m": _rounded(median, 4),
        "mad_m": _rounded(mad, 4),
        "p01_m": _rounded(quantiles[0.01], 4),
        "p05_m": _rounded(quantiles[0.05], 4),
        "rolling3_median_min_m": _rounded(rolling3_min, 4),
        "rolling5_median_min_m": _rounded(rolling5_min, 4),
        "first_timestamp_ns": first_timestamp,
        "last_timestamp_ns": last_timestamp,
    }
    return summary, lowest, candidates


def _source_total_count(
    connection: sqlite3.Connection,
    frame_source: str,
    valid_count: int,
) -> int:
    if frame_source == "clearance_source_frames":
        return int(connection.execute("SELECT COUNT(*) FROM clearance_source_frames").fetchone()[0])
    if frame_source == "clearance_samples":
        columns = _columns(connection, "clearance_samples")
        if "is_repeated" in columns:
            return int(connection.execute(
                "SELECT COUNT(*) FROM clearance_samples WHERE is_repeated=0"
            ).fetchone()[0])
        return int(connection.execute("SELECT COUNT(*) FROM clearance_samples").fetchone()[0])
    return valid_count


def _candidate_contexts(
    connection: sqlite3.Connection,
    frame_query: str,
    parameters: tuple[object, ...],
    candidates: list[dict[str, object]],
) -> list[dict[str, object]]:
    contexts: list[dict[str, object]] = []
    for candidate in candidates:
        sequence = int(candidate["seq"])
        pre = [
            _frame_dict(row)
            for row in connection.execute(
                f"SELECT * FROM ({frame_query}) WHERE seq < ? "
                "ORDER BY seq DESC LIMIT ?",
                (*parameters, sequence, CANDIDATE_CONTEXT_RADIUS),
            )
        ]
        pre.reverse()
        current_row = connection.execute(
            f"SELECT * FROM ({frame_query}) WHERE seq = ? LIMIT 1",
            (*parameters, sequence),
        ).fetchone()
        post = [
            _frame_dict(row)
            for row in connection.execute(
                f"SELECT * FROM ({frame_query}) WHERE seq > ? "
                "ORDER BY seq ASC LIMIT ?",
                (*parameters, sequence, CANDIDATE_CONTEXT_RADIUS),
            )
        ]
        current = _frame_dict(current_row) if current_row is not None else None
        context_sequences = [int(row["seq"]) for row in pre]
        if current is not None:
            context_sequences.append(int(current["seq"]))
        context_sequences.extend(int(row["seq"]) for row in post)
        contexts.append({
            **candidate,
            "pre": pre,
            "current": current,
            "post": post,
            "continuous_frame_count": len(context_sequences),
            "sequence_continuous": all(
                right == left + 1
                for left, right in zip(context_sequences, context_sequences[1:])
            ),
            "continuous_distance_m": None,
        })
    return contexts


def _attach_candidate_snapshots(
    connection: sqlite3.Connection,
    tables: set[str],
    lowest: list[dict[str, object]],
    contexts: list[dict[str, object]],
) -> None:
    if "clearance_samples" not in tables:
        return
    columns = _columns(connection, "clearance_samples")
    if "source_sequence" not in columns:
        return
    sequences = {
        int(frame["seq"])
        for frame in lowest
    }
    sequences.update(int(context["seq"]) for context in contexts)
    if not sequences:
        return
    fields = [
        "source_sequence",
        "minimum_point_x_m",
        "minimum_point_y_m",
        "minimum_point_z_m",
        "rtk_valid",
        "rtk_satellite_count",
        "rtk_hdop",
        "rtk_pdop",
        "odin_position_x_m",
        "odin_position_y_m",
        "odin_position_z_m",
    ]
    select_fields = [
        _column_or_null(columns, field)
        for field in fields
    ]
    placeholders = ",".join("?" for _ in sequences)
    snapshots: dict[int, dict[str, object]] = {}
    ordering = next(
        (
            field
            for field in ("sample_index", "recorded_timestamp_ns", "source_timestamp_ns")
            if field in columns
        ),
        "source_sequence",
    )
    for row in connection.execute(
        f"SELECT {', '.join(select_fields)} FROM clearance_samples "
        f"WHERE source_sequence IN ({placeholders}) ORDER BY {ordering} ASC",
        tuple(sorted(sequences)),
    ):
        sequence = int(row["source_sequence"])
        if sequence in snapshots:
            continue
        snapshots[sequence] = {
            "minimum_point_xyz_m": [
                _rounded(_finite(row["minimum_point_x_m"]), 4),
                _rounded(_finite(row["minimum_point_y_m"]), 4),
                _rounded(_finite(row["minimum_point_z_m"]), 4),
            ],
            "rtk_valid": bool(row["rtk_valid"]) if row["rtk_valid"] is not None else None,
            "rtk_satellite_count": row["rtk_satellite_count"],
            "rtk_hdop": _rounded(_finite(row["rtk_hdop"]), 3),
            "rtk_pdop": _rounded(_finite(row["rtk_pdop"]), 3),
            "odin_position_xyz_m": [
                _rounded(_finite(row["odin_position_x_m"]), 4),
                _rounded(_finite(row["odin_position_y_m"]), 4),
                _rounded(_finite(row["odin_position_z_m"]), 4),
            ],
        }
    for frame in lowest:
        frame["snapshot"] = snapshots.get(int(frame["seq"]))
    for context in contexts:
        context["snapshot"] = snapshots.get(int(context["seq"]))


def _data_quality(
    connection: sqlite3.Connection,
    tables: set[str],
    source_summary: dict[str, object],
) -> dict[str, object]:
    quality: dict[str, object] = {
        "source_valid_ratio": _ratio(
            int(source_summary.get("valid_frames_before_height_filter", source_summary["valid_frames"])),
            int(source_summary["total_frames"])
        ),
        "source_invalid_reasons": _invalid_source_reasons(connection, tables),
    }
    if "clearance_samples" in tables:
        columns = _columns(connection, "clearance_samples")
        repeated = "SUM(CASE WHEN is_repeated=1 THEN 1 ELSE 0 END)" if "is_repeated" in columns else "0"
        row = connection.execute(
            "SELECT COUNT(*), SUM(CASE WHEN valid=1 THEN 1 ELSE 0 END), "
            f"{repeated} FROM clearance_samples"
        ).fetchone()
        quality["clearance_samples"] = {
            "rows": int(row[0] or 0),
            "valid_rows": int(row[1] or 0),
            "repeated_hold_rows": int(row[2] or 0),
        }
    if "imu_samples" in tables:
        columns = _columns(connection, "imu_samples")
        timestamp = "recorded_timestamp_ns" if "recorded_timestamp_ns" in columns else None
        expressions = ["COUNT(*)"]
        expressions.extend([
            f"MIN({timestamp})" if timestamp else "NULL",
            f"MAX({timestamp})" if timestamp else "NULL",
            "SUM(CASE WHEN clearance_height_m IS NOT NULL THEN 1 ELSE 0 END)"
            if "clearance_height_m" in columns else "0",
            "SUM(CASE WHEN rtk_valid=1 THEN 1 ELSE 0 END)"
            if "rtk_valid" in columns else "0",
            "SUM(CASE WHEN minimum_point_x_m IS NOT NULL AND minimum_point_y_m IS NOT NULL "
            "AND minimum_point_z_m IS NOT NULL AND NOT (minimum_point_x_m=0 AND "
            "minimum_point_y_m=0 AND minimum_point_z_m=0) THEN 1 ELSE 0 END)"
            if {"minimum_point_x_m", "minimum_point_y_m", "minimum_point_z_m"}.issubset(columns)
            else "0",
            "AVG(rtk_satellite_count)" if "rtk_satellite_count" in columns else "NULL",
            "AVG(rtk_hdop)" if "rtk_hdop" in columns else "NULL",
            "AVG(rtk_pdop)" if "rtk_pdop" in columns else "NULL",
        ])
        row = connection.execute(f"SELECT {', '.join(expressions)} FROM imu_samples").fetchone()
        count = int(row[0] or 0)
        duration_s = (
            max(0.0, (int(row[2]) - int(row[1])) / 1_000_000_000.0)
            if row[1] is not None and row[2] is not None else 0.0
        )
        quality["imu_samples"] = {
            "rows": count,
            "duration_s": _rounded(duration_s, 3),
            "actual_frequency_hz": _rounded(
                (count - 1) / duration_s if count > 1 and duration_s > 0 else None,
                3,
            ),
            "clearance_snapshot_ratio": _ratio(int(row[3] or 0), count),
            "rtk_valid_ratio": _ratio(int(row[4] or 0), count),
            "minimum_point_evidence_ratio": _ratio(int(row[5] or 0), count),
            "average_satellite_count": _rounded(_finite(row[6]), 2),
            "average_hdop": _rounded(_finite(row[7]), 3),
            "average_pdop": _rounded(_finite(row[8]), 3),
        }
    if "rtk_samples" in tables:
        row = connection.execute(
            "SELECT COUNT(*), SUM(CASE WHEN valid=1 THEN 1 ELSE 0 END) FROM rtk_samples"
        ).fetchone()
        quality["rtk_samples"] = {
            "rows": int(row[0] or 0),
            "valid_ratio": _ratio(int(row[1] or 0), int(row[0] or 0)),
        }
    return quality


def _invalid_source_reasons(
    connection: sqlite3.Connection,
    tables: set[str],
) -> list[dict[str, object]]:
    if "clearance_source_frames" not in tables:
        return []
    columns = _columns(connection, "clearance_source_frames")
    if "invalid_reason" not in columns:
        return []
    return [
        {"reason": str(row[0] or "UNKNOWN"), "count": int(row[1])}
        for row in connection.execute(
            "SELECT invalid_reason, COUNT(*) AS count FROM clearance_source_frames "
            "WHERE valid=0 GROUP BY invalid_reason ORDER BY count DESC LIMIT 10"
        )
    ]


def _recent_task_events(
    connection: sqlite3.Connection,
    tables: set[str],
) -> list[dict[str, object]]:
    if "task_events" not in tables:
        return []
    columns = _columns(connection, "task_events")
    if "id" not in columns:
        return []
    selected = [
        _column_or_null(columns, "id"),
        _column_or_null(columns, "event_type"),
        _column_or_null(columns, "occurred_at_ns"),
        _column_or_null(columns, "message"),
        _column_or_null(columns, "error_code"),
    ]
    rows = connection.execute(
        f"SELECT {', '.join(selected)} FROM task_events ORDER BY id DESC LIMIT 20"
    ).fetchall()
    return [
        {
            "event_type": str(row["event_type"] or ""),
            "occurred_at_ns": row["occurred_at_ns"],
            "message": " ".join(str(row["message"] or "").split())[:200],
            "error_code": str(row["error_code"] or "")[:100],
        }
        for row in reversed(rows)
    ]


def _safe_task_summary(summary: dict[str, object]) -> dict[str, object]:
    excluded = {"skill_prompt", "database_path", "task_range"}
    return {
        key: _json_safe(value)
        for key, value in summary.items()
        if key not in excluded
    }


def _task_height_bounds(summary: dict[str, object]) -> tuple[float, float]:
    try:
        lower = float(summary["clearance_threshold_m"])
        upper = float(summary["clearance_upper_limit_m"])
    except (KeyError, TypeError, ValueError) as error:
        raise DeepSeekPayloadError("任务缺少有效的高度上下限阈值") from error
    if not math.isfinite(lower) or not math.isfinite(upper) or lower < 0.0 or upper > 20.0 or lower > upper:
        raise DeepSeekPayloadError("任务高度上下限阈值无效")
    return lower, upper


def _frame_dict(row: sqlite3.Row) -> dict[str, object]:
    return {
        "seq": int(row["seq"]),
        "timestamp_ns": int(row["timestamp_ns"]),
        "height_m": _rounded(_finite(row["height_m"]), 4),
        "quality_score": _rounded(_finite(row["quality_score"]), 4),
        "selected_inlier_count": row["selected_inlier_count"],
        "candidate_region_count": row["candidate_region_count"],
        "selected_grid_area_m2": _rounded(_finite(row["selected_grid_area_m2"]), 4),
        "selected_residual_p95_m": _rounded(_finite(row["selected_residual_p95_m"]), 4),
        "processing_time_ms": _rounded(_finite(row["processing_time_ms"]), 3),
    }


def _percentile_indices(count: int, percentile: float) -> tuple[int, int]:
    position = (count - 1) * percentile
    return math.floor(position), math.ceil(position)


def _interpolate_percentile(
    count: int,
    percentile: float,
    values: dict[int, float],
) -> float:
    position = (count - 1) * percentile
    lower = math.floor(position)
    upper = math.ceil(position)
    lower_value = values[lower]
    upper_value = values[upper]
    return lower_value + (upper_value - lower_value) * (position - lower)


def _median(values: Iterable[float]) -> float:
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def _finite(value: object) -> float | None:
    if value is None:
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def _rounded(value: float | None, digits: int) -> float | None:
    return round(value, digits) if value is not None and math.isfinite(value) else None


def _ratio(numerator: int, denominator: int) -> float | None:
    return round(numerator / denominator, 4) if denominator > 0 else None


def _json_safe(value: object) -> object:
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if value is None or isinstance(value, (str, int, bool)):
        return value
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    return str(value)
