from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import tempfile
import uuid
from dataclasses import asdict, dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from threading import Lock
from typing import Iterable, TextIO

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4, landscape
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import Paragraph, SimpleDocTemplate, Spacer, Table, TableStyle

from backend.measurements.clearance_anomaly import (
    DEFAULT_CLEARANCE_ANOMALY_CONFIG,
    ClearanceAnalysisResult,
)
from backend.measurements.repository import (
    MeasurementNotFoundError,
    MeasurementRepository,
    MeasurementStorageError,
    MeasurementSummaryRecord,
    NormalHeightStatisticsRecord,
    RtkEndpointRecord,
)
from backend.tasks.repository import TaskRecord, TaskRepository


class ExportBlockedError(RuntimeError):
    """任务或记录不满足正式导出条件。"""


class ExportStorageError(RuntimeError):
    """导出目录或文件不可用。"""


class ExportNotFoundError(LookupError):
    """请求下载的导出文件不存在。"""


@dataclass(frozen=True)
class TaskExportAssessment:
    task: TaskRecord
    exportable: bool
    blocked_reason: str | None
    summary: MeasurementSummaryRecord | None
    pdf_exportable: bool = False
    pdf_blocked_reason: str | None = None
    normal_height_statistics: NormalHeightStatisticsRecord | None = None
    clearance_analysis: ClearanceAnalysisResult | None = None


@dataclass(frozen=True)
class GeneratedExport:
    export_format: str
    path: Path
    generated_at: str
    report_id: str | None = None
    task_id: str | None = None
    included_task_count: int | None = None
    batch_id: str | None = None
    batch_code: str | None = None


_CHINA_TIMEZONE = timezone(timedelta(hours=8))
_SAFE_FILE_COMPONENT = re.compile(r"[^0-9A-Za-z._-]+")
_PDF_FONT_NAME = "CaptureSystemCJK"
_PDF_FONT_LOCK = Lock()
_TXT_FIELD_SEPARATOR = "    "
_DEFAULT_PDF_FONT_CANDIDATES = (
    Path("/usr/share/fonts/truetype/arphic-gbsn00lp/gbsn00lp.ttf"),
    Path("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"),
    Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
)


class ReportExportService:
    def __init__(
        self,
        data_root: Path,
        task_repository: TaskRepository,
        measurement_repository: MeasurementRepository,
        *,
        pdf_font_path: Path | None = None,
    ) -> None:
        self.data_root = data_root.resolve()
        self.tasks_directory = (self.data_root / "tasks").resolve()
        self.reports_directory = (self.data_root / "reports").resolve()
        self.task_repository = task_repository
        self.measurement_repository = measurement_repository
        self.pdf_font_path = pdf_font_path.resolve() if pdf_font_path else None

    def preview_tasks(self, task_ids: list[str]) -> list[TaskExportAssessment]:
        if not task_ids:
            raise ExportBlockedError("请至少选择一个任务")
        assessments: list[TaskExportAssessment] = []
        seen: set[str] = set()
        for task_id in task_ids:
            if task_id in seen:
                continue
            seen.add(task_id)
            assessments.append(self.assess_task(self.task_repository.get_task(task_id)))
        assessments.sort(key=lambda item: (item.task.global_sequence, item.task.display_id))
        return assessments

    def assess_task(self, task: TaskRecord) -> TaskExportAssessment:
        summary: MeasurementSummaryRecord | None = None
        if not task.has_measurements:
            return TaskExportAssessment(task, False, "任务尚无测量记录", None)
        try:
            summary = self.measurement_repository.load_summary(task)
        except MeasurementNotFoundError:
            return TaskExportAssessment(task, False, "任务尚无测量记录", None)
        except MeasurementStorageError as error:
            return TaskExportAssessment(task, False, f"测量记录不可读取：{error}", None)

        if task.status != "completed":
            if task.status in {"running", "paused"}:
                reason = "任务仍处于采集或暂停状态"
            elif task.status == "interrupted":
                reason = "任务异常中断，不能生成正式报告"
            elif task.status == "failed":
                reason = "任务执行失败，不能生成正式报告"
            else:
                reason = "任务尚未完成"
            return TaskExportAssessment(task, False, reason, summary)
        if summary.data_origin != "recorded":
            return TaskExportAssessment(task, False, "界面测试数据不能用于正式导出", summary)
        if not summary.complete:
            return TaskExportAssessment(task, False, "测量记录未完整结束", summary)
        if summary.statistics.valid_samples <= 0 or summary.statistics.minimum_height_m is None:
            return TaskExportAssessment(task, False, "测量记录中没有有效净空样本", summary)
        try:
            clearance_analysis = self.measurement_repository.load_clearance_analysis(task)
        except MeasurementStorageError as error:
            return TaskExportAssessment(
                task, False, f"净空异常分析不可读取：{error}", summary
            )
        if clearance_analysis.effective_min_clearance_m is None:
            return TaskExportAssessment(
                task, False, "测量记录中没有可用于报告的有效净空样本", summary,
                clearance_analysis=clearance_analysis,
            )
        try:
            normal_statistics = self.measurement_repository.load_normal_height_statistics(task)
        except MeasurementStorageError as error:
            normal_statistics = None
        return TaskExportAssessment(
            task,
            True,
            None,
            summary,
            True,
            None,
            normal_statistics,
            clearance_analysis,
        )

    def generate_txt(self, task: TaskRecord) -> GeneratedExport:
        assessment = self.assess_task(task)
        if not assessment.exportable or assessment.summary is None:
            raise ExportBlockedError(assessment.blocked_reason or "当前任务不能导出 TXT")
        summary = assessment.summary
        analysis = assessment.clearance_analysis
        if analysis is None:
            raise ExportBlockedError("当前任务缺少净空异常分析结果")
        output_directory = self._task_export_directory(task)
        try:
            output_directory.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            raise ExportStorageError(f"无法创建 TXT 导出目录：{error}") from error
        safe_code = _safe_component(task.tunnel_code)
        file_name = f"{task.display_id}_{safe_code}_50Hz测量明细.txt"
        destination = output_directory / file_name
        generated_at = _utc_now_text()
        temporary_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8-sig",
                newline="",
                dir=output_directory,
                prefix=".txt-export-",
                suffix=".tmp",
                delete=False,
            ) as temporary:
                temporary_path = Path(temporary.name)
                self._write_txt(temporary, task, summary, analysis, generated_at)
                temporary.flush()
                os.fsync(temporary.fileno())
            os.replace(temporary_path, destination)
            trace = analysis.to_trace_dict()
            trace["config"] = asdict(DEFAULT_CLEARANCE_ANOMALY_CONFIG)
            destination.with_name(f"{destination.stem}_异常分析.json").write_text(
                json.dumps(trace, ensure_ascii=False, indent=2), encoding="utf-8"
            )
        except (OSError, MeasurementStorageError, ValueError) as error:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)
            raise ExportStorageError(f"生成 TXT 失败：{error}") from error
        return GeneratedExport(
            export_format="txt",
            path=destination,
            generated_at=generated_at,
            task_id=task.task_id,

        )

    def generate_pdf(self, task_ids: list[str]) -> GeneratedExport:
        assessments = self.preview_tasks(task_ids)
        eligible = [assessment for assessment in assessments if assessment.pdf_exportable]
        if not eligible:
            raise ExportBlockedError("所选任务中没有满足正式 PDF 汇总条件的记录")
        report_id = str(uuid.uuid4())
        report_directory = (self.reports_directory / report_id).resolve()
        try:
            report_directory.mkdir(parents=True, exist_ok=False)
        except OSError as error:
            raise ExportStorageError(f"无法创建 PDF 报告目录：{error}") from error
        generated_at = _utc_now_text()
        local_generated = datetime.fromisoformat(generated_at.replace("Z", "+00:00")).astimezone(_CHINA_TIMEZONE)
        file_name = f"{local_generated.strftime('%Y%m%d_%H%M%S')}_隧道净空检测汇总报告.pdf"
        destination = report_directory / file_name
        temporary_path = report_directory / ".report.tmp.pdf"
        try:
            font_name = self._register_pdf_font()
            self._write_pdf(temporary_path, eligible, report_id, generated_at, font_name)
            os.replace(temporary_path, destination)
            report_sha256 = _sha256_file(destination)
            manifest = {
                "report_id": report_id,
                "generated_at": generated_at,
                "file_name": destination.name,
                "sha256": report_sha256,
                "task_ids": [assessment.task.task_id for assessment in eligible],
                "task_display_ids": [assessment.task.display_id for assessment in eligible],
                "clearance_analysis": [
                    {
                        "task_id": assessment.task.task_id,
                        **assessment.clearance_analysis.to_trace_dict(),
                    }
                    for assessment in eligible
                    if assessment.clearance_analysis is not None
                ],
                "clearance_anomaly_config": asdict(DEFAULT_CLEARANCE_ANOMALY_CONFIG),
            }
            (report_directory / "manifest.json").write_text(
                json.dumps(manifest, ensure_ascii=False, indent=2),
                encoding="utf-8",
            )
        except Exception as error:
            shutil.rmtree(report_directory, ignore_errors=True)
            if isinstance(error, (ExportStorageError, ExportBlockedError)):
                raise
            raise ExportStorageError(f"生成 PDF 失败：{error}") from error
        return GeneratedExport(
            export_format="pdf",
            path=destination,
            generated_at=generated_at,
            report_id=report_id,
            included_task_count=len(eligible),
        )

    def resolve_txt_download(self, task: TaskRecord) -> Path:
        output_directory = self._task_export_directory(task)
        safe_code = _safe_component(task.tunnel_code)
        candidate = output_directory / f"{task.display_id}_{safe_code}_50Hz测量明细.txt"
        if not candidate.is_file():
            raise ExportNotFoundError("TXT 尚未生成")
        return candidate

    def resolve_pdf_download(self, report_id: str) -> Path:
        try:
            normalized_id = str(uuid.UUID(report_id))
        except ValueError as error:
            raise ExportNotFoundError("PDF 报告不存在") from error
        report_directory = (self.reports_directory / normalized_id).resolve()
        try:
            report_directory.relative_to(self.reports_directory)
        except ValueError as error:
            raise ExportNotFoundError("PDF 报告不存在") from error
        manifest_path = report_directory / "manifest.json"
        if not manifest_path.is_file():
            raise ExportNotFoundError("PDF 报告不存在")
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            file_name = str(manifest["file_name"])
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise ExportStorageError(f"PDF 报告索引不可读取：{error}") from error
        candidate = (report_directory / file_name).resolve()
        try:
            candidate.relative_to(report_directory)
        except ValueError as error:
            raise ExportNotFoundError("PDF 报告不存在") from error
        if not candidate.is_file():
            raise ExportNotFoundError("PDF 报告不存在")
        return candidate

    def _task_export_directory(self, task: TaskRecord) -> Path:
        directory = (self.tasks_directory / task.task_id / "exports").resolve()
        try:
            directory.relative_to(self.tasks_directory)
        except ValueError as error:
            raise ExportStorageError("任务导出目录超出数据根目录") from error
        return directory

    def _write_txt(
        self,
        file_object: TextIO,
        task: TaskRecord,
        summary: MeasurementSummaryRecord,
        analysis: ClearanceAnalysisResult,
        generated_at: str,
    ) -> None:
        entry_rtk = _format_rtk(summary.entry_rtk)
        exit_rtk = _format_rtk(summary.exit_rtk)
        _write_txt_row(file_object, ["隧道净空检测 50 Hz 测量明细"])
        _write_txt_row(file_object, [])
        _write_txt_row(
            file_object,
            [
                "采样序号",
                "源帧序号",
                "源帧时间",
                "记录时间",
                "源帧年龄 ms",
                "重复源帧",
                "重复序号",
                "隧道编号",
                "检测车道",
                "净空有效",
                "无效原因",
                "实时高度 m",
                "最低高度 m",
                "质量分数",
                "隧道入口 RTK",
                "隧道出口 RTK",
                "RTK时间",
                "RTK纬度 deg",
                "RTK经度 deg",
                "RTK高程 m",
                "RTK解类型",
                "RTK有效",
                "RTK卫星数",
                "RTK HDOP",
                "RTK PDOP",
                "RTK速度 knot",
                "RTK航向 deg",
                "陀螺X rad/s",
                "陀螺Y rad/s",
                "陀螺Z rad/s",
                "加速度计X m/s2",
                "加速度计Y m/s2",
                "加速度计Z m/s2",
                "IMU样本数",
                "雷达温度 °C",
                "最低点云X m",
                "最低点云Y m",
                "最低点云Z m",
                "俯仰 deg",
                "横滚 deg",
                "方位 deg",
                "里程计位置x m",
                "里程计位置y m",
                "里程计位置z m",
                "里程计四元数x",
                "里程计四元数y",
                "里程计四元数z",
                "里程计四元数w",
            ]
        )
        minimum_height = _format_number(analysis.effective_min_clearance_m, 3)
        for sample in self.measurement_repository.iter_export_samples(task):
            _write_txt_row(
                file_object,
                [
                    sample.sample_index,
                    sample.source_sequence if sample.source_sequence is not None else "",
                    _format_timestamp_ms(sample.source_timestamp_ms),
                    _format_timestamp_ms(sample.recorded_timestamp_ms),
                    _format_txt_number(sample.source_age_ms, 3),
                    _format_optional_bool(sample.is_repeated),
                    sample.repeat_index if sample.repeat_index is not None else "",
                    task.tunnel_code,
                    _lane_text(summary.lane, summary.travel_direction, summary.lane_side),
                    "1" if sample.valid else "0",
                    sample.invalid_reason or "",
                    _format_txt_number(sample.height_m if sample.valid else None, 3),
                    minimum_height,
                    _format_txt_number(sample.quality_score, 6),
                    entry_rtk,
                    exit_rtk,
                    _format_timestamp_ms(sample.rtk_timestamp_ms) if sample.rtk_timestamp_ms is not None else "",
                    _format_txt_number(sample.rtk_latitude_deg, 9),
                    _format_txt_number(sample.rtk_longitude_deg, 9),
                    _format_txt_number(sample.rtk_altitude_m, 4),
                    sample.rtk_fix_type or "",
                    _format_optional_bool(sample.rtk_valid),
                    sample.rtk_satellite_count if sample.rtk_satellite_count is not None else "",
                    _format_txt_number(sample.rtk_hdop, 3),
                    _format_txt_number(sample.rtk_pdop, 3),
                    _format_txt_number(sample.rtk_speed_knots, 4),
                    _format_txt_number(sample.rtk_track_degrees, 4),
                    _format_txt_number(sample.gyro_x_rad_s, 6),
                    _format_txt_number(sample.gyro_y_rad_s, 6),
                    _format_txt_number(sample.gyro_z_rad_s, 6),
                    _format_txt_number(sample.accel_x_m_s2, 6),
                    _format_txt_number(sample.accel_y_m_s2, 6),
                    _format_txt_number(sample.accel_z_m_s2, 6),
                    sample.imu_sample_count if sample.imu_sample_count is not None else "",
                    _format_txt_number(sample.radar_temperature_c, 2),
                    _format_txt_number(sample.minimum_point_x_m, 4),
                    _format_txt_number(sample.minimum_point_y_m, 4),
                    _format_txt_number(sample.minimum_point_z_m, 4),
                    _format_txt_number(sample.vehicle_pitch_deg, 4),
                    _format_txt_number(sample.vehicle_roll_deg, 4),
                    _format_txt_number(sample.vehicle_heading_deg, 4),
                    _format_txt_number(sample.odin_position_x_m, 4),
                    _format_txt_number(sample.odin_position_y_m, 4),
                    _format_txt_number(sample.odin_position_z_m, 4),
                    _format_txt_number(sample.odin_qx, 8),
                    _format_txt_number(sample.odin_qy, 8),
                    _format_txt_number(sample.odin_qz, 8),
                    _format_txt_number(sample.odin_qw, 8),
                ]
            )

    def _register_pdf_font(self) -> str:
        with _PDF_FONT_LOCK:
            if _PDF_FONT_NAME in pdfmetrics.getRegisteredFontNames():
                return _PDF_FONT_NAME
            candidates: Iterable[Path]
            if self.pdf_font_path is not None:
                candidates = (self.pdf_font_path,)
            else:
                configured = os.getenv("CAPTURE_PDF_FONT_PATH")
                candidates = (
                    (Path(configured).expanduser().resolve(),)
                    if configured
                    else _DEFAULT_PDF_FONT_CANDIDATES
                )
            errors: list[str] = []
            for candidate in candidates:
                if not candidate.is_file():
                    errors.append(f"{candidate} 不存在")
                    continue
                try:
                    pdfmetrics.registerFont(TTFont(_PDF_FONT_NAME, str(candidate)))
                    return _PDF_FONT_NAME
                except Exception as error:
                    errors.append(f"{candidate} 无法加载：{error}")
            detail = "；".join(errors) or "未配置可用中文字体"
            raise ExportStorageError(
                "PDF 中文字体不可用。请设置 CAPTURE_PDF_FONT_PATH 指向可读取的 TTF/TTC 字体。"
                f" 当前检查结果：{detail}"
            )

    @staticmethod
    def _write_pdf(
        destination: Path,
        eligible: list[TaskExportAssessment],
        report_id: str,
        generated_at: str,
        font_name: str,
    ) -> None:
        page_size = landscape(A4)
        document = SimpleDocTemplate(
            str(destination),
            pagesize=page_size,
            leftMargin=14 * mm,
            rightMargin=14 * mm,
            topMargin=15 * mm,
            bottomMargin=15 * mm,
            title="隧道净空检测汇总报告",
            author="车载激光雷达隧道净空测量系统",
        )
        styles = getSampleStyleSheet()
        title_style = ParagraphStyle(
            "CaptureTitle",
            parent=styles["Title"],
            fontName=font_name,
            fontSize=18,
            leading=23,
            alignment=TA_CENTER,
            spaceAfter=8 * mm,
        )
        body_style = ParagraphStyle(
            "CaptureBody",
            parent=styles["BodyText"],
            fontName=font_name,
            fontSize=8,
            leading=11,
        )
        heading_style = ParagraphStyle(
            "CaptureHeading",
            parent=body_style,
            fontSize=10,
            leading=14,
            spaceBefore=4 * mm,
            spaceAfter=3 * mm,
        )
        story = [
            Paragraph("隧道净空检测汇总报告", title_style),
            Paragraph(f"报告编号　{report_id}", body_style),
            Paragraph(f"生成时间　{_format_iso_text(generated_at)}", body_style),
            Paragraph(f"汇总任务数　{len(eligible)}", body_style),
            Spacer(1, 5 * mm),
            Paragraph("任务汇总", heading_style),
        ]
        headers = [
            "任务序号",
            "隧道编号",
            "检测车道",
            "最低高度 m",
            "记录时间",
            "隧道入口 RTK",
            "隧道出口 RTK",
        ]
        rows: list[list[Paragraph]] = [
            [Paragraph(header, body_style) for header in headers]
        ]
        for assessment in eligible:
            summary = assessment.summary
            if summary is None:
                continue
            analysis = assessment.clearance_analysis
            if analysis is None or analysis.effective_min_clearance_m is None:
                continue
            time_text = f"{_format_iso_text(summary.started_at)}<br/>{_format_iso_text(summary.ended_at)}"
            rows.append(
                [
                    Paragraph(assessment.task.display_id, body_style),
                    Paragraph(_escape_pdf_text(assessment.task.tunnel_code), body_style),
                    Paragraph(_lane_text(summary.lane, summary.travel_direction, summary.lane_side), body_style),
                    Paragraph(_format_number(analysis.effective_min_clearance_m, 3), body_style),
                    Paragraph(time_text, body_style),
                    Paragraph(_escape_pdf_text(_format_rtk(summary.entry_rtk)), body_style),
                    Paragraph(_escape_pdf_text(_format_rtk(summary.exit_rtk)), body_style),
                ]
            )
        table = Table(
            rows,
            repeatRows=1,
            colWidths=[36 * mm, 31 * mm, 22 * mm, 25 * mm, 45 * mm, 44 * mm, 44 * mm],
        )
        table.setStyle(
            TableStyle(
                [
                    ("FONTNAME", (0, 0), (-1, -1), font_name),
                    ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#E8EDF3")),
                    ("TEXTCOLOR", (0, 0), (-1, 0), colors.HexColor("#202833")),
                    ("ALIGN", (0, 0), (0, -1), "CENTER"),
                    ("ALIGN", (2, 1), (3, -1), "CENTER"),
                    ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
                    ("GRID", (0, 0), (-1, -1), 0.35, colors.HexColor("#8D98A6")),
                    ("LEFTPADDING", (0, 0), (-1, -1), 4),
                    ("RIGHTPADDING", (0, 0), (-1, -1), 4),
                    ("TOPPADDING", (0, 0), (-1, -1), 5),
                    ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
                ]
            )
        )
        story.append(table)
        story.extend(
            [
                Spacer(1, 5 * mm),
                Paragraph(
                    "说明　最低高度为排除高置信度偶发异常后的有效最低净空；待复核低值仍保守计入。"
                    "周期性设施和具有空间连续性的单个结构不会因高度低而自动排除。"
                    "无有效 RTK 端点时对应字段标记为未记录。",
                    body_style,
                ),
            ]
        )

        def draw_footer(canvas: object, doc: object) -> None:
            canvas.saveState()
            canvas.setFont(font_name, 8)
            canvas.drawRightString(page_size[0] - 14 * mm, 8 * mm, f"第 {doc.page} 页")
            canvas.restoreState()

        document.build(story, onFirstPage=draw_footer, onLaterPages=draw_footer)


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_component(value: str) -> str:
    normalized = _SAFE_FILE_COMPONENT.sub("_", value.strip()).strip("._")
    return normalized[:80] or "tunnel"


def _lane_text(lane: str, travel_direction: str = "unknown", lane_side: str | None = None) -> str:
    side = lane_side if lane_side in {"left", "right"} else lane
    if travel_direction == "up" and side == "left":
        return "上行左车道"
    if travel_direction == "up" and side == "right":
        return "上行右车道"
    if travel_direction == "down" and side == "left":
        return "下行左车道"
    if travel_direction == "down" and side == "right":
        return "下行右车道"
    if side == "left":
        return "左车道"
    if side == "right":
        return "右车道"
    return "未记录"


def _format_rtk(endpoint: RtkEndpointRecord | None) -> str:
    if endpoint is None or not endpoint.valid:
        return "未记录"
    altitude = f" H{endpoint.altitude_m:.3f}m" if endpoint.altitude_m is not None else ""
    return f"N{endpoint.latitude_deg:.7f} E{endpoint.longitude_deg:.7f}{altitude}"


def _write_txt_row(file_object: TextIO, fields: Iterable[object]) -> None:
    sanitized = [" ".join(str(field).split()) for field in fields]
    file_object.write(_TXT_FIELD_SEPARATOR.join(sanitized) + "\r\n")


def _format_number(value: float | None, digits: int) -> str:
    return "" if value is None else f"{value:.{digits}f}"


def _format_txt_number(value: float | None, digits: int) -> str:
    # TXT 的 0 只承担缺失占位语义，SQLite 中的 NULL 和有效标志保持不变。
    return "0" if value is None else f"{value:.{digits}f}"


def _format_optional_bool(value: bool | None) -> str:
    if value is None:
        return ""
    return "1" if value else "0"


def _parse_datetime(value: str) -> datetime:
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    # ROS 2/C++ timestamps can contain nanosecond precision. Python datetime stores
    # microseconds, so retain the first six fractional digits for report display.
    normalized = re.sub(r"(\.\d{6})\d+(?=(?:[+-]\d{2}:\d{2})?$)", r"\1", normalized)
    parsed = datetime.fromisoformat(normalized)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed


def _format_iso_text(value: str | None) -> str:
    if not value:
        return "未记录"
    return _parse_datetime(value).astimezone(_CHINA_TIMEZONE).isoformat(timespec="milliseconds")


def _format_timestamp_ms(timestamp_ms: int) -> str:
    value = datetime.fromtimestamp(timestamp_ms / 1000.0, timezone.utc)
    return value.astimezone(_CHINA_TIMEZONE).isoformat(timespec="milliseconds")


def _escape_pdf_text(value: str) -> str:
    return value.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _utc_now_text() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
