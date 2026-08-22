from __future__ import annotations

from dataclasses import replace

from backend.measurements.clearance_anomaly import (
    DEFAULT_CLEARANCE_ANOMALY_CONFIG,
    ClearanceEventStatus,
    ClearanceMeasurement,
    analyze_clearance,
)


def measurement(
    index: int,
    distance_m: float,
    height_m: float,
    *,
    point: tuple[float, float, float] | None = None,
    valid: bool = True,
) -> ClearanceMeasurement:
    minimum = point if point is not None else (0.0, 0.0, height_m - 2.25)
    return ClearanceMeasurement(
        sample_index=index,
        height_m=height_m,
        valid=valid,
        source_sequence=index + 1,
        is_repeated=False,
        minimum_point_x_m=minimum[0],
        minimum_point_y_m=minimum[1],
        minimum_point_z_m=minimum[2],
        odin_position_x_m=distance_m,
        odin_position_y_m=0.0,
        odin_position_z_m=0.0,
        vehicle_heading_deg=90.0,
    )


def normal_tunnel(count: int = 61, spacing_m: float = 1.0) -> list[ClearanceMeasurement]:
    return [
        measurement(index, index * spacing_m, 7.0 + 0.02 * ((index % 5) - 2))
        for index in range(count)
    ]


def fixed_structure_point(structure_s_m: float, vehicle_s_m: float, height_m: float) -> tuple[float, float, float]:
    return (structure_s_m - vehicle_s_m, 0.0, height_m - 2.25)


def test_normal_tunnel_variation_has_no_low_event() -> None:
    result = analyze_clearance(normal_tunnel())
    assert result.low_clearance_events == ()
    assert result.raw_min_clearance_m == 6.96
    assert result.effective_min_clearance_m == 6.96


def test_isolated_very_low_measurement_is_high_confidence_outlier() -> None:
    records = normal_tunnel()
    records[30] = measurement(30, 30.0, 3.8, point=(25.0, 10.0, 1.55))
    result = analyze_clearance(records)
    event = result.low_clearance_events[0]
    assert event.status == ClearanceEventStatus.HIGH_CONFIDENCE_OUTLIER
    assert "ISOLATED_MEASUREMENT:+2" in event.matched_rules
    assert "IMMEDIATE_TWO_SIDED_RECOVERY:+2" in event.matched_rules
    assert result.raw_min_clearance_m == 3.8
    assert result.effective_min_clearance_m > 6.9


def test_two_meter_continuous_low_structure_is_preserved() -> None:
    records = normal_tunnel()
    for index in range(29, 32):
        records[index] = measurement(
            index,
            float(index),
            5.4,
            point=fixed_structure_point(30.0, float(index), 5.4),
        )
    event = analyze_clearance(records).low_clearance_events[0]
    assert event.length_m == 2.0
    assert event.point_trajectory_continuous is True
    assert event.status == ClearanceEventStatus.VALID_STRUCTURE


def test_three_similar_equally_spaced_structures_are_periodically_protected() -> None:
    records = normal_tunnel(81)
    for center in (15, 35, 55):
        for index in range(center - 1, center + 2):
            records[index] = measurement(
                index,
                float(index),
                5.5,
                point=fixed_structure_point(float(center), float(index), 5.5),
            )
    events = analyze_clearance(records).low_clearance_events
    assert len(events) == 3
    assert all(
        event.status == ClearanceEventStatus.PROTECTED_PERIODIC_STRUCTURE
        for event in events
    )
    assert all(abs(float(event.estimated_period_m) - 20.0) < 0.1 for event in events)


def test_two_similar_events_do_not_establish_periodicity() -> None:
    records = normal_tunnel()
    for center in (15, 35):
        for index in range(center - 1, center + 2):
            records[index] = measurement(
                index,
                float(index),
                5.5,
                point=fixed_structure_point(float(center), float(index), 5.5),
            )
    events = analyze_clearance(records).low_clearance_events
    assert len(events) == 2
    assert all(not event.periodic_protected for event in events)


def test_unrelated_outlier_among_periodic_fans_is_not_protected() -> None:
    records = normal_tunnel(81)
    for center in (15, 35, 55):
        for index in range(center - 1, center + 2):
            records[index] = measurement(
                index,
                float(index),
                5.5,
                point=fixed_structure_point(float(center), float(index), 5.5),
            )
    records[45] = measurement(45, 45.0, 3.8, point=(20.0, 8.0, 1.55))
    events = analyze_clearance(records).low_clearance_events
    outlier = next(event for event in events if event.min_height_m == 3.8)
    assert outlier.periodic_protected is False
    assert outlier.status == ClearanceEventStatus.HIGH_CONFIDENCE_OUTLIER


def test_single_nonperiodic_beam_with_continuous_measurements_is_preserved() -> None:
    records = normal_tunnel()
    for index in range(24, 28):
        records[index] = measurement(
            index,
            float(index),
            5.2,
            point=fixed_structure_point(25.5, float(index), 5.2),
        )
    event = analyze_clearance(records).low_clearance_events[0]
    assert event.periodic_protected is False
    assert event.status == ClearanceEventStatus.VALID_STRUCTURE


def test_single_low_value_with_large_point_jump_gets_high_score() -> None:
    records = normal_tunnel()
    records[20] = measurement(20, 20.0, 4.0, point=(100.0, -50.0, 1.75))
    event = analyze_clearance(records).low_clearance_events[0]
    assert event.anomaly_score >= 4
    assert "LARGE_MINIMUM_POINT_JUMP:+1" in event.matched_rules


def test_zero_height_and_zero_minimum_point_are_invalid() -> None:
    records = normal_tunnel()
    records[10] = measurement(10, 10.0, 0.0, point=(1.0, 1.0, 1.0))
    records[20] = measurement(20, 20.0, 5.0, point=(0.0, 0.0, 0.0))
    result = analyze_clearance(records)
    assert result.invalid_record_count == 2
    assert result.raw_min_clearance_m > 6.9


def test_insufficient_local_window_never_auto_deletes_low_value() -> None:
    records = [measurement(index, float(index), 7.0) for index in range(7)]
    records[3] = measurement(3, 3.0, 3.8, point=(20.0, 10.0, 1.55))
    result = analyze_clearance(records)
    event = result.low_clearance_events[0]
    assert event.analysis_sufficient is False
    assert event.status == ClearanceEventStatus.REVIEW_REQUIRED
    assert result.effective_min_clearance_m == 3.8


def test_distance_domain_result_is_stable_when_measurement_frequency_changes() -> None:
    coarse = normal_tunnel(61, 1.0)
    fine = normal_tunnel(121, 0.5)
    for records, spacing in ((coarse, 1.0), (fine, 0.5)):
        for index, record in enumerate(records):
            distance_m = index * spacing
            if 29.0 <= distance_m <= 31.0:
                records[index] = measurement(
                    index,
                    distance_m,
                    5.4,
                    point=fixed_structure_point(30.0, distance_m, 5.4),
                )
    coarse_event = analyze_clearance(coarse).low_clearance_events[0]
    fine_event = analyze_clearance(fine).low_clearance_events[0]
    assert coarse_event.status == fine_event.status == ClearanceEventStatus.VALID_STRUCTURE
    assert abs(coarse_event.length_m - fine_event.length_m) < 0.1


def test_identical_real_heights_are_not_deduplicated_by_value() -> None:
    records = [measurement(index, float(index), 7.0) for index in range(30)]
    result = analyze_clearance(records)
    assert result.valid_record_count == 30
    assert result.duplicate_source_record_count == 0


def test_distance_reliability_recovers_after_an_odometry_gap() -> None:
    records = normal_tunnel()
    records[20] = replace(
        records[20],
        odin_position_x_m=None,
        odin_position_y_m=None,
        odin_position_z_m=None,
    )
    for index in range(29, 32):
        records[index] = measurement(
            index,
            float(index),
            5.4,
            point=fixed_structure_point(30.0, float(index), 5.4),
        )

    event = analyze_clearance(records).low_clearance_events[0]
    assert event.analysis_sufficient is True
    assert event.status == ClearanceEventStatus.VALID_STRUCTURE


def test_invalid_odometry_uses_bounded_fallback_and_never_auto_excludes_low_value() -> None:
    config = replace(
        DEFAULT_CLEARANCE_ANOMALY_CONFIG,
        maximum_local_samples=65,
        fallback_window_radius_samples=16,
    )
    records = [measurement(index, float(index) * 100.0, 7.0) for index in range(2000)]
    records[1000] = measurement(1000, 100000.0, 3.8, point=(20.0, 10.0, 1.55))

    result = analyze_clearance(records, config)

    event = next(event for event in result.low_clearance_events if event.min_height_m == 3.8)
    assert event.analysis_sufficient is False
    assert event.status == ClearanceEventStatus.REVIEW_REQUIRED
    assert result.effective_min_clearance_m == 3.8


def test_too_many_periodic_candidates_degrade_to_review() -> None:
    config = replace(DEFAULT_CLEARANCE_ANOMALY_CONFIG, periodic_max_candidate_events=4)
    records = normal_tunnel(121)
    for center in (10, 30, 50, 70, 90):
        records[center] = measurement(center, float(center), 5.0)

    result = analyze_clearance(records, config)

    assert len(result.low_clearance_events) == 5
    assert all(event.status == ClearanceEventStatus.REVIEW_REQUIRED for event in result.low_clearance_events)
