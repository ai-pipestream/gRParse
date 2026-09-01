"""Facts read from the object bytes themselves, in memory, so a check can
compare the parse with what the source declares: an EPUB's spine, a page's
headings and title, a mail's attachment parts, a deck's slide count, a
document's inline pictures, a CSV's grid. Cheap and forgiving: a fact that
cannot be read is None and the check that needs it does not run."""

from __future__ import annotations

import csv
import html
import io
import re
import zipfile
from dataclasses import dataclass, field
from html.parser import HTMLParser
from typing import Any
from xml.etree import ElementTree

_HTML_STRIP = re.compile(rb"<(script|style)\b.*?</\1>|<!--.*?-->", re.I | re.S)
_ATTACHMENT = re.compile(rb"^content-disposition:\s*attachment", re.I | re.M)
_MULTIPART = re.compile(rb"^content-type:\s*multipart/", re.I | re.M)
_PIC = re.compile(rb"<pic:pic\b")
_SLIDE_ENTRY = re.compile(r"^ppt/slides/slide(\d+)\.xml$")
_SHEET_CELL = re.compile(rb"<c\b")


@dataclass
class SourceFacts:
    ok: bool = True
    note: str = ""
    # html / markdown: (level, text) in source order, and whether the source
    # has any visible text at all (a page of bare tags has none to yield)
    headings: list[tuple[int, str]] | None = None
    title: str | None = None
    has_text: bool | None = None
    # email
    attachments: int | None = None
    multipart: bool | None = None
    # epub: spine hrefs in order with their media types
    spine: list[tuple[str, str]] | None = None
    # deck
    slides: int | None = None
    # word
    inline_pictures: int | None = None
    # sheet
    sheets: list[tuple[str, bool]] | None = None   # (name, non-empty)
    csv_rows: int | None = None
    csv_cols: int | None = None
    extra: dict[str, Any] = field(default_factory=dict)


def _zip(data: bytes) -> zipfile.ZipFile | None:
    try:
        return zipfile.ZipFile(io.BytesIO(data))
    except (zipfile.BadZipFile, OSError):
        return None


def _plain(markup: bytes) -> str:
    text = re.sub(rb"<[^>]+>", b"", markup).decode("utf-8", "replace")
    return " ".join(html.unescape(text).split())


class _HeadingReader(HTMLParser):
    """Headings and the title as the tokenizer sees them. A regex over the
    bytes read a literal "<h6>" inside a title="..." attribute as a heading
    (the WHATWG specification does that); the tokenizer knows an attribute
    value from a tag."""

    _SKIP = frozenset({"script", "style", "template", "textarea", "noscript"})

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.headings: list[tuple[int, str]] = []
        self.title: str | None = None
        self._open: tuple[int, list[str]] | None = None
        self._title: list[str] | None = None
        self._skip = 0

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag in self._SKIP:
            self._skip += 1
        elif self._skip == 0 and len(tag) == 2 and tag[0] == "h" and tag[1] in "123456":
            self._open = (int(tag[1]), [])
        elif tag == "title" and self.title is None:
            self._title = []

    def handle_endtag(self, tag: str) -> None:
        if tag in self._SKIP:
            self._skip = max(0, self._skip - 1)
        elif self._open is not None and tag == f"h{self._open[0]}":
            level, parts = self._open
            self.headings.append((level, " ".join("".join(parts).split())))
            self._open = None
        elif tag == "title" and self._title is not None:
            self.title = " ".join("".join(self._title).split())
            self._title = None

    def handle_data(self, data: str) -> None:
        if self._open is not None:
            self._open[1].append(data)
        if self._title is not None:
            self._title.append(data)


def html_facts(data: bytes) -> SourceFacts:
    body = _HTML_STRIP.sub(b"", data)
    reader = _HeadingReader()
    reader.feed(body.decode("utf-8", "replace"))
    reader.close()
    visible = _plain(re.sub(rb"<head\b.*?</head>", b"", body, flags=re.I | re.S))
    return SourceFacts(headings=reader.headings, title=reader.title, has_text=bool(visible))


def _md_inline(text: str) -> str:
    """Heading text as a reader sees it: emphasis and links unwrapped, trailing hashes dropped."""
    text = re.sub(r"\s+#+\s*$", "", text.strip())
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"[*_`]+", "", text)
    return " ".join(text.split())


def markdown_facts(data: bytes) -> SourceFacts:
    headings: list[tuple[int, str]] = []
    title = None
    in_fence = False
    lines = data.decode("utf-8", "replace").splitlines()
    start = 0
    if lines and lines[0].strip() == "---":
        for index in range(1, len(lines)):
            if lines[index].strip() in ("---", "..."):
                start = index + 1
                break
            key, _, value = lines[index].partition(":")
            if key.strip().lower() == "title" and value.strip():
                title = value.strip().strip("'\"")
    for index in range(start, len(lines)):
        line = lines[index]
        stripped = line.strip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        atx = re.match(r"^ {0,3}(#{1,6})\s+(\S.*)$", line)
        if atx:
            headings.append((len(atx.group(1)), _md_inline(atx.group(2))))
            continue
        if index + 1 < len(lines) and stripped and not stripped.startswith(("-", "*", "+", "|", ">", "#")):
            underline = lines[index + 1].strip()
            if len(underline) >= 3 and (set(underline) == {"="} or set(underline) == {"-"}):
                previous = lines[index - 1].strip() if index > start else ""
                if not previous:
                    headings.append((1 if underline[0] == "=" else 2, _md_inline(stripped)))
    return SourceFacts(headings=headings, title=title,
                       has_text=any(line.strip() for line in lines[start:]))


def email_facts(data: bytes) -> SourceFacts:
    return SourceFacts(attachments=len(_ATTACHMENT.findall(data)), multipart=bool(_MULTIPART.search(data)))


def epub_facts(data: bytes) -> SourceFacts:
    archive = _zip(data)
    if archive is None:
        return SourceFacts(ok=False, note="not a zip archive")
    try:
        container = ElementTree.fromstring(archive.read("META-INF/container.xml"))
        ns = {"c": "urn:oasis:names:tc:opendocument:xmlns:container"}
        rootfile = container.find(".//c:rootfile", ns)
        opf_path = rootfile.get("full-path") if rootfile is not None else None
        if not opf_path:
            return SourceFacts(ok=False, note="container.xml names no rootfile")
        opf = ElementTree.fromstring(archive.read(opf_path))
    except (KeyError, ElementTree.ParseError) as error:
        return SourceFacts(ok=False, note=f"opf unreadable: {error}")
    base = opf_path.rsplit("/", 1)[0] + "/" if "/" in opf_path else ""
    manifest: dict[str, tuple[str, str]] = {}
    for item in opf.iter():
        if item.tag.endswith("}item") or item.tag == "item":
            manifest[item.get("id", "")] = (base + item.get("href", ""), item.get("media-type", ""))
    spine = []
    for itemref in opf.iter():
        if itemref.tag.endswith("}itemref") or itemref.tag == "itemref":
            entry = manifest.get(itemref.get("idref", ""))
            if entry:
                spine.append(entry)
    return SourceFacts(spine=spine)


def deck_facts(data: bytes) -> SourceFacts:
    archive = _zip(data)
    if archive is None:
        return SourceFacts(ok=False, note="not a zip archive")
    slides = sum(1 for name in archive.namelist() if _SLIDE_ENTRY.match(name))
    return SourceFacts(slides=slides)


def word_facts(data: bytes) -> SourceFacts:
    archive = _zip(data)
    if archive is None:
        return SourceFacts(ok=False, note="not a zip archive")
    try:
        document = archive.read("word/document.xml")
    except KeyError:
        return SourceFacts(ok=False, note="no word/document.xml")
    return SourceFacts(inline_pictures=len(_PIC.findall(document)))


def xlsx_facts(data: bytes) -> SourceFacts:
    archive = _zip(data)
    if archive is None:
        return SourceFacts(ok=False, note="not a zip archive")
    try:
        workbook = ElementTree.fromstring(archive.read("xl/workbook.xml"))
        rels = ElementTree.fromstring(archive.read("xl/_rels/workbook.xml.rels"))
    except (KeyError, ElementTree.ParseError) as error:
        return SourceFacts(ok=False, note=f"workbook unreadable: {error}")
    targets = {}
    for rel in rels:
        targets[rel.get("Id", "")] = rel.get("Target", "")
    sheets: list[tuple[str, bool]] = []
    for sheet in workbook.iter():
        if not (sheet.tag.endswith("}sheet") or sheet.tag == "sheet"):
            continue
        rid = next((value for key, value in sheet.attrib.items() if key.endswith("}id") or key == "r:id"), "")
        target = targets.get(rid, "")
        target = target[1:] if target.startswith("/") else "xl/" + target
        try:
            non_empty = _SHEET_CELL.search(archive.read(target)) is not None
        except KeyError:
            non_empty = False
        sheets.append((sheet.get("name", ""), non_empty))
    return SourceFacts(sheets=sheets)


def csv_facts(data: bytes) -> SourceFacts:
    text = data.decode("utf-8-sig", "replace")
    rows = [row for row in csv.reader(io.StringIO(text)) if any(cell.strip() for cell in row)]
    return SourceFacts(csv_rows=len(rows), csv_cols=max((len(row) for row in rows), default=0))


def source_facts(ext: str, family: str, data: bytes) -> SourceFacts:
    try:
        if family == "html":
            return html_facts(data)
        if family == "markdown":
            return markdown_facts(data)
        if family == "email":
            return email_facts(data)
        if family == "epub":
            return epub_facts(data)
        if ext in ("pptx", "pptm"):
            return deck_facts(data)
        if ext in ("docx", "docm"):
            return word_facts(data)
        if ext in ("xlsx", "xlsm"):
            return xlsx_facts(data)
        if ext == "csv":
            return csv_facts(data)
    except Exception as error:  # noqa: BLE001 - a fact reader must never sink the run
        return SourceFacts(ok=False, note=f"{type(error).__name__}: {error}")
    return SourceFacts()
