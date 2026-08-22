from __future__ import annotations

import json
import math
import os
import sqlite3
import tempfile
from dataclasses import dataclass
from dataclasses import asdict
from pathlib import Path
from threading import Lock
from typing import Iterator, cast

from backend.measurements.clearance_anomaly import (
    CLEARANCE_ANALYSIS_VERSION,
    DEFAULT_CLEARANCE_ANOMALY_CONFIG,
    ClearanceAnalysisResult,
    ClearanceAnomalyConfig,
    ClearanceMeasurement,
    analyze_clearance,
)
from backend.measurements.models import MeasurementDataOrigin, MeasurementLane, MeasurementTravelDirection
from backend.measurements.query_coordinator import (
    MeasurementQueryCancelledError,
    MeasurementQueryHandle,
)
from backend.tasks.repository import TaskRecord


class MeasurementStorageError(RuntimeError):
    """任务测量数据库缺失、损坏或结构不受支持。"""


class MeasurementNotFoundError(LookupError):
    """任务尚无可读取的测量记录。"""


@dataclass(frozen=True)
class ClearanceHistorySampleRecord:
    sample_index: int
    timestamp_ms: int
    elapsed_ms: float
    height_m: float | None
    lidar_to_top_m: float | None
    valid: bool
    invalid_reason: str | None
    quality_score: float | None


@dataclass(frozen=True)
class MeasurementExportSampleRecord:
    sample_index: int
    source_timestamp_ms: int
    recorded_timestamp_ms: int
    elapsed_ms: float
    height_m: float | None
    lidar_to_top_m: float | None
    valid: bool
    invalid_reason: str | None
    quality_score: float | None
    source_sequence: int | None
    source_age_ms: float | None
    is_repeated: bool | None
    repeat_index: int | None
    rtk_timestamp_ms: int | None
    rtk_latitude_deg: float | None
    rtk_longitude_deg: float | None
    rtk_altitude_m: float | None
    rtk_fix_type: str | None
    rtk_valid: bool | None
    rtk_satellite_count: int | None
    rtk_hdop: float | None
    rtk_pdop: float | None
    rtk_speed_knots: float | None
    rtk_track_degrees: float | None
    gyro_x_rad_s: float | None
    gyro_y_rad_s: float | None
    gyro_z_rad_s: float | None
    accel_x_m_s2: float | None
    accel_y_m_s2: float | None
    accel_z_m_s2: float | None
    imu_sample_count: int | None
    radar_temperature_c: float | None
    minimum_point_x_m: float | None
    minimum_point_y_m: float | None
    minimum_point_z_m: float | None
    vehicle_pitch_deg: float | None
    vehicle_roll_deg: float | None
    vehicle_heading_deg: float | None
    odin_position_x_m: float | None
    odin_position_y_m: float | None
    odin_position_z_m: float | None
    odin_qx: float | None
    odin_qy: float | None
    odin_qz: float | None
    odin_qw: float | None


@dataclass(frozen=True)
class RtkEndpointRecord:
    timestamp_ms: int
    latitude_deg: float
    longitude_deg: float
    altitude_m: float | None
    fix_type: str
    valid: bool


@dataclass(frozen=True)
class PauseIntervalRecord:
    started_elapsed_ms: float
    ended_elapsed_ms: float


@dataclass(frozen=True)
class MeasurementStatisticsRecord:
    total_samples: int
    valid_samples: int
    invalid_samples: int
    minimum_height_m: float | None
    average_height_m: float | None
    maximum_height_m: float | None
    duration_ms: float
    nominal_sample_rate_hz: float
    actual_average_sample_rate_hz: float | None


@dataclass(frozen=True)
class NormalHeightStatisticsRecord:
    clearance_threshold_m: float
    clearance_upper_limit_m: float
    normal_samples: int
    below_threshold_samples: int
    above_upper_limit_samples: int
    minimum_height_m: float | None


@dataclass(frozen=True)
class MeasurementSummaryRecord:
    task_id: str
    recording_schema_version: int
    data_origin: MeasurementDataOrigin
    lane: MeasurementLane
    travel_direction: MeasurementTravelDirection
    lane_side: MeasurementLane
    started_at: str
    ended_at: str | None
    complete: bool
    algorithm_version: str | None
    config_version: str | None
    software_version: str | None
    statistics: MeasurementStatisticsRecord
    entry_rtk: RtkEndpointRecord | None
    exit_rtk: RtkEndpointRecord | None
    pause_interval_count: int
    first_sample_index: int
    last_sample_index: int
    first_timestamp_ms: int
    last_timestamp_ms: int


@dataclass(frozen=True)
class ClearanceSeriesSampleRecord:
    sample_index: int
    timestamp_ms: int
    elapsed_ms: float
    height_m: float | None
    valid: bool
    invalid_reason: str | None


@dataclass(frozen=True)
class MeasurementSeriesRecord:
    task_id: str
    domain_start_timestamp_ms: int
    domain_end_timestamp_ms: int
    requested_start_timestamp_ms: int
    requested_end_timestamp_ms: int
    source_sample_count: int
    downsampled: bool
    samples: list[ClearanceSeriesSampleRecord]


@dataclass(frozen=True)
class MeasurementHistoryRecord:
    task_id: str
    recording_schema_version: int
    data_origin: MeasurementDataOrigin
    lane: MeasurementLane
    travel_direction: MeasurementTravelDirection
    lane_side: MeasurementLane
    started_at: str
    ended_at: str | None
    complete: bool
    algorithm_version: str | None
    config_version: str | None
    software_version: str | None
    statistics: MeasurementStatisticsRecord
    entry_rtk: RtkEndpointRecord | None
    exit_rtk: RtkEndpointRecord | None
    pause_intervals: list[PauseIntervalRecord]
    samples: list[ClearanceHistorySampleRecord]


_SUPPORTED_SCHEMA_VERSIONS = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}
_MAX_HISTORY_SAMPLES = 500_000


class MeasurementRepository:
    def __init__(self, tasks_directory: Path) -> None:
        self.tasks_directory = tasks_directory.resolve()
        self._analysis_locks_guard = Lock()
        self._analysis_locks: dict[str, Lock] = {}

    def load_summary(
        self,
        task: TaskRecord,
        *,
        query: MeasurementQueryHandle | None = None,
    ) -> MeasurementSummaryRecord:
        database_path = self._resolve_recording_database(task)
        connection = self._open_readonly(database_path)
        try:
            if query is not None:
                query.bind(connection)
            metadata = self._load_metadata(connection, task)
            statistics = self._load_statistics(connection, task.task_id)
            sample_bounds = self._load_sample_bounds(connection, task.task_id)
            endpoints = self._load_endpoints(connection)
            pause_count = int(
                connection.execute("SELECT COUNT(*) FROM pause_intervals").fetchone()[0]
            )
            return self._summary_from_parts(
                task=task,
                metadata=metadata,
                statistics=statistics,
                endpoints=endpoints,
                pause_interval_count=pause_count,
                sample_bounds=sample_bounds,
            )
        except (MeasurementNotFoundError, MeasurementStorageError, MeasurementQueryCancelledError):
            raise
        except sqlite3.Error as error:
            if query is not None and query.cancelled:
                raise MeasurementQueryCancelledError("回放摘要查询已被后续请求取消") from error
            raise MeasurementStorageError(f"读取任务测量数据库失败：{error}") from error
        finally:
            if query is not None:
                query.unbind(connection)
            connection.close()

    def load_normal_height_statistics(self, task: TaskRecord) -> NormalHeightStatisticsRecord:
        """按任务开始时冻结的业务高度区间计算报告统计，不改写算法有效性。"""
        database_path = self._resolve_recording_database(task)
        connection = self._open_readonly(database_path)
        try:
            metadata = self._load_metadata(connection, task)
            metadata_keys = set(metadata.keys())

            def resolve_bound(metadata_name: str, actual: float | None, planned: float | None, fallback: float) -> float:
                if metadata_name in metadata_keys and metadata[metadata_name] is not None:
                    return float(metadata[metadata_name])
                if actual is not None:
                    return float(actual)
                if planned is not None:
                    return float(planned)
                return fallback

            threshold = resolve_bound(
                "clearance_threshold_m",
                task.clearance_threshold_m,
                task.planned_clearance_threshold_m,
                0.0,
            )
            upper_limit = resolve_bound(
                "clearance_upper_limit_m",
                task.clearance_upper_limit_m,
                task.planned_clearance_upper_limit_m,
                20.0,
            )
            if (
                not math.isfinite(threshold)
                or not math.isfinite(upper_limit)
                or threshold < 0.0
                or upper_limit > 20.0
                or threshold > upper_limit
            ):
                raise MeasurementStorageError("任务测量数据库中的正常高度区间无效")

            aggregate = connection.execute(
                """
                SELECT
                    SUM(CASE WHEN valid = 1 AND clearance_height_m IS NOT NULL
                                  AND clearance_height_m BETWEEN ? AND ? THEN 1 ELSE 0 END) AS normal_samples,
                    SUM(CASE WHEN valid = 1 AND clearance_height_m IS NOT NULL
                                  AND clearance_height_m < ? THEN 1 ELSE 0 END) AS below_threshold_samples,
                    SUM(CASE WHEN valid = 1 AND clearance_height_m IS NOT NULL
                                  AND clearance_height_m > ? THEN 1 ELSE 0 END) AS above_upper_limit_samples,
                    MIN(CASE WHEN valid = 1 AND clearance_height_m BETWEEN ? AND ?
                             THEN clearance_height_m END) AS minimum_height_m
                FROM clearance_samples
                """,
                (threshold, upper_limit, threshold, upper_limit, threshold, upper_limit),
            ).fetchone()
            return NormalHeightStatisticsRecord(
                clearance_threshold_m=threshold,
                clearance_upper_limit_m=upper_limit,
                normal_samples=int(aggregate["normal_samples"] or 0),
                below_threshold_samples=int(aggregate["below_threshold_samples"] or 0),
                above_upper_limit_samples=int(aggregate["above_upper_limit_samples"] or 0),
                minimum_height_m=_optional_float(aggregate["minimum_height_m"]),
            )
        except (MeasurementStorageError, MeasurementNotFoundError):
            raise
        except sqlite3.Error as error:
            raise MeasurementStorageError(f"读取报告正常高度统计失败：{error}") from error
        finally:
            connection.close()

    def load_history(self, task: TaskRecord) -> MeasurementHistoryRecord:
        database_path = self._resolve_recording_database(task)
        connection = self._open_readonly(database_path)
        try:
            metadata = self._load_metadata(connection, task)
            statistics = self._load_statistics(connection, task.task_id)
            if statistics.total_samples > _MAX_HISTORY_SAMPLES:
                raise MeasurementStorageError(
                    f"任务测量样本数 {statistics.total_samples} 超过当前接口上限 {_MAX_HISTORY_SAMPLES}"
                )
            sample_rows = connection.execute(
                """
                SELECT
                    sample_index,
                    source_timestamp_ns,
                    elapsed_ms,
                    clearance_height_m,
                    lidar_to_top_m,
                    valid,
                    invalid_reason,
                    quality_score
                FROM clearance_samples
                ORDER BY sample_index ASC
                """
            ).fetchall()
            samples = [
                ClearanceHistorySampleRecord(
                    sample_index=int(row["sample_index"]),
                    timestamp_ms=int(row["source_timestamp_ns"]) // 1_000_000,
                    elapsed_ms=float(row["elapsed_ms"]),
                    height_m=_optional_float(row["clearance_height_m"]),
                    lidar_to_top_m=_optional_float(row["lidar_to_top_m"]),
                    valid=bool(row["valid"]),
                    invalid_reason=row["invalid_reason"],
                    quality_score=_optional_float(row["quality_score"]),
                )
                for row in sample_rows
            ]
            endpoints = self._load_endpoints(connection)
            pause_intervals = [
                PauseIntervalRecord(
                    started_elapsed_ms=float(row["started_elapsed_ms"]),
                    ended_elapsed_ms=float(row["ended_elapsed_ms"]),
                )
                for row in connection.execute(
                    """
                    SELECT started_elapsed_ms, ended_elapsed_ms
                    FROM pause_intervals
                    ORDER BY started_elapsed_ms
                    """
                ).fetchall()
            ]
            summary = self._summary_from_parts(
                task=task,
                metadata=metadata,
                statistics=statistics,
                endpoints=endpoints,
                pause_interval_count=len(pause_intervals),
                sample_bounds=self._load_sample_bounds(connection, task.task_id),
            )
            return MeasurementHistoryRecord(
                task_id=summary.task_id,
                recording_schema_version=summary.recording_schema_version,
                data_origin=summary.data_origin,
                lane=summary.lane,
                travel_direction=summary.travel_direction,
                lane_side=summary.lane_side,
                started_at=summary.started_at,
                ended_at=summary.ended_at,
                complete=summary.complete,
                algorithm_version=summary.algorithm_version,
                config_version=summary.config_version,
                software_version=summary.software_version,
                statistics=summary.statistics,
                entry_rtk=summary.entry_rtk,
                exit_rtk=summary.exit_rtk,
                pause_intervals=pause_intervals,
                samples=samples,
            )
        except (MeasurementNotFoundError, MeasurementStorageError):
            raise
        except sqlite3.Error as error:
            raise MeasurementStorageError(f"读取任务测量数据库失败：{error}") from error
        finally:
            connection.close()

    def load_series(
        self,
        task: TaskRecord,
        *,
        start_timestamp_ms: int | None,
        end_timestamp_ms: int | None,
        max_points: int,
        query: MeasurementQueryHandle | None = None,
    ) -> MeasurementSeriesRecord:
        if max_points < 5:
            raise ValueError("max_points必须至少为5")

        database_path = self._resolve_recording_database(task)
        connection = self._open_readonly(database_path)
        try:
            if query is not None:
                query.bind(connection)
            self._load_metadata(connection, task)
            _, _, first_timestamp_ms, last_timestamp_ms = self._load_sample_bounds(
                connection, task.task_id
            )

            requested_start = (
                first_timestamp_ms if start_timestamp_ms is None else start_timestamp_ms
            )
            requested_end = last_timestamp_ms if end_timestamp_ms is None else end_timestamp_ms
            if requested_start > requested_end:
                raise ValueError("start_timestamp_ms不得大于end_timestamp_ms")

            effective_start = max(first_timestamp_ms, requested_start)
            effective_end = min(last_timestamp_ms, requested_end)
            if effective_start > effective_end:
                return MeasurementSeriesRecord(
                    task_id=task.task_id,
                    domain_start_timestamp_ms=first_timestamp_ms,
                    domain_end_timestamp_ms=last_timestamp_ms,
                    requested_start_timestamp_ms=requested_start,
                    requested_end_timestamp_ms=requested_end,
                    source_sample_count=0,
                    downsampled=False,
                    samples=[],
                )

            # API 时间窗口使用毫秒，数据库保留纳秒。结束边界覆盖该毫秒内全部样本。
            start_timestamp_ns = effective_start * 1_000_000
            end_timestamp_ns = effective_end * 1_000_000 + 999_999
            count_row = connection.execute(
                """
                SELECT COUNT(*) AS sample_count
                FROM clearance_samples
                WHERE source_timestamp_ns BETWEEN ? AND ?
                """,
                (start_timestamp_ns, end_timestamp_ns),
            ).fetchone()
            source_sample_count = int(count_row["sample_count"])
            if query is not None:
                query.raise_if_cancelled()

            if source_sample_count <= max_points:
                rows = connection.execute(
                    """
                    SELECT
                        sample_index,
                        source_timestamp_ns,
                        elapsed_ms,
                        clearance_height_m,
                        valid,
                        invalid_reason
                    FROM clearance_samples
                    WHERE source_timestamp_ns BETWEEN ? AND ?
                    ORDER BY source_timestamp_ns ASC, sample_index ASC
                    """,
                    (start_timestamp_ns, end_timestamp_ns),
                ).fetchall()
                samples = [self._series_sample_from_row(row) for row in rows]
                return MeasurementSeriesRecord(
                    task_id=task.task_id,
                    domain_start_timestamp_ms=first_timestamp_ms,
                    domain_end_timestamp_ms=last_timestamp_ms,
                    requested_start_timestamp_ms=requested_start,
                    requested_end_timestamp_ms=requested_end,
                    source_sample_count=source_sample_count,
                    downsampled=False,
                    samples=samples,
                )

            samples = self._load_downsampled_series(
                connection,
                start_timestamp_ns=start_timestamp_ns,
                end_timestamp_ns=end_timestamp_ns,
                max_points=max_points,
            )
            if query is not None:
                query.raise_if_cancelled()
            return MeasurementSeriesRecord(
                task_id=task.task_id,
                domain_start_timestamp_ms=first_timestamp_ms,
                domain_end_timestamp_ms=last_timestamp_ms,
                requested_start_timestamp_ms=requested_start,
                requested_end_timestamp_ms=requested_end,
                source_sample_count=source_sample_count,
                downsampled=True,
                samples=samples,
            )
        except (
            MeasurementNotFoundError,
            MeasurementStorageError,
            MeasurementQueryCancelledError,
            ValueError,
        ):
            raise
        except sqlite3.Error as error:
            if query is not None and query.cancelled:
                raise MeasurementQueryCancelledError("回放曲线查询已被后续请求取消") from error
            raise MeasurementStorageError(f"读取任务回放曲线失败：{error}") from error
        finally:
            if query is not None:
                query.unbind(connection)
            connection.close()

    def load_prefix(
        self,
        task: TaskRecord,
        *,
        max_samples: int,
        query: MeasurementQueryHandle | None = None,
    ) -> MeasurementSeriesRecord:
        """读取任务开头固定数量样本，避免首屏扫描整条长任务曲线。"""
        if max_samples < 200:
            raise ValueError("max_samples必须至少为200")

        database_path = self._resolve_recording_database(task)
        connection = self._open_readonly(database_path)
        try:
            if query is not None:
                query.bind(connection)
            self._load_metadata(connection, task)
            _, _, first_timestamp_ms, last_timestamp_ms = self._load_sample_bounds(
                connection, task.task_id
            )
            rows = connection.execute(
                """
                SELECT
                    sample_index,
                    source_timestamp_ns,
                    elapsed_ms,
                    clearance_height_m,
                    valid,
                    invalid_reason
                FROM clearance_samples
                ORDER BY sample_index ASC
                LIMIT ?
                """,
                (max_samples,),
            ).fetchall()
            if not rows:
                raise MeasurementNotFoundError(task.task_id)
            samples = [self._series_sample_from_row(row) for row in rows]
            return MeasurementSeriesRecord(
                task_id=task.task_id,
                domain_start_timestamp_ms=first_timestamp_ms,
                domain_end_timestamp_ms=last_timestamp_ms,
                requested_start_timestamp_ms=samples[0].timestamp_ms,
                requested_end_timestamp_ms=samples[-1].timestamp_ms,
                source_sample_count=len(samples),
                downsampled=False,
                samples=samples,
            )
        except (
            MeasurementNotFoundError,
            MeasurementStorageError,
            MeasurementQueryCancelledError,
            ValueError,
        ):
            raise
        except sqlite3.Error as error:
            if query is not None and query.cancelled:
                raise MeasurementQueryCancelledError("回放首段查询已被后续请求取消") from error
            raise MeasurementStorageError(f"读取任务回放首段曲线失败：{error}") from error
        finally:
            if query is not None:
                query.unbind(connection)
            connection.close()

    def iter_export_samples(self, task: TaskRecord) -> Iterator[MeasurementExportSampleRecord]:
        database_path = self._resolve_recording_database(task)
        connection = self._open_readonly(database_path)
        try:
            self._load_metadata(connection, task)
            sample_columns = {
                str(row[1])
                for row in connection.execute("PRAGMA table_info(clearance_samples)").fetchall()
            }
            source_sequence_expr = "source_sequence" if "source_sequence" in sample_columns else "NULL"
            source_age_expr = "source_age_ms" if "source_age_ms" in sample_columns else "NULL"
            repeated_expr = "is_repeated" if "is_repeated" in sample_columns else "NULL"
            repeat_index_expr = "repeat_index" if "repeat_index" in sample_columns else "NULL"
            def optional_column(name: str) -> str:
                return name if name in sample_columns else "NULL"

            cursor = connection.execute(
                f"""
                SELECT
                    sample_index,
                    source_timestamp_ns,
                    recorded_timestamp_ns,
                    elapsed_ms,
                    clearance_height_m,
                    lidar_to_top_m,
                    valid,
                    invalid_reason,
                    quality_score,
                    {source_sequence_expr} AS source_sequence,
                    {source_age_expr} AS source_age_ms,
                    {repeated_expr} AS is_repeated,
                    {repeat_index_expr} AS repeat_index,
                    {optional_column("rtk_timestamp_ns")} AS rtk_timestamp_ns,
                    {optional_column("rtk_latitude_deg")} AS rtk_latitude_deg,
                    {optional_column("rtk_longitude_deg")} AS rtk_longitude_deg,
                    {optional_column("rtk_altitude_m")} AS rtk_altitude_m,
                    {optional_column("rtk_fix_type")} AS rtk_fix_type,
                    {optional_column("rtk_valid")} AS rtk_valid,
                    {optional_column("rtk_satellite_count")} AS rtk_satellite_count,
                    {optional_column("rtk_hdop")} AS rtk_hdop,
                    {optional_column("rtk_pdop")} AS rtk_pdop,
                    {optional_column("rtk_speed_knots")} AS rtk_speed_knots,
                    {optional_column("rtk_track_degrees")} AS rtk_track_degrees,
                    {optional_column("gyro_x_rad_s")} AS gyro_x_rad_s,
                    {optional_column("gyro_y_rad_s")} AS gyro_y_rad_s,
                    {optional_column("gyro_z_rad_s")} AS gyro_z_rad_s,
                    {optional_column("accel_x_m_s2")} AS accel_x_m_s2,
                    {optional_column("accel_y_m_s2")} AS accel_y_m_s2,
                    {optional_column("accel_z_m_s2")} AS accel_z_m_s2,
                    {optional_column("imu_sample_count")} AS imu_sample_count,
                    {optional_column("radar_temperature_c")} AS radar_temperature_c,
                    {optional_column("minimum_point_x_m")} AS minimum_point_x_m,
                    {optional_column("minimum_point_y_m")} AS minimum_point_y_m,
                    {optional_column("minimum_point_z_m")} AS minimum_point_z_m,
                    {optional_column("vehicle_pitch_deg")} AS vehicle_pitch_deg,
                    {optional_column("vehicle_roll_deg")} AS vehicle_roll_deg,
                    {optional_column("vehicle_heading_deg")} AS vehicle_heading_deg,
                    {optional_column("odin_position_x_m")} AS odin_position_x_m,
                    {optional_column("odin_position_y_m")} AS odin_position_y_m,
                    {optional_column("odin_position_z_m")} AS odin_position_z_m,
                    {optional_column("odin_qx")} AS odin_qx,
                    {optional_column("odin_qy")} AS odin_qy,
                    {optional_column("odin_qz")} AS odin_qz,
                    {optional_column("odin_qw")} AS odin_qw
                FROM clearance_samples
                ORDER BY sample_index ASC
                """
            )
            for row in cursor:
                yield MeasurementExportSampleRecord(
                    sample_index=int(row["sample_index"]),
                    source_timestamp_ms=int(row["source_timestamp_ns"]) // 1_000_000,
                    recorded_timestamp_ms=int(row["recorded_timestamp_ns"]) // 1_000_000,
                    elapsed_ms=float(row["elapsed_ms"]),
                    height_m=_optional_float(row["clearance_height_m"]),
                    lidar_to_top_m=_optional_float(row["lidar_to_top_m"]),
                    valid=bool(row["valid"]),
                    invalid_reason=row["invalid_reason"],
                    quality_score=_optional_float(row["quality_score"]),
                    source_sequence=(
                        int(row["source_sequence"])
                        if row["source_sequence"] is not None else None
                    ),
                    source_age_ms=_optional_float(row["source_age_ms"]),
                    is_repeated=(
                        bool(row["is_repeated"])
                        if row["is_repeated"] is not None else None
                    ),
                    repeat_index=(
                        int(row["repeat_index"])
                        if row["repeat_index"] is not None else None
                    ),
                    rtk_timestamp_ms=(
                        int(row["rtk_timestamp_ns"]) // 1_000_000
                        if row["rtk_timestamp_ns"] is not None else None
                    ),
                    rtk_latitude_deg=_optional_float(row["rtk_latitude_deg"]),
                    rtk_longitude_deg=_optional_float(row["rtk_longitude_deg"]),
                    rtk_altitude_m=_optional_float(row["rtk_altitude_m"]),
                    rtk_fix_type=(
                        str(row["rtk_fix_type"])
                        if row["rtk_fix_type"] is not None else None
                    ),
                    rtk_valid=(bool(row["rtk_valid"]) if row["rtk_valid"] is not None else None),
                    rtk_satellite_count=(
                        int(row["rtk_satellite_count"])
                        if row["rtk_satellite_count"] is not None else None
                    ),
                    rtk_hdop=_optional_float(row["rtk_hdop"]),
                    rtk_pdop=_optional_float(row["rtk_pdop"]),
                    rtk_speed_knots=_optional_float(row["rtk_speed_knots"]),
                    rtk_track_degrees=_optional_float(row["rtk_track_degrees"]),
                    gyro_x_rad_s=_optional_float(row["gyro_x_rad_s"]),
                    gyro_y_rad_s=_optional_float(row["gyro_y_rad_s"]),
                    gyro_z_rad_s=_optional_float(row["gyro_z_rad_s"]),
                    accel_x_m_s2=_optional_float(row["accel_x_m_s2"]),
                    accel_y_m_s2=_optional_float(row["accel_y_m_s2"]),
                    accel_z_m_s2=_optional_float(row["accel_z_m_s2"]),
                    imu_sample_count=(
                        int(row["imu_sample_count"])
                        if row["imu_sample_count"] is not None else None
                    ),
                    radar_temperature_c=_optional_float(row["radar_temperature_c"]),
                    minimum_point_x_m=_optional_float(row["minimum_point_x_m"]),
                    minimum_point_y_m=_optional_float(row["minimum_point_y_m"]),
                    minimum_point_z_m=_optional_float(row["minimum_point_z_m"]),
                    vehicle_pitch_deg=_optional_float(row["vehicle_pitch_deg"]),
                    vehicle_roll_deg=_optional_float(row["vehicle_roll_deg"]),
                    vehicle_heading_deg=_optional_float(row["vehicle_heading_deg"]),
                    odin_position_x_m=_optional_float(row["odin_position_x_m"]),
                    odin_position_y_m=_optional_float(row["odin_position_y_m"]),
                    odin_position_z_m=_optional_float(row["odin_position_z_m"]),
                    odin_qx=_optional_float(row["odin_qx"]),
                    odin_qy=_optional_float(row["odin_qy"]),
                    odin_qz=_optional_float(row["odin_qz"]),
                    odin_qw=_optional_float(row["odin_qw"]),
                )
        except MeasurementStorageError:
            raise
        except sqlite3.Error as error:
            raise MeasurementStorageError(f"读取任务测量明细失败：{error}") from error
        finally:
            connection.close()

    def load_clearance_analysis(
        self,
        task: TaskRecord,
        config: ClearanceAnomalyConfig = DEFAULT_CLEARANCE_ANOMALY_CONFIG,
    ) -> ClearanceAnalysisResult:
        """Analyze report clearance without changing the immutable measurement database."""
        database_path = self._resolve_recording_database(task)
        lock = self._analysis_lock(task.task_id)
        with lock:
            cached = self._load_clearance_analysis_cache(database_path, config)
            if cached is not None:
                return cached
            result = analyze_clearance(
                (
                    ClearanceMeasurement(
                        sample_index=sample.sample_index,
                        height_m=sample.height_m,
                        valid=sample.valid,
                        invalid_reason=sample.invalid_reason,
                        source_sequence=sample.source_sequence,
                        is_repeated=sample.is_repeated,
                        minimum_point_x_m=sample.minimum_point_x_m,
                        minimum_point_y_m=sample.minimum_point_y_m,
                        minimum_point_z_m=sample.minimum_point_z_m,
                        odin_position_x_m=sample.odin_position_x_m,
                        odin_position_y_m=sample.odin_position_y_m,
                        odin_position_z_m=sample.odin_position_z_m,
                        vehicle_heading_deg=sample.vehicle_heading_deg,
                    )
                    for sample in self.iter_export_samples(task)
                ),
                config,
            )
            self._write_clearance_analysis_cache(database_path, config, result)
            return result

    def _analysis_lock(self, task_id: str) -> Lock:
        with self._analysis_locks_guard:
            return self._analysis_locks.setdefault(task_id, Lock())

    @staticmethod
    def _analysis_cache_path(database_path: Path) -> Path:
        return database_path.parent / "analysis" / f"{CLEARANCE_ANALYSIS_VERSION}.json"

    @staticmethod
    def _analysis_cache_identity(
        database_path: Path,
        config: ClearanceAnomalyConfig,
    ) -> dict[str, object]:
        stat = database_path.stat()
        return {
            "database_size_bytes": stat.st_size,
            "database_mtime_ns": stat.st_mtime_ns,
            "config": asdict(config),
        }

    def _load_clearance_analysis_cache(
        self,
        database_path: Path,
        config: ClearanceAnomalyConfig,
    ) -> ClearanceAnalysisResult | None:
        path = self._analysis_cache_path(database_path)
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(payload, dict):
                return None
            if payload.get("schema_version") != 1:
                return None
            if payload.get("identity") != self._analysis_cache_identity(database_path, config):
                return None
            result = payload.get("result")
            if not isinstance(result, dict):
                return None
            return ClearanceAnalysisResult.from_trace_dict(result)
        except (OSError, ValueError, KeyError, TypeError):
            return None

    def _write_clearance_analysis_cache(
        self,
        database_path: Path,
        config: ClearanceAnomalyConfig,
        result: ClearanceAnalysisResult,
    ) -> None:
        path = self._analysis_cache_path(database_path)
        temporary_path: Path | None = None
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            payload = {
                "schema_version": 1,
                "identity": self._analysis_cache_identity(database_path, config),
                "result": result.to_trace_dict(),
            }
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                dir=path.parent,
                prefix=".clearance-analysis-",
                suffix=".tmp",
                delete=False,
            ) as temporary:
                temporary_path = Path(temporary.name)
                json.dump(payload, temporary, ensure_ascii=False, indent=2)
                temporary.flush()
                os.fsync(temporary.fileno())
            os.replace(temporary_path, path)
        except OSError:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)

    def _open_readonly(self, database_path: Path) -> sqlite3.Connection:
        try:
            connection = sqlite3.connect(
                f"file:{database_path.as_posix()}?mode=ro",
                uri=True,
                timeout=5.0,
            )
            connection.row_factory = sqlite3.Row
            connection.execute("PRAGMA query_only = ON")
            connection.execute("PRAGMA busy_timeout = 5000")
            return connection
        except (OSError, sqlite3.Error) as error:
            raise MeasurementStorageError(f"无法打开任务测量数据库：{error}") from error

    @staticmethod
    def _load_metadata(connection: sqlite3.Connection, task: TaskRecord) -> sqlite3.Row:
        metadata = connection.execute(
            "SELECT * FROM recording_metadata WHERE id = 1"
        ).fetchone()
        if metadata is None:
            raise MeasurementStorageError("任务测量数据库缺少 recording_metadata")
        schema_version = int(metadata["schema_version"])
        if schema_version not in _SUPPORTED_SCHEMA_VERSIONS:
            supported = ", ".join(str(value) for value in sorted(_SUPPORTED_SCHEMA_VERSIONS))
            raise MeasurementStorageError(
                f"不支持的测量数据库版本：{schema_version}，程序支持版本：{supported}"
            )
        if metadata["task_id"] != task.task_id:
            raise MeasurementStorageError("任务测量数据库中的 task_id 与任务索引不一致")
        return metadata

    @staticmethod
    def _load_statistics(
        connection: sqlite3.Connection,
        task_id: str,
    ) -> MeasurementStatisticsRecord:
        aggregate = connection.execute(
            """
            SELECT
                COUNT(*) AS total_samples,
                SUM(CASE WHEN valid = 1 AND clearance_height_m IS NOT NULL THEN 1 ELSE 0 END) AS valid_samples,
                MIN(CASE WHEN valid = 1 THEN clearance_height_m END) AS minimum_height_m,
                AVG(CASE WHEN valid = 1 THEN clearance_height_m END) AS average_height_m,
                MAX(CASE WHEN valid = 1 THEN clearance_height_m END) AS maximum_height_m,
                MIN(recorded_timestamp_ns) AS first_timestamp_ns,
                MAX(recorded_timestamp_ns) AS last_timestamp_ns
            FROM clearance_samples
            """
        ).fetchone()
        total_samples = int(aggregate["total_samples"])
        if total_samples <= 0:
            raise MeasurementNotFoundError(task_id)
        valid_samples = int(aggregate["valid_samples"] or 0)
        first_timestamp_ns = int(aggregate["first_timestamp_ns"])
        last_timestamp_ns = int(aggregate["last_timestamp_ns"])
        duration_ms = max(0.0, (last_timestamp_ns - first_timestamp_ns) / 1_000_000.0)
        actual_average_sample_rate_hz = (
            (total_samples - 1) * 1000.0 / duration_ms
            if total_samples > 1 and duration_ms > 0
            else None
        )
        nominal_row = connection.execute(
            "SELECT nominal_sample_rate_hz FROM recording_metadata WHERE id = 1"
        ).fetchone()
        return MeasurementStatisticsRecord(
            total_samples=total_samples,
            valid_samples=valid_samples,
            invalid_samples=total_samples - valid_samples,
            minimum_height_m=_optional_float(aggregate["minimum_height_m"]),
            average_height_m=_optional_float(aggregate["average_height_m"]),
            maximum_height_m=_optional_float(aggregate["maximum_height_m"]),
            duration_ms=duration_ms,
            nominal_sample_rate_hz=float(nominal_row["nominal_sample_rate_hz"]),
            actual_average_sample_rate_hz=actual_average_sample_rate_hz,
        )

    @staticmethod
    def _load_sample_bounds(
        connection: sqlite3.Connection,
        task_id: str,
    ) -> tuple[int, int, int, int]:
        first = connection.execute(
            """
            SELECT sample_index, source_timestamp_ns
            FROM clearance_samples
            ORDER BY sample_index ASC
            LIMIT 1
            """
        ).fetchone()
        last = connection.execute(
            """
            SELECT sample_index, source_timestamp_ns
            FROM clearance_samples
            ORDER BY sample_index DESC
            LIMIT 1
            """
        ).fetchone()
        if first is None or last is None:
            raise MeasurementNotFoundError(task_id)
        return (
            int(first["sample_index"]),
            int(last["sample_index"]),
            int(first["source_timestamp_ns"]) // 1_000_000,
            int(last["source_timestamp_ns"]) // 1_000_000,
        )

    @staticmethod
    def _series_sample_from_row(row: sqlite3.Row) -> ClearanceSeriesSampleRecord:
        return ClearanceSeriesSampleRecord(
            sample_index=int(row["sample_index"]),
            timestamp_ms=int(row["source_timestamp_ns"]) // 1_000_000,
            elapsed_ms=float(row["elapsed_ms"]),
            height_m=_optional_float(row["clearance_height_m"]),
            valid=bool(row["valid"]),
            invalid_reason=row["invalid_reason"],
        )

    @classmethod
    def _load_downsampled_series(
        cls,
        connection: sqlite3.Connection,
        *,
        start_timestamp_ns: int,
        end_timestamp_ns: int,
        max_points: int,
    ) -> list[ClearanceSeriesSampleRecord]:
        """在SQLite内按真实时间桶选择极值和无效断点，避免逐行跨入Python。"""
        bucket_count = max(1, (max_points - 2) // 3)
        duration_ns = max(1, end_timestamp_ns - start_timestamp_ns + 1)
        columns = (
            "sample_index, source_timestamp_ns, elapsed_ms, "
            "clearance_height_m, valid, invalid_reason"
        )
        bucket_expression = (
            "MIN(? - 1, CAST(((source_timestamp_ns - ?) * ?) / ? AS INTEGER))"
        )
        sql = f"""
            SELECT DISTINCT {columns}
            FROM (
                SELECT {columns}, MIN(clearance_height_m) AS selected_value
                FROM clearance_samples
                WHERE source_timestamp_ns BETWEEN ? AND ?
                  AND valid = 1 AND clearance_height_m IS NOT NULL
                GROUP BY {bucket_expression}

                UNION ALL

                SELECT {columns}, MAX(clearance_height_m) AS selected_value
                FROM clearance_samples
                WHERE source_timestamp_ns BETWEEN ? AND ?
                  AND valid = 1 AND clearance_height_m IS NOT NULL
                GROUP BY {bucket_expression}

                UNION ALL

                SELECT {columns}, MIN(sample_index) AS selected_value
                FROM clearance_samples
                WHERE source_timestamp_ns BETWEEN ? AND ?
                  AND (valid = 0 OR clearance_height_m IS NULL)
                GROUP BY {bucket_expression}

                UNION ALL

                SELECT * FROM (
                    SELECT {columns}, NULL AS selected_value
                    FROM clearance_samples
                    WHERE source_timestamp_ns BETWEEN ? AND ?
                    ORDER BY source_timestamp_ns ASC, sample_index ASC
                    LIMIT 1
                )

                UNION ALL

                SELECT * FROM (
                    SELECT {columns}, NULL AS selected_value
                    FROM clearance_samples
                    WHERE source_timestamp_ns BETWEEN ? AND ?
                    ORDER BY source_timestamp_ns DESC, sample_index DESC
                    LIMIT 1
                )
            )
            ORDER BY source_timestamp_ns ASC, sample_index ASC
        """
        bucket_parameters = (
            bucket_count,
            start_timestamp_ns,
            bucket_count,
            duration_ns,
        )
        parameters = (
            start_timestamp_ns,
            end_timestamp_ns,
            *bucket_parameters,
            start_timestamp_ns,
            end_timestamp_ns,
            *bucket_parameters,
            start_timestamp_ns,
            end_timestamp_ns,
            *bucket_parameters,
            start_timestamp_ns,
            end_timestamp_ns,
            start_timestamp_ns,
            end_timestamp_ns,
        )
        rows = connection.execute(sql, parameters).fetchall()
        return [cls._series_sample_from_row(row) for row in rows]

    @staticmethod
    def _load_endpoints(connection: sqlite3.Connection) -> dict[str, RtkEndpointRecord]:
        return {
            row["role"]: RtkEndpointRecord(
                timestamp_ms=int(row["timestamp_ns"]) // 1_000_000,
                latitude_deg=float(row["latitude_deg"]),
                longitude_deg=float(row["longitude_deg"]),
                altitude_m=_optional_float(row["altitude_m"]),
                fix_type=str(row["fix_type"]),
                valid=bool(row["valid"]),
            )
            for row in connection.execute(
                """
                SELECT role, timestamp_ns, latitude_deg, longitude_deg,
                       altitude_m, fix_type, valid
                FROM rtk_endpoints
                ORDER BY role
                """
            ).fetchall()
        }

    @staticmethod
    def _summary_from_parts(
        *,
        task: TaskRecord,
        metadata: sqlite3.Row,
        statistics: MeasurementStatisticsRecord,
        endpoints: dict[str, RtkEndpointRecord],
        pause_interval_count: int,
        sample_bounds: tuple[int, int, int, int],
    ) -> MeasurementSummaryRecord:
        data_origin = str(metadata["data_origin"])
        if data_origin not in {"recorded", "test_fixture"}:
            raise MeasurementStorageError(f"未知测量数据来源：{data_origin}")
        lane = str(metadata["lane"])
        if lane not in {"left", "right", "unknown"}:
            raise MeasurementStorageError(f"未知检测车道：{lane}")
        metadata_keys = set(metadata.keys())
        travel_direction = (
            str(metadata["travel_direction"]) if "travel_direction" in metadata_keys else "unknown"
        )
        lane_side = str(metadata["lane_side"]) if "lane_side" in metadata_keys else lane
        if travel_direction not in {"up", "down", "unknown"}:
            raise MeasurementStorageError(f"未知行驶方向：{travel_direction}")
        if lane_side not in {"left", "right", "unknown"}:
            raise MeasurementStorageError(f"未知车道位置：{lane_side}")
        first_sample_index, last_sample_index, first_timestamp_ms, last_timestamp_ms = sample_bounds
        return MeasurementSummaryRecord(
            task_id=task.task_id,
            recording_schema_version=int(metadata["schema_version"]),
            data_origin=cast(MeasurementDataOrigin, data_origin),
            lane=cast(MeasurementLane, lane),
            travel_direction=cast(MeasurementTravelDirection, travel_direction),
            lane_side=cast(MeasurementLane, lane_side),
            started_at=str(metadata["started_at"]),
            ended_at=metadata["ended_at"],
            complete=bool(metadata["complete"]),
            algorithm_version=metadata["algorithm_version"],
            config_version=metadata["config_version"],
            software_version=metadata["software_version"],
            statistics=statistics,
            entry_rtk=endpoints.get("entry"),
            exit_rtk=endpoints.get("exit"),
            pause_interval_count=pause_interval_count,
            first_sample_index=first_sample_index,
            last_sample_index=last_sample_index,
            first_timestamp_ms=first_timestamp_ms,
            last_timestamp_ms=last_timestamp_ms,
        )

    def _resolve_recording_database(self, task: TaskRecord) -> Path:
        if not task.has_measurements or not task.recording_path:
            raise MeasurementNotFoundError(task.task_id)
        relative_path = Path(task.recording_path)
        if relative_path.is_absolute():
            raise MeasurementStorageError("recording_path 必须是 tasks 目录内的相对路径")
        candidate = (self.tasks_directory / relative_path).resolve()
        try:
            candidate.relative_to(self.tasks_directory)
        except ValueError as error:
            raise MeasurementStorageError("recording_path 超出 tasks 数据目录") from error
        if not candidate.is_file():
            raise MeasurementStorageError(f"任务测量数据库不存在：{task.recording_path}")
        return candidate


def _optional_float(value: object) -> float | None:
    return float(value) if value is not None else None
