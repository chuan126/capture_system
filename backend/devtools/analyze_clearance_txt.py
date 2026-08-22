from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

from backend.measurements.clearance_anomaly import ClearanceMeasurement, analyze_clearance


def _number(value: str) -> float | None:
    try:
        parsed = float(value.strip())
    except ValueError:
        return None
    return parsed if math.isfinite(parsed) else None


def load_txt_measurements(path: Path) -> list[ClearanceMeasurement]:
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    header_index = next(
        index for index, line in enumerate(lines) if "采样序号" in line and "实时高度 m" in line
    )
    separator = "\t" if "\t" in lines[header_index] else "    "
    headers = lines[header_index].split(separator)
    positions = {name.strip(): index for index, name in enumerate(headers)}

    def field(values: list[str], name: str) -> str:
        index = positions.get(name)
        return values[index] if index is not None and index < len(values) else ""

    records: list[ClearanceMeasurement] = []
    for line in lines[header_index + 1 :]:
        if not line.strip():
            continue
        values = line.split(separator)
        if len(values) != len(headers):
            continue
        sample_index_value = _number(field(values, "采样序号"))
        if sample_index_value is None:
            continue
        height = _number(field(values, "实时高度 m"))
        point_x = _number(field(values, "最低点云X m"))
        point_y = _number(field(values, "最低点云Y m"))
        point_z = _number(field(values, "最低点云Z m"))
        records.append(
            ClearanceMeasurement(
                sample_index=int(sample_index_value),
                height_m=height,
                valid=height is not None and height > 0.0,
                minimum_point_x_m=point_x,
                minimum_point_y_m=point_y,
                minimum_point_z_m=point_z,
                vehicle_heading_deg=_number(field(values, "方位 deg")),
                odin_position_x_m=_number(field(values, "里程计位置x m")),
                odin_position_y_m=_number(field(values, "里程计位置y m")),
                odin_position_z_m=_number(field(values, "里程计位置z m")),
            )
        )
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description="分析隧道净空TXT中的距离域低值事件")
    parser.add_argument("txt", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    analysis = analyze_clearance(load_txt_measurements(args.txt))
    payload = analysis.to_trace_dict()
    text = json.dumps(payload, ensure_ascii=False, indent=2)
    if args.output is not None:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
