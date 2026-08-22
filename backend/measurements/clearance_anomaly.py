from __future__ import annotations

import math
import statistics
from dataclasses import asdict, dataclass, field
from enum import Enum
from typing import Iterable


class ClearanceEventStatus(str, Enum):
    VALID_STRUCTURE = "VALID_STRUCTURE"
    REVIEW_REQUIRED = "REVIEW_REQUIRED"
    HIGH_CONFIDENCE_OUTLIER = "HIGH_CONFIDENCE_OUTLIER"
    PROTECTED_PERIODIC_STRUCTURE = "PROTECTED_PERIODIC_STRUCTURE"


@dataclass(frozen=True)
class ClearanceAnomalyConfig:
    # Local baseline window and robust low-value candidate detection.
    local_window_radius_m: float = 20.0
    local_baseline_quantile: float = 0.70
    absolute_drop_threshold_m: float = 0.50
    mad_multiplier: float = 6.0
    mad_scale: float = 1.4826
    minimum_local_samples: int = 9
    minimum_local_span_m: float = 8.0
    # 车辆静止或里程计异常时，空间窗口可能覆盖整条任务。限制参与一次稳健统计的
    # 样本数，避免报告分析退化为二次复杂度；证据不足的结果仍保守保留。
    maximum_local_samples: int = 1001
    fallback_window_radius_samples: int = 64

    # Distance construction and low-event clustering.
    maximum_odometry_step_m: float = 10.0
    event_merge_gap_m: float = 2.0
    flank_search_distance_m: float = 5.0
    normal_return_tolerance_m: float = 0.25

    # Single real structure protection and point-trajectory checks.
    structure_min_samples: int = 3
    structure_min_length_m: float = 0.80
    structure_max_height_mad_m: float = 0.15
    short_event_length_m: float = 0.30
    large_drop_m: float = 1.00
    point_boundary_jump_m: float = 2.00
    world_point_spread_m: float = 1.00
    world_point_spread_length_ratio: float = 0.25

    # Periodic structure protection.
    periodic_min_events: int = 3
    periodic_min_spacing_m: float = 5.0
    periodic_position_tolerance_m: float = 1.50
    periodic_position_tolerance_ratio: float = 0.05
    periodic_height_tolerance_m: float = 0.20
    periodic_lateral_tolerance_m: float = 0.50
    periodic_vertical_tolerance_m: float = 0.30
    periodic_length_ratio_max: float = 2.0
    # 周期候选配对包含多层遍历。候选过多通常意味着定位或参数异常，此时不做
    # 自动周期保护/剔除，统一降级为待复核。
    periodic_max_candidate_events: int = 128

    # Explainable score weights and classification thresholds.
    score_isolated: int = 2
    score_short: int = 2
    score_large_drop: int = 2
    score_immediate_recovery: int = 2
    score_point_jump: int = 1
    score_poor_point_continuity: int = 2
    score_multiple_samples: int = -1
    score_meaningful_length: int = -1
    score_stable_height: int = -1
    score_structure_continuity: int = -2
    score_periodic: int = -4
    review_score_threshold: int = 2
    high_confidence_score_threshold: int = 4


DEFAULT_CLEARANCE_ANOMALY_CONFIG = ClearanceAnomalyConfig()
CLEARANCE_ANALYSIS_VERSION = "clearance-anomaly-distance-v2"


@dataclass(frozen=True)
class ClearanceMeasurement:
    sample_index: int
    height_m: float | None
    valid: bool
    invalid_reason: str | None = None
    source_sequence: int | None = None
    is_repeated: bool | None = None
    minimum_point_x_m: float | None = None
    minimum_point_y_m: float | None = None
    minimum_point_z_m: float | None = None
    odin_position_x_m: float | None = None
    odin_position_y_m: float | None = None
    odin_position_z_m: float | None = None
    vehicle_heading_deg: float | None = None


@dataclass
class AnalyzedClearanceMeasurement:
    measurement: ClearanceMeasurement
    distance_m: float = 0.0
    distance_reliable: bool = False
    distance_segment_id: int = 0
    valid: bool = False
    invalid_reason: str | None = None
    local_baseline_m: float | None = None
    local_mad_m: float | None = None
    local_drop_m: float | None = None
    local_sample_count: int = 0
    local_span_m: float = 0.0
    low_candidate: bool = False


@dataclass
class ClearanceLowEvent:
    event_id: str
    sample_indices: list[int]
    start_s_m: float
    end_s_m: float
    center_s_m: float
    length_m: float
    sample_count: int
    min_height_m: float
    median_height_m: float
    local_baseline_m: float
    max_drop_m: float
    representative_point_x_m: float | None
    representative_point_y_m: float | None
    representative_point_z_m: float | None
    representative_lateral_m: float | None
    height_mad_m: float
    front_normal_height_m: float | None
    back_normal_height_m: float | None
    entry_gradient_m_per_m: float | None
    exit_gradient_m_per_m: float | None
    boundary_point_jump_m: float | None
    world_point_spread_m: float | None
    point_trajectory_continuous: bool | None
    analysis_sufficient: bool
    periodic_protected: bool = False
    periodic_group_id: str | None = None
    estimated_period_m: float | None = None
    anomaly_score: int = 0
    matched_rules: list[str] = field(default_factory=list)
    status: ClearanceEventStatus = ClearanceEventStatus.REVIEW_REQUIRED

    def to_trace_dict(self) -> dict[str, object]:
        payload = asdict(self)
        payload["status"] = self.status.value
        return payload


@dataclass(frozen=True)
class ClearanceAnalysisResult:
    raw_min_clearance_m: float | None
    effective_min_clearance_m: float | None
    valid_record_count: int
    invalid_record_count: int
    duplicate_source_record_count: int
    low_clearance_events: tuple[ClearanceLowEvent, ...]

    @property
    def protected_structure_events(self) -> tuple[ClearanceLowEvent, ...]:
        return tuple(
            event
            for event in self.low_clearance_events
            if event.status
            in {
                ClearanceEventStatus.VALID_STRUCTURE,
                ClearanceEventStatus.PROTECTED_PERIODIC_STRUCTURE,
            }
        )

    @property
    def review_required_events(self) -> tuple[ClearanceLowEvent, ...]:
        return tuple(
            event
            for event in self.low_clearance_events
            if event.status == ClearanceEventStatus.REVIEW_REQUIRED
        )

    @property
    def outlier_events(self) -> tuple[ClearanceLowEvent, ...]:
        return tuple(
            event
            for event in self.low_clearance_events
            if event.status == ClearanceEventStatus.HIGH_CONFIDENCE_OUTLIER
        )

    @property
    def has_review_required(self) -> bool:
        return bool(self.review_required_events)

    def to_trace_dict(self) -> dict[str, object]:
        return {
            "algorithm_version": CLEARANCE_ANALYSIS_VERSION,
            "raw_min_clearance_m": self.raw_min_clearance_m,
            "effective_min_clearance_m": self.effective_min_clearance_m,
            "valid_record_count": self.valid_record_count,
            "invalid_record_count": self.invalid_record_count,
            "duplicate_source_record_count": self.duplicate_source_record_count,
            "has_review_required": self.has_review_required,
            "review_required_count": len(self.review_required_events),
            "outlier_count": len(self.outlier_events),
            "protected_structure_count": len(self.protected_structure_events),
            "events": [event.to_trace_dict() for event in self.low_clearance_events],
        }

    @staticmethod
    def from_trace_dict(payload: dict[str, object]) -> "ClearanceAnalysisResult":
        if payload.get("algorithm_version") != CLEARANCE_ANALYSIS_VERSION:
            raise ValueError("净空异常分析缓存版本不匹配")
        raw_events = payload.get("events")
        if not isinstance(raw_events, list):
            raise ValueError("净空异常分析缓存缺少事件列表")
        events: list[ClearanceLowEvent] = []
        for raw_event in raw_events:
            if not isinstance(raw_event, dict):
                raise ValueError("净空异常分析缓存事件格式无效")
            event_payload = dict(raw_event)
            event_payload["status"] = ClearanceEventStatus(str(event_payload["status"]))
            events.append(ClearanceLowEvent(**event_payload))
        return ClearanceAnalysisResult(
            raw_min_clearance_m=_optional_cache_float(payload.get("raw_min_clearance_m")),
            effective_min_clearance_m=_optional_cache_float(
                payload.get("effective_min_clearance_m")
            ),
            valid_record_count=int(payload["valid_record_count"]),
            invalid_record_count=int(payload["invalid_record_count"]),
            duplicate_source_record_count=int(payload["duplicate_source_record_count"]),
            low_clearance_events=tuple(events),
        )


def _optional_cache_float(value: object) -> float | None:
    if value is None:
        return None
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValueError("净空异常分析缓存包含非有限数值")
    return parsed


def _finite(value: float | None) -> bool:
    return value is not None and math.isfinite(value)


def _point(record: ClearanceMeasurement) -> tuple[float, float, float] | None:
    values = (
        record.minimum_point_x_m,
        record.minimum_point_y_m,
        record.minimum_point_z_m,
    )
    return tuple(float(value) for value in values) if all(_finite(value) for value in values) else None


def _odometry(record: ClearanceMeasurement) -> tuple[float, float, float] | None:
    values = (
        record.odin_position_x_m,
        record.odin_position_y_m,
        record.odin_position_z_m,
    )
    return tuple(float(value) for value in values) if all(_finite(value) for value in values) else None


def _distance(first: tuple[float, ...], second: tuple[float, ...]) -> float:
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(first, second, strict=True)))


def _median(values: Iterable[float]) -> float:
    return float(statistics.median(values))


def _mad(values: list[float]) -> float:
    if not values:
        return 0.0
    center = _median(values)
    return _median(abs(value - center) for value in values)


def _quantile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = probability * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] + fraction * (ordered[upper] - ordered[lower])


def _validate_and_calculate_distance(
    records: list[ClearanceMeasurement], config: ClearanceAnomalyConfig
) -> tuple[list[AnalyzedClearanceMeasurement], int]:
    analyzed: list[AnalyzedClearanceMeasurement] = []
    seen_sequences: set[int] = set()
    duplicate_count = 0
    distance_m = 0.0
    distance_segment_id = 0
    previous_odometry: tuple[float, float, float] | None = None
    for record in records:
        odometry = _odometry(record)
        distance_reliable = odometry is not None and not analyzed
        if previous_odometry is not None and odometry is not None:
            step_m = _distance(previous_odometry, odometry)
            if math.isfinite(step_m) and step_m <= config.maximum_odometry_step_m:
                distance_m += step_m
                distance_reliable = True
            else:
                distance_reliable = False
                distance_segment_id += 1
        elif analyzed:
            distance_reliable = False
            distance_segment_id += 1

        if odometry is not None:
            previous_odometry = odometry
        else:
            # Break the adjacency chain. Two consecutive plausible positions after a
            # gap establish a new reliable distance segment without trusting the gap.
            previous_odometry = None

        item = AnalyzedClearanceMeasurement(
            measurement=record,
            distance_m=distance_m,
            distance_reliable=distance_reliable,
            distance_segment_id=distance_segment_id,
        )
        duplicate = bool(record.is_repeated)
        if record.source_sequence is not None:
            duplicate = duplicate or record.source_sequence in seen_sequences
            seen_sequences.add(record.source_sequence)
        if duplicate:
            item.invalid_reason = "duplicate_source_frame"
            duplicate_count += 1
        elif not record.valid:
            item.invalid_reason = record.invalid_reason or "device_invalid"
        elif not _finite(record.height_m) or float(record.height_m) <= 0.0:
            item.invalid_reason = "invalid_realtime_height"
        else:
            minimum_point = _point(record)
            if minimum_point is not None and all(abs(value) <= 1.0e-12 for value in minimum_point):
                item.invalid_reason = "zero_minimum_point"
            else:
                item.valid = True
        analyzed.append(item)
    return analyzed, duplicate_count


def _calculate_local_baselines(
    analyzed: list[AnalyzedClearanceMeasurement], config: ClearanceAnomalyConfig
) -> None:
    valid = [item for item in analyzed if item.valid]
    if not valid:
        return

    def apply_local(
        item: AnalyzedClearanceMeasurement,
        candidates: list[AnalyzedClearanceMeasurement],
    ) -> None:
        local = candidates
        heights = [float(candidate.measurement.height_m) for candidate in local]
        item.local_sample_count = len(local)
        item.local_span_m = (
            max(candidate.distance_m for candidate in local)
            - min(candidate.distance_m for candidate in local)
            if local
            else 0.0
        )
        if not heights:
            return
        item.local_baseline_m = _quantile(heights, config.local_baseline_quantile)
        item.local_mad_m = _mad(heights)
        item.local_drop_m = item.local_baseline_m - float(item.measurement.height_m)
        threshold_m = max(
            config.absolute_drop_threshold_m,
            config.mad_multiplier * config.mad_scale * item.local_mad_m,
        )
        item.low_candidate = item.local_drop_m > threshold_m

    reliable_segments: dict[int, list[AnalyzedClearanceMeasurement]] = {}
    for item in valid:
        if item.distance_reliable:
            reliable_segments.setdefault(item.distance_segment_id, []).append(item)

    for segment in reliable_segments.values():
        left = 0
        right = 0
        for item_index, item in enumerate(segment):
            minimum_distance = item.distance_m - config.local_window_radius_m
            maximum_distance = item.distance_m + config.local_window_radius_m
            while left < item_index and segment[left].distance_m < minimum_distance:
                left += 1
            if right < item_index:
                right = item_index
            while right < len(segment) and segment[right].distance_m <= maximum_distance:
                right += 1
            local_left = left
            local_right = right
            if local_right - local_left > config.maximum_local_samples:
                half = config.maximum_local_samples // 2
                local_left = max(
                    left,
                    min(
                        item_index - half,
                        right - config.maximum_local_samples,
                    ),
                )
                local_right = local_left + config.maximum_local_samples
            apply_local(item, segment[local_left:local_right])

    valid_positions = {id(item): index for index, item in enumerate(valid)}
    fallback_radius = max(1, config.fallback_window_radius_samples)
    for item in valid:
        if item.distance_reliable:
            continue
        position = valid_positions[id(item)]
        start = max(0, position - fallback_radius)
        end = min(len(valid), position + fallback_radius + 1)
        apply_local(item, valid[start:end])


def _nearest_flank(
    analyzed: list[AnalyzedClearanceMeasurement],
    start_index: int,
    direction: int,
    center_distance_m: float,
    config: ClearanceAnomalyConfig,
) -> AnalyzedClearanceMeasurement | None:
    index = start_index
    while 0 <= index < len(analyzed):
        item = analyzed[index]
        if abs(item.distance_m - center_distance_m) > config.flank_search_distance_m:
            return None
        if item.valid and not item.low_candidate:
            return item
        index += direction
    return None


def _lateral_coordinate(record: ClearanceMeasurement) -> float | None:
    point = _point(record)
    if point is None:
        return None
    if not _finite(record.vehicle_heading_deg):
        return point[0]
    heading_rad = math.radians(float(record.vehicle_heading_deg))
    return point[0] * math.cos(heading_rad) - point[1] * math.sin(heading_rad)


def _build_event(
    event_number: int,
    member_positions: list[int],
    analyzed: list[AnalyzedClearanceMeasurement],
    config: ClearanceAnomalyConfig,
) -> ClearanceLowEvent:
    members = [analyzed[index] for index in member_positions]
    heights = [float(item.measurement.height_m) for item in members]
    baselines = [item.local_baseline_m for item in members if item.local_baseline_m is not None]
    drops = [item.local_drop_m for item in members if item.local_drop_m is not None]
    min_member = min(members, key=lambda item: float(item.measurement.height_m))
    representative_point = _point(min_member.measurement)
    start_s = min(item.distance_m for item in members)
    end_s = max(item.distance_m for item in members)
    center_s = (start_s + end_s) / 2.0

    before = _nearest_flank(
        analyzed, member_positions[0] - 1, -1, start_s, config
    )
    after = _nearest_flank(
        analyzed, member_positions[-1] + 1, 1, end_s, config
    )
    front_height = float(before.measurement.height_m) if before is not None else None
    back_height = float(after.measurement.height_m) if after is not None else None

    entry_gradient = None
    if before is not None and start_s > before.distance_m:
        entry_gradient = (_median(heights) - front_height) / (start_s - before.distance_m)
    exit_gradient = None
    if after is not None and after.distance_m > end_s:
        exit_gradient = (back_height - _median(heights)) / (after.distance_m - end_s)

    boundary_jumps: list[float] = []
    first_point = _point(members[0].measurement)
    last_point = _point(members[-1].measurement)
    if before is not None and first_point is not None:
        before_point = _point(before.measurement)
        if before_point is not None:
            boundary_jumps.append(_distance(before_point, first_point))
    if after is not None and last_point is not None:
        after_point = _point(after.measurement)
        if after_point is not None:
            boundary_jumps.append(_distance(last_point, after_point))

    world_points: list[tuple[float, float, float]] = []
    for member in members:
        point = _point(member.measurement)
        odometry = _odometry(member.measurement)
        if point is not None and odometry is not None:
            world_points.append(tuple(a + b for a, b in zip(point, odometry, strict=True)))
    world_spread = None
    point_continuous = None
    if len(world_points) >= 2:
        center = tuple(_median(point[axis] for point in world_points) for axis in range(3))
        world_spread = max(_distance(point, center) for point in world_points)
        limit = max(
            config.world_point_spread_m,
            (end_s - start_s) * config.world_point_spread_length_ratio,
        )
        point_continuous = world_spread <= limit

    local_sufficient = all(
        member.local_sample_count >= config.minimum_local_samples
        and member.local_span_m >= config.minimum_local_span_m
        and member.distance_reliable
        for member in members
    )
    return ClearanceLowEvent(
        event_id=f"E{event_number:04d}",
        sample_indices=[member.measurement.sample_index for member in members],
        start_s_m=start_s,
        end_s_m=end_s,
        center_s_m=center_s,
        length_m=end_s - start_s,
        sample_count=len(members),
        min_height_m=min(heights),
        median_height_m=_median(heights),
        local_baseline_m=_median(float(value) for value in baselines),
        max_drop_m=max(float(value) for value in drops),
        representative_point_x_m=representative_point[0] if representative_point else None,
        representative_point_y_m=representative_point[1] if representative_point else None,
        representative_point_z_m=representative_point[2] if representative_point else None,
        representative_lateral_m=_lateral_coordinate(min_member.measurement),
        height_mad_m=_mad(heights),
        front_normal_height_m=front_height,
        back_normal_height_m=back_height,
        entry_gradient_m_per_m=entry_gradient,
        exit_gradient_m_per_m=exit_gradient,
        boundary_point_jump_m=max(boundary_jumps) if boundary_jumps else None,
        world_point_spread_m=world_spread,
        point_trajectory_continuous=point_continuous,
        analysis_sufficient=local_sufficient,
    )


def _cluster_events(
    analyzed: list[AnalyzedClearanceMeasurement], config: ClearanceAnomalyConfig
) -> list[ClearanceLowEvent]:
    candidate_positions = [index for index, item in enumerate(analyzed) if item.low_candidate]
    if not candidate_positions:
        return []
    clusters: list[list[int]] = [[candidate_positions[0]]]
    for position in candidate_positions[1:]:
        previous = analyzed[clusters[-1][-1]]
        current = analyzed[position]
        if current.distance_m - previous.distance_m <= config.event_merge_gap_m:
            clusters[-1].append(position)
        else:
            clusters.append([position])
    return [
        _build_event(index + 1, cluster, analyzed, config)
        for index, cluster in enumerate(clusters)
    ]


def _lengths_similar(first: ClearanceLowEvent, second: ClearanceLowEvent, ratio: float) -> bool:
    if first.length_m <= 1.0e-9 and second.length_m <= 1.0e-9:
        return True
    shorter = min(first.length_m, second.length_m)
    longer = max(first.length_m, second.length_m)
    return shorter > 1.0e-9 and longer / shorter <= ratio


def _events_similar(
    first: ClearanceLowEvent, second: ClearanceLowEvent, config: ClearanceAnomalyConfig
) -> bool:
    if abs(first.min_height_m - second.min_height_m) > config.periodic_height_tolerance_m:
        return False
    if not _lengths_similar(first, second, config.periodic_length_ratio_max):
        return False
    if (
        first.representative_lateral_m is not None
        and second.representative_lateral_m is not None
        and abs(first.representative_lateral_m - second.representative_lateral_m)
        > config.periodic_lateral_tolerance_m
    ):
        return False
    if (
        first.representative_point_z_m is not None
        and second.representative_point_z_m is not None
        and abs(first.representative_point_z_m - second.representative_point_z_m)
        > config.periodic_vertical_tolerance_m
    ):
        return False
    return True


def _protect_periodic_events(
    events: list[ClearanceLowEvent], config: ClearanceAnomalyConfig
) -> bool:
    if len(events) < config.periodic_min_events:
        return True
    if len(events) > config.periodic_max_candidate_events:
        return False
    candidates: list[tuple[int, float, float, list[int]]] = []
    for first_index in range(len(events) - 1):
        for second_index in range(first_index + 1, len(events)):
            ordinal_gap = second_index - first_index
            period = (
                events[second_index].center_s_m - events[first_index].center_s_m
            ) / ordinal_gap
            if period < config.periodic_min_spacing_m:
                continue
            anchor = events[first_index]
            tolerance = max(
                config.periodic_position_tolerance_m,
                config.periodic_position_tolerance_ratio * period,
            )
            matched: list[int] = []
            occupied_ordinals: set[int] = set()
            residual_sum = 0.0
            for event_index, event in enumerate(events):
                if not _events_similar(anchor, event, config):
                    continue
                ordinal = round((event.center_s_m - anchor.center_s_m) / period)
                residual = abs(event.center_s_m - (anchor.center_s_m + ordinal * period))
                if residual <= tolerance and ordinal not in occupied_ordinals:
                    occupied_ordinals.add(ordinal)
                    matched.append(event_index)
                    residual_sum += residual
            if len(matched) >= config.periodic_min_events:
                candidates.append((len(matched), residual_sum, period, matched))

    group_number = 0
    while candidates:
        candidates.sort(key=lambda item: (-item[0], item[1], item[2]))
        _, _, period, matched = candidates.pop(0)
        unprotected = [index for index in matched if not events[index].periodic_protected]
        if len(unprotected) < config.periodic_min_events:
            continue
        group_number += 1
        group_id = f"P{group_number:03d}"
        for index in unprotected:
            events[index].periodic_protected = True
            events[index].periodic_group_id = group_id
            events[index].estimated_period_m = period
    return True


def _score_event(event: ClearanceLowEvent, config: ClearanceAnomalyConfig) -> None:
    score = 0
    rules: list[str] = []

    def apply(condition: bool, weight: int, rule: str) -> None:
        nonlocal score
        if condition:
            score += weight
            rules.append(f"{rule}:{weight:+d}")

    immediate_recovery = (
        event.front_normal_height_m is not None
        and event.back_normal_height_m is not None
        and event.front_normal_height_m
        >= event.local_baseline_m - config.normal_return_tolerance_m
        and event.back_normal_height_m
        >= event.local_baseline_m - config.normal_return_tolerance_m
    )
    height_stable = event.height_mad_m <= config.structure_max_height_mad_m
    meaningful_length = event.length_m >= config.structure_min_length_m
    structure_continuity = event.point_trajectory_continuous is True

    apply(event.sample_count == 1, config.score_isolated, "ISOLATED_MEASUREMENT")
    apply(event.length_m < config.short_event_length_m, config.score_short, "VERY_SHORT_DISTANCE")
    apply(event.max_drop_m >= config.large_drop_m, config.score_large_drop, "LARGE_LOCAL_DROP")
    apply(immediate_recovery, config.score_immediate_recovery, "IMMEDIATE_TWO_SIDED_RECOVERY")
    apply(
        event.boundary_point_jump_m is not None
        and event.boundary_point_jump_m >= config.point_boundary_jump_m,
        config.score_point_jump,
        "LARGE_MINIMUM_POINT_JUMP",
    )
    apply(
        event.point_trajectory_continuous is False,
        config.score_poor_point_continuity,
        "INCONSISTENT_WORLD_POINT_TRAJECTORY",
    )
    apply(event.sample_count >= config.structure_min_samples, config.score_multiple_samples, "MULTIPLE_MEASUREMENTS")
    apply(meaningful_length, config.score_meaningful_length, "MEANINGFUL_DISTANCE")
    apply(height_stable and event.sample_count >= 2, config.score_stable_height, "STABLE_EVENT_HEIGHT")
    apply(structure_continuity, config.score_structure_continuity, "CONTINUOUS_STRUCTURE_POINT")
    apply(event.periodic_protected, config.score_periodic, "PERIODIC_STRUCTURE_GROUP")

    event.anomaly_score = score
    event.matched_rules = rules
    if event.periodic_protected:
        event.status = ClearanceEventStatus.PROTECTED_PERIODIC_STRUCTURE
        return
    single_structure_supported = (
        event.sample_count >= config.structure_min_samples
        and meaningful_length
        and height_stable
        and event.point_trajectory_continuous is not False
    )
    if single_structure_supported:
        event.status = ClearanceEventStatus.VALID_STRUCTURE
        return
    if not event.analysis_sufficient:
        event.status = ClearanceEventStatus.REVIEW_REQUIRED
    elif score >= config.high_confidence_score_threshold:
        event.status = ClearanceEventStatus.HIGH_CONFIDENCE_OUTLIER
    elif score >= config.review_score_threshold:
        event.status = ClearanceEventStatus.REVIEW_REQUIRED
    else:
        event.status = ClearanceEventStatus.VALID_STRUCTURE


def analyze_clearance(
    records: Iterable[ClearanceMeasurement],
    config: ClearanceAnomalyConfig = DEFAULT_CLEARANCE_ANOMALY_CONFIG,
) -> ClearanceAnalysisResult:
    source = list(records)
    analyzed, duplicate_count = _validate_and_calculate_distance(source, config)
    _calculate_local_baselines(analyzed, config)
    events = _cluster_events(analyzed, config)
    periodic_analysis_complete = _protect_periodic_events(events, config)
    for event in events:
        if not periodic_analysis_complete:
            event.analysis_sufficient = False
        _score_event(event, config)

    valid = [item for item in analyzed if item.valid]
    raw_minimum = min((float(item.measurement.height_m) for item in valid), default=None)
    excluded_indices = {
        sample_index
        for event in events
        if event.status == ClearanceEventStatus.HIGH_CONFIDENCE_OUTLIER
        for sample_index in event.sample_indices
    }
    effective_minimum = min(
        (
            float(item.measurement.height_m)
            for item in valid
            if item.measurement.sample_index not in excluded_indices
        ),
        default=None,
    )
    return ClearanceAnalysisResult(
        raw_min_clearance_m=raw_minimum,
        effective_min_clearance_m=effective_minimum,
        valid_record_count=len(valid),
        invalid_record_count=len(analyzed) - len(valid),
        duplicate_source_record_count=duplicate_count,
        low_clearance_events=tuple(events),
    )
