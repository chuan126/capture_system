from pathlib import Path

from backend.measurements.clearance_anomaly import (
    CLEARANCE_ANALYSIS_VERSION,
    DEFAULT_CLEARANCE_ANOMALY_CONFIG,
    analyze_clearance,
)
from backend.measurements.repository import MeasurementRepository


def test_late_analysis_cache_write_does_not_recreate_deleted_task_directory(
    tmp_path: Path,
) -> None:
    tasks_directory = tmp_path / "tasks"
    database_path = tasks_directory / "deleted-task" / "measurements.db"
    repository = MeasurementRepository(tasks_directory)

    repository._write_clearance_analysis_cache(
        database_path,
        DEFAULT_CLEARANCE_ANOMALY_CONFIG,
        analyze_clearance([]),
    )

    assert not database_path.parent.exists()


def test_analysis_cache_is_written_beside_an_existing_measurement_database(
    tmp_path: Path,
) -> None:
    tasks_directory = tmp_path / "tasks"
    database_path = tasks_directory / "existing-task" / "measurements.db"
    database_path.parent.mkdir(parents=True)
    database_path.write_bytes(b"measurement identity")
    repository = MeasurementRepository(tasks_directory)

    repository._write_clearance_analysis_cache(
        database_path,
        DEFAULT_CLEARANCE_ANOMALY_CONFIG,
        analyze_clearance([]),
    )

    assert (
        database_path.parent / "analysis" / f"{CLEARANCE_ANALYSIS_VERSION}.json"
    ).is_file()
