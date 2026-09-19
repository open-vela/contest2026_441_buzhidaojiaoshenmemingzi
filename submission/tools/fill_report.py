#!/usr/bin/env python3
"""Fill the official contest DOCX without changing its section structure."""

from __future__ import annotations

import re
from pathlib import Path

from docx import Document
from docx.oxml.ns import qn
from docx.shared import Pt


ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "submission" / "2026首届openvela大赛作品提交模板.docx"
DRAFT = ROOT / "submission" / "技术报告正文草案.md"
OUTPUT = ROOT / "submission" / "技术报告-终版候选.docx"


def clean_markdown(text: str) -> str:
    text = re.sub(r"\[([^]]+)\]\([^)]+\)", r"\1", text)
    # Remove fenced-code markers before stripping individual backticks;
    # otherwise a ```text fence is rendered as the stray word "text".
    text = text.replace("```text", "").replace("```", "")
    text = text.replace("`", "")
    text = re.sub(r"^#{1,6}\s*", "", text, flags=re.MULTILINE)
    text = re.sub(r"^\s*[-*]\s+", "• ", text, flags=re.MULTILINE)
    text = re.sub(r"^\s*\d+\.\s+", "", text, flags=re.MULTILINE)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def markdown_sections(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"(?m)^##\s+([^\n]+)\s*$", text))
    result: dict[str, str] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        key = match.group(1).strip()
        result[key] = text[match.end():end].strip()
    return result


def split_subsections(text: str) -> list[str]:
    parts = re.split(r"(?m)^(###\s+[^\n]+)$", text)
    blocks: list[str] = []
    prefix = parts[0].strip()
    if prefix:
        blocks.append(prefix)
    for index in range(1, len(parts), 2):
        heading = parts[index].removeprefix("###").strip()
        body = parts[index + 1].strip() if index + 1 < len(parts) else ""
        blocks.append(f"{heading}\n{body}".strip())
    return blocks or [text]


def set_paragraph_text(paragraph, text: str) -> None:
    paragraph.clear()
    run = paragraph.add_run(clean_markdown(text))
    run.font.name = "宋体"
    run._element.rPr.rFonts.set(qn("w:eastAsia"), "宋体")
    run.font.size = Pt(10.5)


def find_heading(paragraphs, prefix: str) -> int:
    for index, paragraph in enumerate(paragraphs):
        if paragraph.text.strip().startswith(prefix):
            return index
    raise RuntimeError(f"official template heading not found: {prefix}")


def fill_section(document: Document, section: str, body: str,
                 next_section: str | None) -> None:
    paragraphs = document.paragraphs
    start = find_heading(paragraphs, section) + 1
    end = find_heading(paragraphs, next_section) if next_section else len(paragraphs)
    slots = paragraphs[start:end]
    if not slots:
        raise RuntimeError(f"official template has no content slot for {section}")
    blocks = split_subsections(body)
    if len(blocks) > len(slots):
        blocks = blocks[: len(slots) - 1] + ["\n\n".join(blocks[len(slots) - 1:])]
    for index, paragraph in enumerate(slots):
        set_paragraph_text(paragraph, blocks[index] if index < len(blocks) else "")


def main() -> None:
    document = Document(TEMPLATE)
    sections = markdown_sections(DRAFT.read_text(encoding="utf-8"))

    set_paragraph_text(document.paragraphs[0], "BK7258 R1 视觉辅助胸牌 OpenVela 技术报告")
    document.paragraphs[0].runs[0].font.size = Pt(20)
    document.paragraphs[0].runs[0].bold = True
    for section in document.sections:
        for paragraph in section.header.paragraphs:
            paragraph.clear()  # Remove the template's unrelated picture watermark.

    info = document.tables[2]
    info.cell(1, 1).text = "BK7258 R1 视觉辅助胸牌 OpenVela"
    info.cell(2, 1).text = "不知道叫什么名字（队伍编号 441）"
    info.cell(3, 1).text = "邢晓亮（GitHub：xxluestc）：BSP 移植、驱动适配、构建烧录、实机验证、应用与报告整理"
    info.cell(4, 1).text = "新硬件平台适配"

    abstract = sections.get("2. 摘要") or sections.get("2、摘要")
    if not abstract:
        raise RuntimeError("draft abstract section missing")
    # The official template keeps the abstract instruction immediately after
    # paragraph 6 ("2、摘要").  Use the fixed template slot so the earlier
    # submission-checklist item numbered 2 cannot be mistaken for this title.
    set_paragraph_text(document.paragraphs[7], abstract)

    order = ["3.1", "3.2", "3.3", "3.4", "3.5", "3.6", "3.7"]
    for index, prefix in enumerate(order):
        body = next((value for key, value in sections.items()
                     if key.startswith(prefix)), None)
        if body is None:
            raise RuntimeError(f"draft section missing: {prefix}")
        next_prefix = order[index + 1] if index + 1 < len(order) else "四"
        fill_section(document, prefix, body, next_prefix)

    ai = document.tables[3]
    ai.cell(1, 1).text = "未建立逐行归因口径，不填虚构百分比"
    ai.cell(2, 1).text = "Codex、MiMo Code、本地终端、源码检索与文档处理"
    ai.cell(3, 1).text = "未使用 VelaJS MCP 或 Figma MCP"
    ai.cell(4, 1).text = "bk7258-openvela-porting；用于来源审查、分层、构建与证据门禁，12 项测试通过"
    ai.cell(5, 1).text = "日志 3 个会话、76 条事件；无 Token 字段，未虚构总量"

    for table in (info, ai):
        for row in table.rows:
            for cell in row.cells:
                for paragraph in cell.paragraphs:
                    for run in paragraph.runs:
                        run.font.name = "宋体"
                        run._element.rPr.rFonts.set(qn("w:eastAsia"), "宋体")
                        run.font.size = Pt(10.5)

    # The repository and PR rules are already stated in the report.  Drop the
    # template's duplicate final notice so it does not create a one-line page.
    for paragraph in document.paragraphs:
        if paragraph.text.strip().startswith("大赛仅在 GitHub 进行"):
            paragraph._element.getparent().remove(paragraph._element)
            break

    document.core_properties.title = "BK7258 R1 视觉辅助胸牌 OpenVela"
    document.core_properties.subject = "2026 首届 OpenVela AI 硬件开发者大赛技术报告"
    document.save(OUTPUT)
    print(OUTPUT)


if __name__ == "__main__":
    main()
