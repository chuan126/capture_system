from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_data_recorder_rejects_stale_rtk_endpoints_by_receive_age() -> None:
    source = (ROOT / "ros2_ws/src/data_recorder/src/data_recorder_node.cpp").read_text(encoding="utf-8")
    config = (ROOT / "ros2_ws/src/data_recorder/config/data_recorder.yaml").read_text(encoding="utf-8")
    launch = (ROOT / "ros2_ws/src/bringup/launch/task_control.launch.py").read_text(encoding="utf-8")

    assert 'declare_parameter<double>("endpoint_rtk_max_age_ms", 2000.0)' in source
    assert "latest_fix_.received_monotonic_ns = steady_now_ns();" in source
    assert "steady_now_ns() - latest_fix_.received_monotonic_ns" in source
    assert '(!latest_fix_.valid ? "invalid_fix" : "stale_fix")' in source
    assert "latest_fix_.valid && fresh ? 1 : 0" in source
    assert "endpoint_rtk_max_age_ms: 2000.0" in config
    assert '"endpoint_rtk_max_age_ms": 2000.0' in launch


def test_measurement_schema_v5_uses_semantic_source_diagnostic_names() -> None:
    source = (ROOT / "ros2_ws/src/data_recorder/src/data_recorder_node.cpp").read_text(encoding="utf-8")

    assert "VALUES (1, 12," in source
    assert "travel_direction TEXT NOT NULL" in source
    assert "lane_side TEXT NOT NULL" in source
    assert "candidate_region_count INTEGER" in source
    assert "selected_grid_area_m2 REAL" in source
    assert "selected_residual_median_m REAL" in source
    assert "selected_residual_p95_m REAL" in source
    assert "source.message.residual_p95_m" in source
    assert "candidate_plane_count INTEGER" not in source
    assert "selected_rms_m REAL" not in source


def test_pause_and_resume_rtk_snapshots_use_freshness_argument() -> None:
    source = (ROOT / "ros2_ws/src/data_recorder/src/data_recorder_node.cpp").read_text(encoding="utf-8")

    assert 'capture_event_rtk("pause", requested_ns, latest_fix_is_fresh());' in source
    assert 'capture_event_rtk("resume", requested_ns, latest_fix_is_fresh());' in source
    assert 'capture_event_rtk("pause", requested_ns);' not in source
    assert 'capture_event_rtk("resume", requested_ns);' not in source
