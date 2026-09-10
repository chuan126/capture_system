#!/usr/bin/env python3
"""将 Capture System 正式任务整理为按日期分组的只读派生副本。"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import sqlite3
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Iterable


# 设备显示编号固定按中国标准时间生成；使用固定UTC+8可避免Windows缺少tzdata。
LOCAL_TIMEZONE = timezone(timedelta(hours=8), "Asia/Shanghai")
DISPLAY_ID_PATTERN = re.compile(r"^(\d{8})_(\d{6})(?:_\d+)?$")
WINDOWS_INVALID_PATTERN = re.compile(r'[<>:"/\\|?*\x00-\x1f]')
WINDOWS_RESERVED_NAMES = {
    "CON", "PRN", "AUX", "NUL",
    *(f"COM{index}" for index in range(1, 10)),
    *(f"LPT{index}" for index in range(1, 10)),
}
TASK_CSV_FIELDS = [
    "日期文件夹", "网页显示编号", "任务UUID", "隧道编号", "隧道名称", "任务状态",
    "创建时间", "开始时间", "完成时间", "测量时长秒", "行驶方向", "左右车道",
    "右起车道编号",
    "净空阈值米", "净空上限米", "入口RTK状态", "出口RTK状态", "是否有测量数据",
    "原始记录路径", "整理后相对路径", "数据库字节数", "数据库Schema版本",
    "净空源帧数", "50Hz保持样本数", "IMU样本数", "RTK样本数",
    "有效净空源帧数", "无效净空源帧数", "最低有效净空米", "警告代码",
    "错误代码", "错误信息", "逻辑删除时间", "文件完整性",
]
ANOMALY_FIELDS = ["类型", "任务UUID", "网页显示编号", "原始路径", "说明"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="保留原始runtime不变，按日期和任务名称生成正式任务整理副本",
    )
    parser.add_argument("source_runtime", type=Path, help="包含capture.db和tasks的runtime目录")
    parser.add_argument(
        "output_directory",
        type=Path,
        nargs="?",
        help="输出目录；默认是runtime同级的runtime_整理结果",
    )
    return parser.parse_args()


def sanitize_windows_component(value: object, fallback: str, max_length: int = 120) -> str:
    text = WINDOWS_INVALID_PATTERN.sub("_", str(value or "").strip()).rstrip(" .")
    if not text:
        text = fallback
    if text.upper() in WINDOWS_RESERVED_NAMES:
        text = f"_{text}"
    return text[:max_length].rstrip(" .") or fallback


def readonly_connection(path: Path) -> sqlite3.Connection:
    uri = f"{path.resolve().as_uri()}?mode=ro"
    connection = sqlite3.connect(uri, uri=True, timeout=30.0)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA query_only = ON")
    return connection


def table_exists(connection: sqlite3.Connection, table_name: str) -> bool:
    row = connection.execute(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?", (table_name,)
    ).fetchone()
    return row is not None


def table_columns(connection: sqlite3.Connection, table_name: str) -> set[str]:
    return {str(row[1]) for row in connection.execute(f'PRAGMA table_info("{table_name}")')}


def select_tasks(connection: sqlite3.Connection) -> list[dict[str, Any]]:
    return [dict(row) for row in connection.execute("SELECT * FROM tasks ORDER BY created_at, sequence")]


def select_parameters(connection: sqlite3.Connection) -> dict[str, dict[str, Any]]:
    if not table_exists(connection, "task_parameters"):
        return {}
    return {
        str(row["task_id"]): dict(row)
        for row in connection.execute("SELECT * FROM task_parameters")
    }


def task_date(task: dict[str, Any]) -> str:
    display_id = str(task.get("display_id") or "")
    match = DISPLAY_ID_PATTERN.match(display_id)
    if match:
        return match.group(1)
    created_at = str(task.get("created_at") or "")
    try:
        parsed = datetime.fromisoformat(created_at.replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            parsed = parsed.replace(tzinfo=timezone.utc)
        return parsed.astimezone(LOCAL_TIMEZONE).strftime("%Y%m%d")
    except ValueError:
        return "日期未知"


def display_label(task: dict[str, Any]) -> str:
    display_id = str(task.get("display_id") or "").strip()
    if display_id:
        return display_id
    sequence = task.get("sequence")
    return f"无显示编号_{sequence}" if sequence is not None else "无显示编号"


def duration_seconds(task: dict[str, Any]) -> float | None:
    start = task.get("started_at")
    end = task.get("completed_at")
    if not start or not end:
        return None
    try:
        start_time = datetime.fromisoformat(str(start).replace("Z", "+00:00"))
        end_time = datetime.fromisoformat(str(end).replace("Z", "+00:00"))
        return round(max(0.0, (end_time - start_time).total_seconds()), 3)
    except ValueError:
        return None


def scalar(connection: sqlite3.Connection, sql: str, default: Any = None) -> Any:
    row = connection.execute(sql).fetchone()
    return default if row is None or row[0] is None else row[0]


def measurement_summary(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    summary: dict[str, Any] = {
        "database_bytes": path.stat().st_size,
        "integrity": "未检查",
        "schema_version": None,
        "clearance_source_frames": 0,
        "clearance_samples": 0,
        "imu_samples": 0,
        "rtk_samples": 0,
        "valid_source_frames": 0,
        "invalid_source_frames": 0,
        "minimum_valid_clearance_m": None,
    }
    table_stats: list[dict[str, Any]] = []
    with readonly_connection(path) as connection:
        integrity_rows = [str(row[0]) for row in connection.execute("PRAGMA quick_check")]
        summary["integrity"] = "正常" if integrity_rows == ["ok"] else "; ".join(integrity_rows)
        tables = [
            str(row[0])
            for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
            )
        ]
        for table_name in tables:
            count = int(scalar(connection, f'SELECT COUNT(*) FROM "{table_name}"', 0))
            columns = len(table_columns(connection, table_name))
            table_stats.append({"数据表": table_name, "行数": count, "列数": columns})
            if table_name in summary:
                summary[table_name] = count
        if table_exists(connection, "recording_metadata"):
            columns = table_columns(connection, "recording_metadata")
            if "schema_version" in columns:
                summary["schema_version"] = scalar(
                    connection, "SELECT schema_version FROM recording_metadata WHERE id = 1"
                )
        if table_exists(connection, "clearance_source_frames"):
            columns = table_columns(connection, "clearance_source_frames")
            summary["valid_source_frames"] = int(scalar(
                connection, "SELECT COUNT(*) FROM clearance_source_frames WHERE valid = 1", 0
            ))
            summary["invalid_source_frames"] = int(scalar(
                connection, "SELECT COUNT(*) FROM clearance_source_frames WHERE valid = 0", 0
            ))
            if "lidar_to_top_m" in columns:
                mount_height = 0.0
                if table_exists(connection, "recording_metadata"):
                    metadata_columns = table_columns(connection, "recording_metadata")
                    if "lidar_mount_height_m" in metadata_columns:
                        mount_height = float(scalar(
                            connection,
                            "SELECT COALESCE(lidar_mount_height_m, 0) FROM recording_metadata WHERE id = 1",
                            0.0,
                        ))
                minimum = scalar(
                    connection,
                    "SELECT MIN(lidar_to_top_m) FROM clearance_source_frames "
                    "WHERE valid = 1 AND lidar_to_top_m IS NOT NULL",
                )
                if minimum is not None:
                    summary["minimum_valid_clearance_m"] = round(float(minimum) + mount_height, 6)
    return summary, table_stats


def backup_database(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with readonly_connection(source) as source_connection:
        destination_connection = sqlite3.connect(destination)
        try:
            source_connection.backup(destination_connection)
        finally:
            destination_connection.close()


def copy_optional_directory(source: Path, destination: Path) -> None:
    if source.is_dir():
        shutil.copytree(source, destination, copy_function=shutil.copy2)


def write_csv(path: Path, fields: list[str], rows: Iterable[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8-sig", newline="") as file_object:
        writer = csv.DictWriter(file_object, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def json_dump(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def task_csv_row(
    task: dict[str, Any], parameters: dict[str, Any], date: str, relative_path: str,
    measurement_path: Path | None, summary: dict[str, Any] | None,
) -> dict[str, Any]:
    summary = summary or {}
    return {
        "日期文件夹": date,
        "网页显示编号": task.get("display_id"),
        "任务UUID": task.get("task_id"),
        "隧道编号": task.get("tunnel_code"),
        "隧道名称": task.get("tunnel_name"),
        "任务状态": task.get("status"),
        "创建时间": task.get("created_at"),
        "开始时间": task.get("started_at"),
        "完成时间": task.get("completed_at"),
        "测量时长秒": duration_seconds(task),
        "行驶方向": parameters.get("travel_direction") or task.get("planned_travel_direction"),
        "左右车道": parameters.get("lane_side") or parameters.get("lane") or task.get("planned_lane_side"),
        "右起车道编号": parameters.get("lane_number_from_right") or task.get("planned_lane_number_from_right"),
        "净空阈值米": parameters.get("clearance_threshold_m", task.get("planned_clearance_threshold_m")),
        "净空上限米": parameters.get("clearance_upper_limit_m", task.get("planned_clearance_upper_limit_m")),
        "入口RTK状态": task.get("entry_rtk_status"),
        "出口RTK状态": task.get("exit_rtk_status"),
        "是否有测量数据": "是" if measurement_path else "否",
        "原始记录路径": task.get("recording_path"),
        "整理后相对路径": relative_path,
        "数据库字节数": summary.get("database_bytes"),
        "数据库Schema版本": summary.get("schema_version"),
        "净空源帧数": summary.get("clearance_source_frames"),
        "50Hz保持样本数": summary.get("clearance_samples"),
        "IMU样本数": summary.get("imu_samples"),
        "RTK样本数": summary.get("rtk_samples"),
        "有效净空源帧数": summary.get("valid_source_frames"),
        "无效净空源帧数": summary.get("invalid_source_frames"),
        "最低有效净空米": summary.get("minimum_valid_clearance_m"),
        "警告代码": task.get("warning_code"),
        "错误代码": task.get("last_error_code"),
        "错误信息": task.get("last_error_message"),
        "逻辑删除时间": task.get("deleted_at"),
        "文件完整性": summary.get("integrity", "无数据库"),
    }


def resolve_measurement_path(source_runtime: Path, task: dict[str, Any]) -> Path:
    task_id = str(task["task_id"])
    recording_path = str(task.get("recording_path") or "").strip()
    if recording_path:
        candidate = source_runtime / "tasks" / recording_path
        if candidate.is_file():
            return candidate
    return source_runtime / "tasks" / task_id / "measurements.db"


def organize(source_runtime: Path, output_directory: Path) -> dict[str, int]:
    source_runtime = source_runtime.resolve()
    output_directory = output_directory.resolve()
    capture_database = source_runtime / "capture.db"
    tasks_root = source_runtime / "tasks"
    if not capture_database.is_file() or not tasks_root.is_dir():
        raise RuntimeError("源目录必须包含capture.db和tasks目录")
    if output_directory == source_runtime or source_runtime in output_directory.parents:
        raise RuntimeError("输出目录不能等于或位于原始runtime内部")
    if output_directory.exists():
        raise RuntimeError(f"输出目录已存在，未覆盖：{output_directory}")

    with readonly_connection(capture_database) as connection:
        tasks = select_tasks(connection)
        parameters = select_parameters(connection)

    staging_parent = output_directory.parent
    staging_parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{output_directory.name}.tmp-", dir=staging_parent))
    task_rows: list[dict[str, Any]] = []
    task_documents: list[dict[str, Any]] = []
    anomalies: list[dict[str, Any]] = []
    no_measurement_rows: list[dict[str, Any]] = []
    indexed_task_ids = {str(task["task_id"]) for task in tasks}
    used_relative_paths: set[str] = set()

    try:
        for task in tasks:
            task_id = str(task["task_id"])
            date = task_date(task)
            name = sanitize_windows_component(task.get("tunnel_name"), "未命名任务", 80)
            folder_name = sanitize_windows_component(f"{display_label(task)}_{name}", task_id, 140)
            relative = Path(date) / folder_name
            if str(relative).casefold() in used_relative_paths:
                relative = Path(date) / f"{folder_name}_{task_id[:8]}"
            used_relative_paths.add(str(relative).casefold())
            task_output = staging / relative
            task_output.mkdir(parents=True)

            source_task_directory = tasks_root / task_id
            measurement_candidate = resolve_measurement_path(source_runtime, task)
            measurement_path = measurement_candidate if measurement_candidate.is_file() else None
            summary: dict[str, Any] | None = None
            table_stats: list[dict[str, Any]] = []
            if measurement_path:
                try:
                    summary, table_stats = measurement_summary(measurement_path)
                    backup_database(measurement_path, task_output / "measurements.db")
                    with readonly_connection(task_output / "measurements.db") as copied:
                        copied_check = [str(row[0]) for row in copied.execute("PRAGMA quick_check")]
                    if copied_check != ["ok"]:
                        raise RuntimeError("整理副本quick_check失败：" + "; ".join(copied_check))
                except (OSError, sqlite3.Error, RuntimeError) as error:
                    anomalies.append({
                        "类型": "测量数据库读取失败", "任务UUID": task_id,
                        "网页显示编号": task.get("display_id"), "原始路径": str(measurement_candidate),
                        "说明": str(error),
                    })
                    measurement_path = None
                    summary = {"integrity": f"读取失败：{error}"}
            else:
                anomalies.append({
                    "类型": "缺少测量数据库", "任务UUID": task_id,
                    "网页显示编号": task.get("display_id"), "原始路径": str(measurement_candidate),
                    "说明": "中央任务索引未找到可读取的measurements.db",
                })

            copy_optional_directory(source_task_directory / "analysis", task_output / "analysis")
            copy_optional_directory(source_task_directory / "exports", task_output / "exports")
            row = task_csv_row(
                task, parameters.get(task_id, {}), date, str(relative), measurement_path, summary
            )
            task_rows.append(row)
            if not measurement_path:
                no_measurement_rows.append(row)
            document = {
                "整理说明": "本目录是原始runtime的派生副本；任务身份仍以UUID为准",
                "日期来源": "display_id" if DISPLAY_ID_PATTERN.match(str(task.get("display_id") or "")) else "created_at按Asia/Shanghai推导",
                "原始任务": task,
                "参数快照": parameters.get(task_id),
                "测量数据库原始路径": str(measurement_candidate),
                "测量数据库统计": summary,
            }
            json_dump(task_output / "任务信息.json", document)
            if table_stats:
                write_csv(task_output / "数据表统计.csv", ["数据表", "行数", "列数"], table_stats)
            task_documents.append({**document, "整理后相对路径": str(relative)})

        for directory in sorted(path for path in tasks_root.iterdir() if path.is_dir()):
            task_id = directory.name
            if task_id in indexed_task_ids:
                continue
            orphan_output = staging / "未关联任务" / sanitize_windows_component(task_id, "未知UUID")
            orphan_output.mkdir(parents=True)
            measurement = directory / "measurements.db"
            description = "磁盘任务目录没有对应的capture.db任务记录"
            if measurement.is_file():
                try:
                    summary, table_stats = measurement_summary(measurement)
                    backup_database(measurement, orphan_output / "measurements.db")
                    json_dump(orphan_output / "任务信息.json", {
                        "整理说明": description,
                        "推测任务UUID": task_id,
                        "测量数据库原始路径": str(measurement),
                        "测量数据库统计": summary,
                    })
                    write_csv(orphan_output / "数据表统计.csv", ["数据表", "行数", "列数"], table_stats)
                except (OSError, sqlite3.Error, RuntimeError) as error:
                    description += f"；数据库读取失败：{error}"
            copy_optional_directory(directory / "analysis", orphan_output / "analysis")
            copy_optional_directory(directory / "exports", orphan_output / "exports")
            anomalies.append({
                "类型": "未关联任务目录", "任务UUID": task_id, "网页显示编号": "",
                "原始路径": str(directory), "说明": description,
            })

        write_csv(staging / "任务总表.csv", TASK_CSV_FIELDS, task_rows)
        json_dump(staging / "任务总表.json", task_documents)
        write_csv(staging / "无测量数据任务.csv", TASK_CSV_FIELDS, no_measurement_rows)
        write_csv(staging / "异常与缺失文件.csv", ANOMALY_FIELDS, anomalies)
        json_dump(staging / "整理摘要.json", {
            "源目录": str(source_runtime),
            "生成时间UTC": datetime.now(timezone.utc).isoformat(),
            "任务总数": len(tasks),
            "有测量数据库任务数": sum(1 for row in task_rows if row["是否有测量数据"] == "是"),
            "无测量数据库任务数": len(no_measurement_rows),
            "异常记录数": len(anomalies),
            "原始点云处理": "已按要求跳过runtime/dev-tests/raw-cloud及全部MCAP",
        })
        os.replace(staging, output_directory)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise

    return {
        "tasks": len(tasks),
        "measurements": sum(1 for row in task_rows if row["是否有测量数据"] == "是"),
        "missing": len(no_measurement_rows),
        "anomalies": len(anomalies),
    }


def main() -> int:
    args = parse_args()
    source = args.source_runtime
    output = args.output_directory or source.resolve().parent / "runtime_整理结果"
    try:
        counts = organize(source, output)
    except (OSError, sqlite3.Error, RuntimeError) as error:
        print(f"整理失败：{error}", file=sys.stderr)
        return 1
    print(
        f"整理完成：{output.resolve()}\n"
        f"任务={counts['tasks']}，测量数据库={counts['measurements']}，"
        f"无测量数据库={counts['missing']}，异常记录={counts['anomalies']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
