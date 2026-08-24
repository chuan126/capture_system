from __future__ import annotations

import html
import os
import re
from dataclasses import dataclass
from pathlib import Path
from threading import Lock
from typing import Iterable

from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import Paragraph


class PdfFontError(RuntimeError):
    """报告字体缺失或无法由ReportLab加载。"""


@dataclass(frozen=True, slots=True)
class PdfFontSet:
    chinese: str
    latin: str
    latin_is_times_new_roman: bool


_FONT_LOCK = Lock()
_CHINESE_FONT_NAME = "CaptureSystemSongti"
_LATIN_FONT_NAME = "CaptureSystemTimesNewRoman"
_ASCII_RUN = re.compile(r"[\x00-\x7f]+|[^\x00-\x7f]+")
_DEFAULT_SONGTI_CANDIDATES = (
    Path("/usr/share/fonts/truetype/arphic-gbsn00lp/gbsn00lp.ttf"),
    Path("/usr/share/fonts/truetype/msttcorefonts/simsun.ttf"),
    Path("/usr/share/fonts/opentype/arphic/uming.ttc"),
)
_DEFAULT_TIMES_NEW_ROMAN_CANDIDATES = (
    Path("/usr/share/fonts/truetype/msttcorefonts/Times_New_Roman.ttf"),
    Path("/usr/share/fonts/truetype/msttcorefonts/times.ttf"),
    Path("/usr/share/fonts/truetype/msttcorefonts/TimesNewRoman.ttf"),
)


def register_report_fonts(chinese_path: Path | None = None) -> PdfFontSet:
    """注册宋体和Times New Roman；未安装后者时使用PDF标准Times字体。"""
    with _FONT_LOCK:
        if _CHINESE_FONT_NAME not in pdfmetrics.getRegisteredFontNames():
            configured_chinese = os.getenv("CAPTURE_PDF_FONT_PATH", "").strip()
            chinese_candidates: Iterable[Path]
            if chinese_path is not None:
                chinese_candidates = (chinese_path,)
            elif configured_chinese:
                chinese_candidates = (Path(configured_chinese).expanduser().resolve(),)
            else:
                chinese_candidates = _DEFAULT_SONGTI_CANDIDATES
            _register_required_font(
                _CHINESE_FONT_NAME,
                chinese_candidates,
                "PDF宋体不可用。请设置CAPTURE_PDF_FONT_PATH指向可读取的宋体TTF/TTC文件。",
            )

        latin_exact = _LATIN_FONT_NAME in pdfmetrics.getRegisteredFontNames()
        if not latin_exact:
            configured_latin = os.getenv("CAPTURE_PDF_LATIN_FONT_PATH", "").strip()
            latin_candidates = (
                (Path(configured_latin).expanduser().resolve(),)
                if configured_latin
                else _DEFAULT_TIMES_NEW_ROMAN_CANDIDATES
            )
            latin_exact = _register_optional_font(_LATIN_FONT_NAME, latin_candidates)
            if configured_latin and not latin_exact:
                raise PdfFontError(
                    "CAPTURE_PDF_LATIN_FONT_PATH配置的Times New Roman字体不可读取或无法加载"
                )

        return PdfFontSet(
            chinese=_CHINESE_FONT_NAME,
            latin=_LATIN_FONT_NAME if latin_exact else "Times-Roman",
            latin_is_times_new_roman=latin_exact,
        )


def mixed_font_markup(value: object, fonts: PdfFontSet) -> str:
    """按字符运行段切换中英文字体，并把换行转成ReportLab段落换行。"""
    text = str(value)
    output: list[str] = []
    for run in _ASCII_RUN.findall(text):
        font_name = fonts.latin if run.isascii() else fonts.chinese
        escaped = html.escape(run, quote=False).replace("\n", "<br/>")
        output.append(f'<font name="{font_name}">{escaped}</font>')
    return "".join(output)


def mixed_paragraph(value: object, style: object, fonts: PdfFontSet) -> Paragraph:
    return Paragraph(mixed_font_markup(value, fonts), style)


def mixed_text_width(value: object, fonts: PdfFontSet, font_size: float) -> float:
    total = 0.0
    for run in _ASCII_RUN.findall(str(value)):
        font_name = fonts.latin if run.isascii() else fonts.chinese
        total += pdfmetrics.stringWidth(run, font_name, font_size)
    return total


def draw_mixed_text(canvas: object, x: float, y: float, value: object, fonts: PdfFontSet, font_size: float) -> None:
    text_object = canvas.beginText(x, y)
    for run in _ASCII_RUN.findall(str(value)):
        font_name = fonts.latin if run.isascii() else fonts.chinese
        text_object.setFont(font_name, font_size)
        text_object.textOut(run)
    canvas.drawText(text_object)


def _register_required_font(name: str, candidates: Iterable[Path], prefix: str) -> None:
    errors: list[str] = []
    for candidate in candidates:
        if not candidate.is_file():
            errors.append(f"{candidate}不存在")
            continue
        try:
            pdfmetrics.registerFont(TTFont(name, str(candidate)))
            return
        except Exception as error:
            errors.append(f"{candidate}无法加载：{error}")
    detail = "；".join(errors) or "未配置可用字体"
    raise PdfFontError(f"{prefix} 当前检查结果：{detail}")


def _register_optional_font(name: str, candidates: Iterable[Path]) -> bool:
    for candidate in candidates:
        if not candidate.is_file():
            continue
        try:
            pdfmetrics.registerFont(TTFont(name, str(candidate)))
            return True
        except Exception:
            continue
    return False
