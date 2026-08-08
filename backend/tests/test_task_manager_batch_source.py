from pathlib import Path


def test_task_manager_does_not_use_legacy_batch_state_for_task_control() -> None:
    source = (
        Path(__file__).parents[2]
        / "ros2_ws"
        / "src"
        / "task_manager"
        / "src"
        / "task_manager_node.cpp"
    ).read_text(encoding="utf-8")

    assert "COALESCE(tasks.batch_sequence,tasks.sequence)" in source
    assert "FROM tasks WHERE tasks.task_id=? AND tasks.deleted_at IS NULL" in source
    assert "JOIN operation_batches ON operation_batches.batch_id=tasks.batch_id" not in source
    assert "batch_status" not in source
    assert "batch_not_active" not in source
    assert "任务所属作业已经结束" not in source


def test_task_manager_releases_stuck_transitions_without_system_restart() -> None:
    source = (
        Path(__file__).parents[2]
        / "ros2_ws"
        / "src"
        / "task_manager"
        / "src"
        / "task_manager_node.cpp"
    ).read_text(encoding="utf-8")

    assert '"/capture/task/recover"' in source
    assert "check_transition_watchdog" in source
    assert "transition_deadline_at" in source
    assert "recover_active_task" in source
    assert "recover_transition_task" in source
    assert "interrupt_active_task" in source
    assert "cancel_preparing_task" in source
    assert 'call_recorder_command(task_id, "abort"' in source
    assert 'phase == "radar_initializing"' in source
    assert 'phase == "recorder_preparing"' in source
    assert 'task.phase == "pausing"' in source
    assert 'task.phase == "resuming"' in source
    assert "active_slot=NULL" in source
    assert "transition_started_at=NULL, transition_deadline_at=NULL" in source
