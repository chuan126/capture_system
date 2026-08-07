from __future__ import annotations

import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, cast

from backend.measurements.models import MeasurementDataOrigin, MeasurementLane
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
class MeasurementSummaryRecord:
    task_id: str
    recording_schema_version: int
    data_origin: MeasurementDataOrigin
    lane: MeasurementLane
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


@dataclass(frozen=True)
class MeasurementHistoryRecord:
    task_id: str
    recording_schema_version: int
    data_origin: MeasurementDataOrigin
    lane: MeasurementLane
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


_SUPPORTED_SCHEMA_VERSIONS = {1, 2}
_MAX_HISTORY_SAMPLES = 500_000


class MeasurementRepository:
    def __init__(self, tasks_directory: Path) -> None:
        self.tasks_directory = tasks_directory.resolve()

    def load_summary(self, task: TaskRecord) -> MeasurementSummaryRecord:
        database_path = self._resolve_recording_database(task)
        connection = self._open_readonly(database_path)
        try:
            metadata = self._load_metadata(connection, task)
            statistics = self._load_statistics(connection, task.task_id)
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
            )
        except (MeasurementNotFoundError, MeasurementStorageError):
            raise
        except sqlite3.Error as error:
            raise MeasurementStorageError(f"读取任务测量数据库失败：{error}") from error
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
            )
            return MeasurementHistoryRecord(
                task_id=summary.task_id,
                recording_schema_version=summary.recording_schema_version,
                data_origin=summary.data_origin,
                lane=summary.lane,
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
                    {repeat_index_expr} AS repeat_index
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
                )
        except MeasurementStorageError:
            raise
        except sqlite3.Error as error:
            raise MeasurementStorageError(f"读取任务测量明细失败：{error}") from error
        finally:
            connection.close()

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
    ) -> MeasurementSummaryRecord:
        data_origin = str(metadata["data_origin"])
        if data_origin not in {"recorded", "test_fixture"}:
            raise MeasurementStorageError(f"未知测量数据来源：{data_origin}")
        lane = str(metadata["lane"])
        if lane not in {"left", "right", "unknown"}:
            raise MeasurementStorageError(f"未知检测车道：{lane}")
        return MeasurementSummaryRecord(
            task_id=task.task_id,
            recording_schema_version=int(metadata["schema_version"]),
            data_origin=cast(MeasurementDataOrigin, data_origin),
            lane=cast(MeasurementLane, lane),
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
