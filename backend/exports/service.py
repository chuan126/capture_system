from __future__ import annotations

import csv
import json
import os
import re
import shutil
import tempfile
import uuid
from dataclasses import dataclass
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

from backend.measurements.repository import (
    MeasurementNotFoundError,
    MeasurementRepository,
    MeasurementStorageError,
    MeasurementSummaryRecord,
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


@dataclass(frozen=True)
class GeneratedExport:
    export_format: str
    path: Path
    generated_at: str
    report_id: str | None = None
    task_id: str | None = None
    included_task_count: int | None = None


_CHINA_TIMEZONE = timezone(timedelta(hours=8))
_SAFE_FILE_COMPONENT = re.compile(r"[^0-9A-Za-z._-]+")
_REPORT_FILE_NAME = "隧道净空检测汇总报告.pdf"
_PDF_FONT_NAME = "CaptureSystemCJK"
_PDF_FONT_LOCK = Lock()
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

    def preview(self) -> list[TaskExportAssessment]:
        assessments: list[TaskExportAssessment] = []
        offset = 0
        while True:
            batch = self.task_repository.list_tasks(limit=500, offset=offset, order="asc")
            assessments.extend(self.assess_task(task) for task in batch)
            if len(batch) < 500:
                break
            offset += len(batch)
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
        return TaskExportAssessment(task, True, None, summary)

    def generate_txt(self, task: TaskRecord) -> GeneratedExport:
        assessment = self.assess_task(task)
        if not assessment.exportable or assessment.summary is None:
            raise ExportBlockedError(assessment.blocked_reason or "当前任务不能导出 TXT")
        summary = assessment.summary
        output_directory = self._task_export_directory(task)
        try:
            output_directory.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            raise ExportStorageError(f"无法创建 TXT 导出目录：{error}") from error
        safe_code = _safe_component(task.tunnel_code)
        file_name = f"任务{task.display_sequence}_{safe_code}_50Hz测量明细.txt"
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
                self._write_txt(temporary, task, summary, generated_at)
                temporary.flush()
                os.fsync(temporary.fileno())
            os.replace(temporary_path, destination)
        except (OSError, csv.Error, MeasurementStorageError) as error:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)
            raise ExportStorageError(f"生成 TXT 失败：{error}") from error
        return GeneratedExport(
            export_format="txt",
            path=destination,
            generated_at=generated_at,
            task_id=task.task_id,
        )

    def generate_pdf(self) -> GeneratedExport:
        eligible = [assessment for assessment in self.preview() if assessment.exportable]
        if not eligible:
            raise ExportBlockedError("没有满足正式 PDF 汇总条件的任务")
        report_id = str(uuid.uuid4())
        report_directory = (self.reports_directory / report_id).resolve()
        try:
            report_directory.mkdir(parents=True, exist_ok=False)
        except OSError as error:
            raise ExportStorageError(f"无法创建 PDF 报告目录：{error}") from error
        destination = report_directory / _REPORT_FILE_NAME
        generated_at = _utc_now_text()
        temporary_path = report_directory / ".report.tmp.pdf"
        try:
            font_name = self._register_pdf_font()
            self._write_pdf(temporary_path, eligible, report_id, generated_at, font_name)
            os.replace(temporary_path, destination)
            manifest = {
                "report_id": report_id,
                "generated_at": generated_at,
                "file_name": destination.name,
                "task_ids": [assessment.task.task_id for assessment in eligible],
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
        candidate = output_directory / f"任务{task.display_sequence}_{safe_code}_50Hz测量明细.txt"
        if not candidate.is_file():
            raise ExportNotFoundError("TXT 尚未生成")
        return candidate

    def resolve_pdf_download(self, report_id: str) -> Path:
        try:
            normalized_id = str(uuid.UUID(report_id))
        except ValueError as error:
            raise ExportNotFoundError("PDF 报告不存在") from error
        candidate = (self.reports_directory / normalized_id / _REPORT_FILE_NAME).resolve()
        try:
            candidate.relative_to(self.reports_directory)
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
        generated_at: str,
    ) -> None:
        writer = csv.writer(file_object, delimiter="\t", lineterminator="\r\n")
        entry_rtk = _format_rtk(summary.entry_rtk)
        exit_rtk = _format_rtk(summary.exit_rtk)
        writer.writerow(["隧道净空检测 50 Hz 测量明细"])
        writer.writerow(["任务序号", task.display_sequence])
        writer.writerow(["隧道编号", task.tunnel_code])
        writer.writerow(["隧道名称", task.tunnel_name])
        writer.writerow(["检测车道", _lane_text(summary.lane)])
        writer.writerow(["记录开始时间", _format_iso_text(summary.started_at)])
        writer.writerow(["记录结束时间", _format_iso_text(summary.ended_at)])
        writer.writerow(["最低有效高度 m", _format_number(summary.statistics.minimum_height_m, 3)])
        writer.writerow(["入口 RTK", entry_rtk])
        writer.writerow(["出口 RTK", exit_rtk])
        writer.writerow(["总样本数", summary.statistics.total_samples])
        writer.writerow(["有效样本数", summary.statistics.valid_samples])
        writer.writerow(["无效样本数", summary.statistics.invalid_samples])
        writer.writerow(["标称采样频率 Hz", _format_number(summary.statistics.nominal_sample_rate_hz, 3)])
        writer.writerow(["实际平均频率 Hz", _format_number(summary.statistics.actual_average_sample_rate_hz, 3)])
        writer.writerow(["算法版本", summary.algorithm_version or ""])
        writer.writerow(["配置版本", summary.config_version or ""])
        writer.writerow(["软件版本", summary.software_version or ""])
        writer.writerow(["文件生成时间", _format_iso_text(generated_at)])
        writer.writerow([])
        writer.writerow(
            [
                "采样序号",
                "数据源时间",
                "记录时间",
                "相对时间 ms",
                "隧道编号",
                "检测车道",
                "实时高度 m",
                "最低高度 m",
                "数据有效",
                "无效原因",
                "质量分数",
                "隧道入口 RTK",
                "隧道出口 RTK",
            ]
        )
        minimum_height = _format_number(summary.statistics.minimum_height_m, 3)
        for sample in self.measurement_repository.iter_export_samples(task):
            writer.writerow(
                [
                    sample.sample_index,
                    _format_timestamp_ms(sample.source_timestamp_ms),
                    _format_timestamp_ms(sample.recorded_timestamp_ms),
                    f"{sample.elapsed_ms:.3f}",
                    task.tunnel_code,
                    _lane_text(summary.lane),
                    _format_number(sample.height_m if sample.valid else None, 3),
                    minimum_height,
                    "是" if sample.valid and sample.height_m is not None else "否",
                    sample.invalid_reason or "",
                    _format_number(sample.quality_score, 4),
                    entry_rtk,
                    exit_rtk,
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
            time_text = f"{_format_iso_text(summary.started_at)}<br/>{_format_iso_text(summary.ended_at)}"
            rows.append(
                [
                    Paragraph(assessment.task.display_sequence, body_style),
                    Paragraph(_escape_pdf_text(assessment.task.tunnel_code), body_style),
                    Paragraph(_lane_text(summary.lane), body_style),
                    Paragraph(_format_number(summary.statistics.minimum_height_m, 3), body_style),
                    Paragraph(time_text, body_style),
                    Paragraph(_escape_pdf_text(_format_rtk(summary.entry_rtk)), body_style),
                    Paragraph(_escape_pdf_text(_format_rtk(summary.exit_rtk)), body_style),
                ]
            )
        table = Table(
            rows,
            repeatRows=1,
            colWidths=[20 * mm, 34 * mm, 24 * mm, 27 * mm, 49 * mm, 47 * mm, 47 * mm],
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
                    "说明　无有效 RTK 端点时对应字段留空并标记为未记录。"
                    "本报告仅汇总数据来源为正式记录、任务正常完成且包含有效净空样本的任务。",
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


def _safe_component(value: str) -> str:
    normalized = _SAFE_FILE_COMPONENT.sub("_", value.strip()).strip("._")
    return normalized[:80] or "tunnel"


def _lane_text(lane: str) -> str:
    if lane == "left":
        return "左车道"
    if lane == "right":
        return "右车道"
    return "未记录"


def _format_rtk(endpoint: RtkEndpointRecord | None) -> str:
    if endpoint is None or not endpoint.valid:
        return "未记录"
    altitude = f", {endpoint.altitude_m:.3f} m" if endpoint.altitude_m is not None else ""
    return f"{endpoint.latitude_deg:.7f}, {endpoint.longitude_deg:.7f}{altitude}"


def _format_number(value: float | None, digits: int) -> str:
    return "" if value is None else f"{value:.{digits}f}"


def _parse_datetime(value: str) -> datetime:
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
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
