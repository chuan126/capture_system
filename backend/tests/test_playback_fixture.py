from pathlib import Path
import sqlite3

from backend.measurements.repository import MeasurementRepository
from backend.tasks.repository import TaskRepository
from tools.generate_playback_test_fixture import (
    TASK_COMPLETED,
    TASK_INTERRUPTED,
    TASK_PENDING,
    generate,
)


def test_playback_fixture_matches_current_task_and_measurement_schemas(tmp_path: Path) -> None:
    data_root = tmp_path / "fixture"
    generate(data_root, force=False)

    repository = TaskRepository(data_root / "capture.db", data_root / "tasks")
    tasks = repository.list_tasks(order="asc")
    assert [task.task_id for task in tasks] == [TASK_PENDING, TASK_COMPLETED, TASK_INTERRUPTED]
    assert [task.display_id for task in tasks] == [
        "20260806_085000",
        "20260806_085500",
        "20260806_095500",
    ]

    completed = next(task for task in tasks if task.task_id == TASK_COMPLETED)
    history = MeasurementRepository(data_root / "tasks").load_history(completed)
    assert history.recording_schema_version == 3
    assert history.data_origin == "test_fixture"
    assert history.complete is True
    assert history.statistics.total_samples == 1500

    with sqlite3.connect(data_root / "tasks" / TASK_COMPLETED / "measurements.db") as connection:
        sample_columns = {row[1] for row in connection.execute("PRAGMA table_info(clearance_samples)")}
        assert {"source_sequence", "source_age_ms", "is_repeated", "repeat_index"}.issubset(sample_columns)
        assert connection.execute("SELECT COUNT(*) FROM clearance_source_frames").fetchone()[0] == 1500
        raw_height, recorded_height = connection.execute(
            "SELECT lidar_to_top_m, clearance_height_m FROM clearance_samples WHERE valid=1 LIMIT 1"
        ).fetchone()
        assert round(recorded_height - raw_height, 6) == 2.30
