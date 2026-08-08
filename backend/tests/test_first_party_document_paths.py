from pathlib import Path
import re
from urllib.parse import unquote


def test_first_party_document_names_and_relative_markdown_links_resolve() -> None:
    root = Path(__file__).parents[2]
    first_party_files = [
        path
        for path in root.rglob("*")
        if path.is_file() and "third_party" not in path.parts and ".git" not in path.parts
    ]
    assert not [path for path in first_party_files if "#U" in path.name]

    broken: list[str] = []
    link_pattern = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
    for markdown in (path for path in first_party_files if path.suffix.lower() == ".md"):
        text = markdown.read_text(encoding="utf-8", errors="replace")
        for match in link_pattern.finditer(text):
            target = match.group(1).strip()
            if not target or target.startswith(("#", "http://", "https://", "mailto:", "data:")):
                continue
            relative = unquote(target.split("#", 1)[0].split("?", 1)[0])
            resolved = (markdown.parent / relative).resolve()
            try:
                resolved.relative_to(root.resolve())
            except ValueError:
                continue
            if not resolved.exists():
                line = text.count("\n", 0, match.start()) + 1
                broken.append(f"{markdown.relative_to(root)}:{line}: {target}")

    assert broken == []
