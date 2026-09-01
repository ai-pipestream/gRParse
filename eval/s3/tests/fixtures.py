"""Canned Documents (protobuf JSON dicts) and fakes shared by the tests."""

from __future__ import annotations

import copy
import io
import struct
import sys
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.client import ConvertResult, ServiceInfo  # noqa: E402
from s3.checks import ObjectContext  # noqa: E402
from s3.document import View  # noqa: E402
from s3.source import ObjectRef  # noqa: E402
from s3.sourcefacts import SourceFacts  # noqa: E402

SUCCESS = "CONVERSION_STATUS_SUCCESS"
LIBRE = [{"collector": {"collector": "libreoffice", "model": "writer"}}]
CV = [{"collector": {"collector": "grparse", "model": "rapidocr"}}]


def prov(page: int, l: float, t: float, r: float, b: float) -> dict[str, Any]:
    return {"page_no": page, "bbox": {"l": l, "t": t, "r": r, "b": b, "coord_origin": "COORD_ORIGIN_TOPLEFT"}}


class Builder:
    """Assembles a well-formed Document item by item, refs kept mutual."""

    def __init__(self, mimetype: str, filename: str, collectors: tuple[str, ...] = ("libreoffice",)) -> None:
        self.doc: dict[str, Any] = {
            "schema_name": "docling_document_v2", "version": "1.10.0", "name": filename,
            "origin": {"mimetype": mimetype, "filename": filename, "binary_hash": "1", "mimetype_evidence": "magic",
                       "field_sources": [{"field": "mimetype", "source": {"collector": "grparse"}},
                                         {"field": "filename", "source": {"collector": "grparse"}}]},
            "body": {"self_ref": "#/body", "children": []},
            "furniture": {"self_ref": "#/furniture", "children": []},
            "texts": [], "tables": [], "pictures": [], "groups": [], "pages": {},
            "claims": [{"source": {"collector": "grparse"}, "origin": {"mimetype": mimetype, "filename": filename}}]
                      + [{"source": {"collector": c}, "source_meta": {"title": "T"}} for c in collectors],
        }

    def page(self, number: int, width: float = 12240, height: float = 15840) -> "Builder":
        self.doc["pages"][str(number)] = {"page_no": number, "size": {"width": width, "height": height}, "unit": "twip"}
        return self

    def _attach(self, ref: str, parent: str) -> None:
        holder = self.doc["body"] if parent == "#/body" else self.doc["furniture"] if parent == "#/furniture" else self.node(parent)
        holder.setdefault("children", []).append({"ref": ref})

    def node(self, ref: str) -> dict[str, Any]:
        arena, index = ref[2:].rsplit("/", 1)
        raw = self.doc[arena][int(index)]
        if arena == "texts":
            kind, inner = next(iter(raw.items()))
            return inner if kind == "code" else inner["base"]
        return raw

    def text(self, kind: str, text: str, parent: str = "#/body", *, page: int | None = None,
             box: tuple[float, float, float, float] | None = None, level: int | None = None,
             label: str | None = None, source: list | None = None, layer: str | None = None) -> str:
        ref = f"#/texts/{len(self.doc['texts'])}"
        base: dict[str, Any] = {"self_ref": ref, "parent": {"ref": parent},
                                "label": "DOC_ITEM_LABEL_" + (label or {"title": "TITLE", "section_header": "SECTION_HEADER",
                                                                          "list_item": "LIST_ITEM"}.get(kind, "TEXT")),
                                "text": text, "orig": text, "source": source if source is not None else LIBRE}
        if layer:
            base["content_layer"] = "CONTENT_LAYER_" + layer.upper()
        if page is not None and box is not None:
            base["prov"] = [prov(page, *box)]
        inner: dict[str, Any] = {"base": base}
        if level is not None:
            inner["level"] = level
        self.doc["texts"].append({kind: inner})
        self._attach(ref, parent)
        return ref

    def picture(self, parent: str = "#/body", *, page: int | None = None,
                box: tuple[float, float, float, float] | None = None, label: str = "PICTURE",
                source: list | None = None) -> str:
        ref = f"#/pictures/{len(self.doc['pictures'])}"
        item: dict[str, Any] = {"self_ref": ref, "parent": {"ref": parent}, "label": f"DOC_ITEM_LABEL_{label}",
                                "source": source if source is not None else LIBRE, "children": []}
        if page is not None and box is not None:
            item["prov"] = [prov(page, *box)]
        self.doc["pictures"].append(item)
        self._attach(ref, parent)
        return ref

    def table(self, cells: list[tuple[int, int, int, int, str]], rows: int, cols: int, parent: str = "#/body", *,
              page: int | None = None, box: tuple[float, float, float, float] | None = None,
              header_rows: int = 0, grid: bool = False, source: list | None = None) -> str:
        ref = f"#/tables/{len(self.doc['tables'])}"
        table_cells = []
        for r0, c0, rs, cs, text in cells:
            cell = {"start_row_offset_idx": r0, "start_col_offset_idx": c0, "end_row_offset_idx": r0 + rs,
                    "end_col_offset_idx": c0 + cs, "row_span": rs, "col_span": cs, "text": text}
            if r0 < header_rows:
                cell["column_header"] = True
            table_cells.append(cell)
        data: dict[str, Any] = {"num_rows": rows, "num_cols": cols, "table_cells": table_cells}
        if grid:
            data["grid"] = [{"cells": [{"start_row_offset_idx": r, "start_col_offset_idx": c, "row_span": 1,
                                        "col_span": 1} for c in range(cols)]} for r in range(rows)]
        item: dict[str, Any] = {"self_ref": ref, "parent": {"ref": parent}, "label": "DOC_ITEM_LABEL_TABLE",
                                "data": data, "source": source if source is not None else LIBRE, "children": []}
        if page is not None and box is not None:
            item["prov"] = [prov(page, *box)]
        self.doc["tables"].append(item)
        self._attach(ref, parent)
        return ref

    def group(self, label: str, name: str, parent: str = "#/body", **extra: Any) -> str:
        ref = f"#/groups/{len(self.doc['groups'])}"
        item = {"self_ref": ref, "parent": {"ref": parent}, "label": f"GROUP_LABEL_{label}", "name": name,
                "children": [], **extra}
        self.doc["groups"].append(item)
        self._attach(ref, parent)
        return ref

    def build(self) -> dict[str, Any]:
        return copy.deepcopy(self.doc)


def word_document() -> dict[str, Any]:
    """A two-page word-processing document: title, headings, prose, an anchored picture."""
    b = Builder("application/vnd.openxmlformats-officedocument.wordprocessingml.document", "report.docx")
    b.page(1).page(2)
    b.text("title", "Field Notes", page=1, box=(1800, 1440, 9000, 2000))
    b.text("section_header", "1. Introduction", page=1, box=(1800, 2600, 4000, 2950), level=1)
    b.text("text", "The first paragraph.", page=1, box=(1800, 3000, 10440, 3300))
    b.picture(page=1, box=(1800, 7500, 8280, 10700))
    b.text("text", "Figure 1: A rectangle.", page=1, box=(1800, 11000, 7800, 11200), label="CAPTION")
    b.text("text", "After the figure.", page=1, box=(1800, 11400, 10440, 11700))
    b.text("section_header", "2. Second", page=2, box=(1800, 1440, 4000, 1800), level=1)
    b.text("text", "Closing words.", page=2, box=(1800, 2000, 10440, 2300))
    return b.build()


def sheet_document() -> dict[str, Any]:
    b = Builder("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "quarter.xlsx")
    b.page(1, 24000, 15000)
    sheet = b.group("SHEET", "Revenue", sheet={"index": 0, "visible": True})
    b.table([(0, 0, 1, 1, "Region"), (0, 1, 1, 1, "Q1"), (1, 0, 1, 1, "North"), (1, 1, 1, 1, "12"),
             (2, 0, 1, 1, "South"), (2, 1, 1, 1, "9")], 3, 2, sheet, page=1, box=(0, 0, 0, 0), header_rows=1)
    chart = b.picture(sheet, page=1, box=(4779, 299, 13849, 4833), label="CHART")
    b.doc["pictures"][-1]["chart"] = {"has_column_headers": True}
    table = b.table([(0, 0, 1, 1, ""), (0, 1, 1, 1, "Q1"), (1, 0, 1, 1, "North"), (1, 1, 1, 1, "12")], 2, 2, chart,
                    page=1, box=(4779, 299, 13849, 4833), header_rows=1, grid=True)
    caption = b.text("text", "Revenue by region", chart, page=1, box=(4779, 299, 13849, 4833), label="CAPTION")
    b.doc["pictures"][-1]["captions"] = [{"ref": caption}]
    del table
    return b.build()


def html_document() -> dict[str, Any]:
    b = Builder("text/html", "page.html", collectors=("markup",))
    markup = [{"collector": {"collector": "markup"}}]
    b.text("title", "Page Title", source=markup)
    b.text("section_header", "Intro", level=1, source=markup)
    b.text("text", "Body prose.", source=markup)
    b.text("section_header", "Details", level=2, source=markup)
    b.text("text", "More prose.", source=markup)
    return b.build()


def email_document() -> dict[str, Any]:
    b = Builder("message/rfc822", "mail.eml", collectors=("email",))
    email = [{"collector": {"collector": "email"}}]
    b.text("title", "Subject line", source=email)
    b.text("text", "Hello there.", source=email)
    group = b.group("LIST", "attachments")
    item = b.text("list_item", "order.pdf (application/pdf, 25 bytes)", group, source=email)
    doc = b.build()
    doc["email"] = {"from": [{"address": "a@example.org"}], "to": [{"address": "b@example.org"}]}
    doc["source_meta"] = {"title": "Subject line"}
    doc["attachments"] = [{"id": "part:1.2", "name": "order.pdf", "media_type": "application/pdf",
                           "size_bytes": "25", "item_ref": item}]
    return doc


def epub_document() -> dict[str, Any]:
    b = Builder("application/epub+zip", "book.epub", collectors=("epub",))
    src = [{"collector": {"collector": "markup"}}]
    one = b.group("CHAPTER", "OEBPS/text/chap1.xhtml")
    b.text("title", "Chapter One", one, source=src)
    b.text("text", "First.", one, source=src)
    two = b.group("CHAPTER", "OEBPS/text/chap2.xhtml")
    b.text("section_header", "Chapter Two", two, level=1, source=src)
    b.text("text", "Second.", two, source=src)
    return b.build()


def scan_document() -> dict[str, Any]:
    b = Builder("image/png", "scan.png", collectors=())
    b.page(1, 1700, 2200)
    b.text("text", "Recognised line one", page=1, box=(100, 100, 1500, 160), source=CV)
    b.text("text", "Recognised line two", page=1, box=(100, 200, 1500, 260), source=CV)
    b.text("text", "Recognised line three", page=1, box=(100, 300, 1500, 360), source=CV)
    return b.build()


def deck_document() -> dict[str, Any]:
    b = Builder("application/vnd.openxmlformats-officedocument.presentationml.presentation", "deck.pptx")
    b.page(1, 14401, 10801).page(2, 14401, 10801)
    one = b.group("SLIDE", "Title slide")
    b.text("title", "Deck Title", one, page=1, box=(1080, 3355, 13319, 5669))
    b.text("text", "Speaker notes", one, layer="notes")
    two = b.group("SLIDE", "Second")
    b.text("section_header", "Second", two, page=2, box=(720, 432, 13679, 2231), level=1)
    b.text("list_item", "a bullet", two, page=2, box=(720, 2520, 13679, 9646))
    return b.build()


def result(document: dict[str, Any] | None, *, status: str = SUCCESS, rpc_error: str | None = None,
           canonical: str = "{}") -> ConvertResult:
    return ConvertResult(document=document or {}, markdown="# md", status=status, errors=[], elapsed_ms=12.0,
                         rpc_error=rpc_error, exports={"md": "# md", "canonical_json": canonical})


def context(document: dict[str, Any] | None, key: str, *, facts: SourceFacts | None = None, runs: int = 1,
            sniff: ConvertResult | None = None, second: dict[str, Any] | None = None) -> ObjectContext:
    from s3.formats import extension_of, family_of

    ext = extension_of(key)
    first = result(document)
    results = [first] + [result(second if second is not None else copy.deepcopy(document)) for _ in range(runs - 1)]
    view = View(document) if document else None
    return ObjectContext(key=key, ext=ext, family=family_of(ext), size=10, facts=facts or SourceFacts(),
                         runs=results, sniff=sniff, view=view)


def zip_bytes(entries: dict[str, bytes]) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return buffer.getvalue()


def epub_bytes() -> bytes:
    container = b'<?xml version="1.0"?><container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'
    opf = (b'<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0">'
           b'<manifest><item id="c1" href="text/chap1.xhtml" media-type="application/xhtml+xml"/>'
           b'<item id="c2" href="text/chap2.xhtml" media-type="application/xhtml+xml"/>'
           b'<item id="img" href="images/a.png" media-type="image/png"/></manifest>'
           b'<spine><itemref idref="c1"/><itemref idref="c2"/></spine></package>')
    return zip_bytes({"mimetype": b"application/epub+zip", "META-INF/container.xml": container,
                      "OEBPS/content.opf": opf, "OEBPS/text/chap1.xhtml": b"<html/>", "OEBPS/text/chap2.xhtml": b"<html/>"})


@dataclass
class FakeSource:
    objects: dict[str, bytes]
    fetched: list[str] = field(default_factory=list)

    def list_objects(self) -> list[ObjectRef]:
        return [ObjectRef(key=key, size=len(data)) for key, data in self.objects.items()]

    def fetch(self, key: str) -> bytes:
        self.fetched.append(key)
        return self.objects[key]


class FakeClient:
    """Answers by filename extension from a table of canned results; records every call."""

    def __init__(self, target: str, answers: dict[str, ConvertResult] | None = None, fail: bool = False,
                 sniff_fails: bool = False, outage_ext: str = "", die_after: int | None = None) -> None:
        self.target = target
        self.answers = answers or {}
        self.calls: list[tuple[str, tuple, bytes | None]] = []
        self.fail = fail
        self.sniff_fails = sniff_fails
        self.last_ext = ""
        # A collector leg that fails on the wire: gRParse answers UNAVAILABLE
        # for this extension while the service itself keeps answering.
        self.outage_ext = outage_ext
        # The service dies (service_info fails too) after this many conversions.
        self.die_after = die_after

    def __enter__(self) -> "FakeClient":
        return self

    def __exit__(self, *exc: object) -> None:
        return None

    def service_info(self) -> ServiceInfo:
        from scorecard.client import Unreachable

        if self.fail or (self.die_after is not None and len(self.calls) > self.die_after):
            raise Unreachable("down")
        return ServiceInfo(name="gRParse", version="test", target=self.target)

    def convert_bytes(self, data: bytes, filename: str, *, formats=(), collectors=(), ebcdic_layout_json=None):
        from scorecard.client import Unreachable
        from s3.formats import extension_of

        self.calls.append((filename, tuple(collectors), ebcdic_layout_json))
        if self.die_after is not None and len(self.calls) > self.die_after:
            raise Unreachable(f"{self.target}: failed to connect")
        ext = extension_of(filename)
        if ext and ext == self.outage_ext:
            raise Unreachable(f"{self.target}: markup collector: ")
        if not ext:
            # The extension-less sniff run: a server that routes by its bytes
            # answers as for the named file; one that does not fails on CV.
            if self.sniff_fails:
                return result(None, status="RPC_ERROR", rpc_error="INVALID_ARGUMENT: grparse-cv: not a raster")
            ext = self.last_ext
        self.last_ext = ext
        if ext in self.answers:
            return copy.deepcopy(self.answers[ext])
        return result(None, status="RPC_ERROR", rpc_error="FAILED_PRECONDITION: no collector")


def flat_png(width: int, height: int, rgb: tuple[int, int, int], dot: bool = False) -> bytes:
    """A truecolour PNG of one colour (a darker pixel at the origin with ``dot``),
    with every scanline filtered differently so the reader's unfiltering is exercised."""
    rows = []
    for y in range(height):
        line = bytearray()
        for x in range(width):
            line += bytes(rgb) if not (dot and x == 0 and y == 0) else bytes(v // 2 for v in rgb)
        rows.append(line)
    raw = bytearray()
    previous = bytearray(width * 3)
    for y, line in enumerate(rows):
        kind = y % 5
        raw.append(kind)
        for i, value in enumerate(line):
            a = line[i - 3] if i >= 3 else 0
            b = previous[i]
            c = previous[i - 3] if i >= 3 else 0
            if kind == 0:
                raw.append(value)
            elif kind == 1:
                raw.append((value - a) & 0xFF)
            elif kind == 2:
                raw.append((value - b) & 0xFF)
            elif kind == 3:
                raw.append((value - ((a + b) >> 1)) & 0xFF)
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
                raw.append((value - predictor) & 0xFF)
        previous = line

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")
