"""The shape battery: one small function per check, each named, documented
and scoped to the objects it applies to. A check returns Failures (empty
means pass); the runner decides applicability with ``applies`` so a check
that does not run is reported as n/a, never as a pass. README.md lists
every check with its rule; keep the two in step."""

from __future__ import annotations

import re
import sys
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[1]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.client import ConvertResult  # noqa: E402
from scorecard.stability import first_difference  # noqa: E402
from scorecard.summary import Node, _table_cells, _top_class, normalize_text  # noqa: E402

from .document import View  # noqa: E402
from .formats import (  # noqa: E402
    KNOWN_COLLECTORS,
    NOT_SELF_DESCRIBING,
    PAGED,
    TEXT_BEARING,
    acceptable_mimetypes,
    declared_mimetype,
    pdf_has_text_layer,
)
from .integrity import integrity_errors  # noqa: E402
from .pngcheck import png_is_uniform  # noqa: E402
from .sourcefacts import SourceFacts  # noqa: E402

SUCCESS = "CONVERSION_STATUS_SUCCESS"
CHART_CLASSES = frozenset({"bar_chart", "line_chart", "pie_chart", "scatter_chart", "stacked_bar_chart"})
EVIDENCE_CAP = 12
SNIPPET = 80
# How far outside its page a box may sit before it counts as off the page,
# as a share of the page's dimension (renderers round, producers clip).
PAGE_TOLERANCE = 0.02


@dataclass
class Failure:
    check: str
    cause: str
    evidence: dict[str, Any] = field(default_factory=dict)


@dataclass
class ObjectContext:
    key: str
    ext: str
    family: str
    size: int
    facts: SourceFacts
    runs: list[ConvertResult]
    sniff: ConvertResult | None = None
    view: View | None = None

    @property
    def first(self) -> ConvertResult:
        return self.runs[0]


@dataclass(frozen=True)
class Check:
    name: str
    doc: str
    applies: Callable[[ObjectContext], bool]
    run: Callable[[ObjectContext], list[Failure]]


CHECKS: list[Check] = []


def has_view(ctx: ObjectContext) -> bool:
    return ctx.view is not None


def check(name: str, doc: str, applies: Callable[[ObjectContext], bool] = has_view):
    def register(run: Callable[[ObjectContext], list[Failure]]) -> Callable[[ObjectContext], list[Failure]]:
        CHECKS.append(Check(name=name, doc=doc, applies=applies, run=run))
        return run
    return register


def normalize_cause(text: str) -> str:
    """A stable grouping key: item numbers and counts collapsed."""
    text = re.sub(r"#/(\w+)/\d+", r"#/\1/*", text)
    text = re.sub(r"\d+", "#", text)
    return " ".join(text.split())[:140]


def snippet(text: str) -> str:
    return normalize_text(text)[:SNIPPET]


def _fail(check_name: str, cause: str, **evidence: Any) -> Failure:
    return Failure(check=check_name, cause=cause, evidence=evidence)


def _cap(items: list[Any]) -> list[Any]:
    return items[:EVIDENCE_CAP] + ([f"... +{len(items) - EVIDENCE_CAP} more"] if len(items) > EVIDENCE_CAP else [])


# ---- parse and validation ----------------------------------------------------

@check("parse_succeeds", "the unary parse returns a Document with CONVERSION_STATUS_SUCCESS and no RPC error",
       applies=lambda ctx: True)
def parse_succeeds(ctx: ObjectContext) -> list[Failure]:
    result = ctx.first
    if result.rpc_error:
        return [_fail("parse_succeeds", "rpc error: " + normalize_cause(result.rpc_error), rpc_error=result.rpc_error)]
    if result.status != SUCCESS or not result.document:
        return [_fail("parse_succeeds", f"status {result.status}", status=result.status,
                      errors=[e.get("message", "")[:SNIPPET] for e in result.errors])]
    return []


@check("integrity", "docling_integrity_errors is empty: references resolve, parents list children, page-plane "
                    "provenance names a 1-based page, anchored references point at items")
def integrity(ctx: ObjectContext) -> list[Failure]:
    errors = integrity_errors(ctx.view.doc)
    if not errors:
        return []
    return [_fail("integrity", normalize_cause(errors[0]), errors=_cap(errors), count=len(errors))]


def _reachable(view: View, root: str) -> tuple[set[str], list[str]]:
    """Refs reachable from a root through children, captions and footnotes;
    plus the refs listed more than once on the way."""
    root_node = view.doc.get(root[2:], {}) or {}
    seen: set[str] = set()
    listed: set[str] = set()
    repeats: list[str] = []
    # (ref, via a children list): a caption sits in its float's captions and
    # may also be listed among its children, which is one placement, not two.
    stack = [(child.get("ref", ""), True) for child in root_node.get("children", []) or []]
    while stack:
        ref, via_children = stack.pop()
        if ref in seen:
            if via_children and ref in listed:
                repeats.append(ref)
            if via_children:
                listed.add(ref)
            continue
        seen.add(ref)
        if via_children:
            listed.add(ref)
        node = view.arena.get(ref)
        if node is None:
            continue
        stack.extend((child.get("ref", ""), True) for child in node.base.get("children", []) or [])
        for key in ("captions", "footnotes"):
            stack.extend((child.get("ref", ""), False) for child in node.base.get(key, []) or [])
    return seen, repeats


@check("placement", "every arena item is reachable from exactly one of #/body or #/furniture, listed once, and "
                    "every captions/footnotes/references entry resolves")
def placement(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    body, body_repeats = _reachable(view, "#/body")
    furniture, furniture_repeats = _reachable(view, "#/furniture")
    failures = []
    both = sorted(body & furniture)
    orphans = sorted(ref for ref in view.arena if ref not in body and ref not in furniture)
    dangling = sorted(set(view.body.dangling + view.furniture.dangling))
    unresolved = []
    for ref, node in view.arena.items():
        for key in ("captions", "footnotes", "references"):
            for entry in node.base.get(key, []) or []:
                if entry.get("ref", "") not in view.arena:
                    unresolved.append(f"{ref}.{key} -> {entry.get('ref', '')}")
    if both:
        failures.append(_fail("placement", "item reachable from both body and furniture", refs=_cap(both)))
    if orphans:
        failures.append(_fail("placement", "arena item reachable from neither body nor furniture",
                              refs=_cap(orphans),
                              labels=_cap([view.label(view.arena[r]) for r in orphans])))
    if body_repeats or furniture_repeats:
        failures.append(_fail("placement", "item listed more than once in the tree",
                              refs=_cap(sorted(set(body_repeats + furniture_repeats)))))
    if dangling:
        failures.append(_fail("placement", "tree names an item that does not exist", refs=_cap(dangling)))
    if unresolved:
        failures.append(_fail("placement", "caption, footnote or reference names an item that does not exist",
                              refs=_cap(unresolved)))
    return failures


@check("custom_field_keys", "no custom_fields key contains ':' except the pinned cell:? escape (collector "
                            "warnings are judged by warnings_typed)")
def custom_field_keys(ctx: ObjectContext) -> list[Failure]:
    offenders = [f"{path}[{key}]" for path, key in ctx.view.custom_field_keys()
                 if ":" in key and key != "cell:?" and not key.startswith("collector_warnings:")]
    if not offenders:
        return []
    return [_fail("custom_field_keys", "colon-keyed custom field: " +
                  normalize_cause(offenders[0].rsplit("[", 1)[1].rstrip("]")), keys=_cap(offenders))]


@check("warnings_typed", "collector warnings ride a typed slot, never a collector_warnings:<name> custom field")
def warnings_typed(ctx: ObjectContext) -> list[Failure]:
    keyed = [(path, key) for path, key in ctx.view.custom_field_keys() if key.startswith("collector_warnings:")]
    if not keyed:
        return []
    fields = ((ctx.view.doc.get("body", {}) or {}).get("meta", {}) or {}).get("custom_fields", {}) or {}
    samples = {key: (fields.get(key) or [])[:3] for _, key in keyed if isinstance(fields.get(key), list)}
    return [_fail("warnings_typed", "collector warnings keyed as custom_fields strings",
                  keys=_cap([key for _, key in keyed]), samples=samples)]


def _field_exists(holder: dict[str, Any], dotted: str) -> bool:
    node: Any = holder
    for part in dotted.split("."):
        if not isinstance(node, dict) or part not in node:
            return False
        node = node[part]
    return True


@check("claims_resolve", "every Document.claims entry and every field_sources entry names a known collector, and "
                         "each field_sources.field exists on the message that lists it")
def claims_resolve(ctx: ObjectContext) -> list[Failure]:
    doc = ctx.view.doc
    problems = []
    for index, claim in enumerate(doc.get("claims", []) or []):
        collector = (claim.get("source") or {}).get("collector", "")
        if collector not in KNOWN_COLLECTORS:
            problems.append(f"claims[{index}] names unknown collector {collector!r}")
    for section in ("origin", "source_meta"):
        holder = doc.get(section)
        if not isinstance(holder, dict):
            continue
        for entry in holder.get("field_sources", []) or []:
            collector = (entry.get("source") or {}).get("collector", "")
            fieldname = entry.get("field", "")
            if collector not in KNOWN_COLLECTORS:
                problems.append(f"{section}.field_sources[{fieldname}] names unknown collector {collector!r}")
            if not fieldname or not _field_exists(holder, fieldname):
                problems.append(f"{section}.field_sources names {fieldname!r}, which the message does not carry")
    if not problems:
        return []
    return [_fail("claims_resolve", normalize_cause(problems[0]), problems=_cap(problems))]


@check("collector_sources", "every placed text, table, picture and form item carries at least one CollectorSource "
                            "naming a known collector")
def collector_sources(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    missing, unknown = [], []
    for node in view.body.nodes + view.furniture.nodes:
        if node.kind == "group":
            continue
        names = view.collectors(node)
        if not names:
            missing.append(node.ref)
        elif not names <= KNOWN_COLLECTORS:
            unknown.append(f"{node.ref}: {sorted(names - KNOWN_COLLECTORS)}")
    failures = []
    if missing:
        failures.append(_fail("collector_sources", "placed item without a collector source", refs=_cap(missing),
                              labels=_cap([view.label(view.arena[r]) for r in missing])))
    if unknown:
        failures.append(_fail("collector_sources", "item names a collector outside the fleet", refs=_cap(unknown)))
    return failures


# ---- origin ----------------------------------------------------------------

def _mimetype_ok(ext: str, actual: str) -> bool:
    if ext == "txt":
        return actual.startswith("text/")
    accepted = acceptable_mimetypes(ext)
    return not accepted or actual in accepted


@check("origin_mimetype", "origin.mimetype agrees with what the key's extension declares (aliases in "
                          "formats.MIMETYPE_ALIASES) and origin.mimetype_evidence says what it rests on",
       applies=lambda ctx: has_view(ctx) and declared_mimetype(ctx.ext) is not None)
def origin_mimetype(ctx: ObjectContext) -> list[Failure]:
    origin = ctx.view.doc.get("origin") or {}
    actual = origin.get("mimetype", "")
    evidence = origin.get("mimetype_evidence", "")
    failures = []
    if not _mimetype_ok(ctx.ext, actual):
        failures.append(_fail("origin_mimetype", f".{ctx.ext} declares {declared_mimetype(ctx.ext)} but the origin "
                              f"says {actual or '(nothing)'} from {evidence or '(no evidence)'}",
                              declared=declared_mimetype(ctx.ext), actual=actual, evidence=evidence))
    if not evidence:
        failures.append(_fail("origin_mimetype", "origin carries no mimetype_evidence", actual=actual))
    return failures


@check("sniff_route", "sent again under a name with no extension, the object still parses (routed by its bytes) "
                      "and its origin mimetype comes from magic and matches the extension's type; formats whose "
                      "bytes do not name them (OLE2 doc/xls/ppt/msg, compressed WARC) are exempt",
       applies=lambda ctx: ctx.sniff is not None and declared_mimetype(ctx.ext) is not None
       and ctx.ext not in NOT_SELF_DESCRIBING)
def sniff_route(ctx: ObjectContext) -> list[Failure]:
    result = ctx.sniff
    if result.rpc_error or result.status != SUCCESS or not result.document:
        return [_fail("sniff_route", "extension-less name is not routed by its bytes: " +
                      normalize_cause(result.rpc_error or result.status),
                      rpc_error=result.rpc_error, status=result.status)]
    origin = result.document.get("origin") or {}
    actual, evidence = origin.get("mimetype", ""), origin.get("mimetype_evidence", "")
    failures = []
    if not _mimetype_ok(ctx.ext, actual):
        failures.append(_fail("sniff_route", f"bytes of a .{ctx.ext} sniffed as {actual or '(nothing)'}",
                              actual=actual, evidence=evidence))
    if evidence != "magic":
        failures.append(_fail("sniff_route", f"undeclared origin rests on {evidence or '(nothing)'}, not magic",
                              actual=actual, evidence=evidence))
    return failures


# ---- pages -------------------------------------------------------------------

def _paged(ctx: ObjectContext) -> bool:
    return has_view(ctx) and (ctx.family in PAGED or bool(ctx.view.pages))


@check("page_count", "pages are numbered 1..N with positive sizes, paged families have at least one, and every "
                     "page-plane provenance names one of them", applies=_paged)
def page_count(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    numbers = sorted(view.pages)
    failures = []
    if not numbers:
        if ctx.family in PAGED:
            failures.append(_fail("page_count", f"{ctx.family} document with no pages"))
        return failures
    if numbers != list(range(1, len(numbers) + 1)):
        failures.append(_fail("page_count", "page numbers are not 1..N", pages=_cap(numbers)))
    for number in numbers:
        size = view.page_size(number)
        if size is None or size[0] <= 0 or size[1] <= 0:
            failures.append(_fail("page_count", "page without a positive size", page=number, size=size))
            break
    highest = max(numbers)
    stray = []
    for ref, node in view.arena.items():
        for prov in view.prov(node):
            page = int(prov.get("page_no", 0))
            if page > highest or (page < 1 and not any(k in prov for k in ("time", "byte_range", "grid"))):
                stray.append(f"{ref} page {page}")
    if stray:
        failures.append(_fail("page_count", "provenance names a page the document does not have",
                              refs=_cap(stray), pages=len(numbers)))
    return failures


@check("boxes_in_page", "every provenance box on a page lies inside that page's size (2% tolerance); "
                        "zero-area placeholders are exempt", applies=_paged)
def boxes_in_page(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    outside = []
    for ref, node in view.arena.items():
        for prov in view.prov(node):
            page = int(prov.get("page_no", 0))
            size = view.page_size(page)
            if page < 1 or size is None or "bbox" not in prov:
                continue
            width, height = size
            bbox = prov["bbox"]
            left, right = float(bbox.get("l", 0)), float(bbox.get("r", 0))
            top, bottom = float(bbox.get("t", 0)), float(bbox.get("b", 0))
            if right - left <= 0 or abs(bottom - top) <= 0:
                continue
            tol_x, tol_y = width * PAGE_TOLERANCE, height * PAGE_TOLERANCE
            if (left < -tol_x or right > width + tol_x or min(top, bottom) < -tol_y
                    or max(top, bottom) > height + tol_y):
                outside.append(f"{ref} p{page} [{left:.0f},{top:.0f},{right:.0f},{bottom:.0f}] "
                               f"page {width:.0f}x{height:.0f}")
    if not outside:
        return []
    return [_fail("boxes_in_page", "provenance box outside its page", boxes=_cap(outside), count=len(outside))]


@check("provenance_present", "in paged families every placed text, table and picture outside the notes layer "
                             "carries provenance with a 1-based page",
       applies=lambda ctx: has_view(ctx) and ctx.family in PAGED)
def provenance_present(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    missing = []
    for node in view.body.nodes + view.furniture.nodes:
        if node.kind == "group" or view.content_layer(node) == "notes":
            continue
        if view.first_page(node) < 1:
            missing.append(f"{node.ref} ({view.label(node)}) {snippet(view.text(node))!r}")
    if not missing:
        return []
    return [_fail("provenance_present", "placed item without page provenance", refs=_cap(missing), count=len(missing))]


# ---- text ----------------------------------------------------------------------

def _text_bearing(ctx: ObjectContext) -> bool:
    if not has_view(ctx):
        return False
    if ctx.family == "pdf":
        return pdf_has_text_layer(ctx.view.doc)
    if ctx.facts.has_text is False:
        return False
    return ctx.family in TEXT_BEARING


@check("text_present", "text-bearing types (pdf with a text layer, word, deck, html, markdown, xml, email, epub, "
                       "txt) yield at least one non-empty text item in the body; a markup source with no visible "
                       "text is exempt", applies=_text_bearing)
def text_present(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    if any(node.kind == "text" and view.text(node) for node in view.body.nodes):
        return []
    return [_fail("text_present", "no non-empty text item in the body",
                  texts=len(view.doc.get("texts", []) or []), body_items=len(view.body.nodes))]


@check("empty_text_items", "no whitespace-only text item is placed in the body")
def empty_text_items(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    empty = [f"{node.ref} ({view.label(node)})" for node in view.body.nodes if node.kind == "text" and not view.text(node)]
    if not empty:
        return []
    return [_fail("empty_text_items", "empty text item in the body", refs=_cap(empty), count=len(empty))]


# ---- tables ---------------------------------------------------------------------

def _table_problems(table: dict[str, Any]) -> list[str]:
    data = table.get("data", {}) or {}
    rows, cols = int(data.get("num_rows", 0)), int(data.get("num_cols", 0))
    cells = list(data.get("table_cells", []) or [])
    problems = []
    if rows <= 0 or cols <= 0:
        if cells:
            problems.append("cells without declared dimensions")
        return problems
    if not cells:
        problems.append("declared dimensions but no cells")
        return problems
    occupied: set[tuple[int, int]] = set()
    for cell in cells:
        r0 = int(cell.get("start_row_offset_idx", 0))
        c0 = int(cell.get("start_col_offset_idx", 0))
        r1 = int(cell.get("end_row_offset_idx", 0)) or r0 + max(1, int(cell.get("row_span", 1) or 1))
        c1 = int(cell.get("end_col_offset_idx", 0)) or c0 + max(1, int(cell.get("col_span", 1) or 1))
        if r1 <= r0 or c1 <= c0:
            problems.append(f"cell ({r0},{c0}) has an empty span")
            continue
        if r1 > rows or c1 > cols or r0 < 0 or c0 < 0:
            problems.append(f"cell ({r0},{c0})-({r1},{c1}) outside {rows}x{cols}")
            continue
        span_rows, span_cols = int(cell.get("row_span", 1) or 1), int(cell.get("col_span", 1) or 1)
        if span_rows != r1 - r0 or span_cols != c1 - c0:
            problems.append(f"cell ({r0},{c0}) spans {span_rows}x{span_cols} but its offsets say {r1 - r0}x{c1 - c0}")
        for r in range(r0, r1):
            for c in range(c0, c1):
                if (r, c) in occupied:
                    problems.append(f"cell ({r0},{c0}) overlaps an earlier cell at ({r},{c})")
                    break
                occupied.add((r, c))
    grid = data.get("grid", []) or []
    if grid:
        if len(grid) != rows:
            problems.append(f"grid has {len(grid)} rows, num_rows says {rows}")
        widths = {len(row.get("cells", []) or []) for row in grid}
        if widths and widths != {cols}:
            problems.append(f"grid rows have {sorted(widths)} cells, num_cols says {cols}")
    return problems


@check("table_grids", "every TableItem's cells fit num_rows x num_cols, spans equal their offsets, no two "
                      "cells overlap, and a materialised grid is num_rows rows of num_cols cells",
       applies=lambda ctx: has_view(ctx) and bool(ctx.view.doc.get("tables")))
def table_grids(ctx: ObjectContext) -> list[Failure]:
    problems = []
    for index, table in enumerate(ctx.view.doc.get("tables", []) or []):
        for problem in _table_problems(table):
            problems.append(f"#/tables/{index}: {problem}")
    if not problems:
        return []
    return [_fail("table_grids", normalize_cause(problems[0].split(": ", 1)[1]), problems=_cap(problems),
                  count=len(problems))]


def _numeric(cell: dict[str, Any]) -> bool:
    value = cell.get("value") or {}
    if "number" in value:
        return True
    text = (cell.get("text") or "").strip().replace(",", "")
    try:
        float(text)
        return bool(text)
    except ValueError:
        return False


def _header_problem(table: dict[str, Any]) -> str | None:
    """A first row of labels over numeric rows must be marked column_header."""
    data = table.get("data", {}) or {}
    cells = list(data.get("table_cells", []) or [])
    rows = int(data.get("num_rows", 0))
    if rows < 2 or not cells:
        return None
    first = [c for c in cells if int(c.get("start_row_offset_idx", 0)) == 0]
    rest = [c for c in cells if int(c.get("start_row_offset_idx", 0)) > 0]
    if len(first) < 2 or not rest:
        return None
    if any(_numeric(c) or not (c.get("text") or "").strip() for c in first):
        return None
    if not any(_numeric(c) for c in rest):
        return None
    if all(c.get("column_header") for c in first):
        return None
    return f"first row {[snippet(c.get('text', '')) for c in first][:4]} labels numeric rows but is not marked column_header"


@check("sheet_tables", "spreadsheets: every SHEET group carries exactly one table, every non-empty source sheet "
                       "has a group, a CSV's table matches its row and column count, and a label row over "
                       "numeric rows is marked column_header",
       applies=lambda ctx: has_view(ctx) and ctx.family == "sheet")
def sheet_tables(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    failures = []
    sheets = view.groups_labelled("sheet")
    if not sheets:
        return [_fail("sheet_tables", "no SHEET group in the body", groups=len(view.doc.get("groups", []) or []),
                      tables=len(view.doc.get("tables", []) or []))]
    without, extra = [], []
    for group in sheets:
        tables = [ref for ref in view.children_refs(group) if ref.startswith("#/tables/")]
        if not tables:
            without.append(f"{group.ref} {group.item.get('name', '')!r}")
        elif len(tables) > 1:
            extra.append(f"{group.ref} {group.item.get('name', '')!r}: {len(tables)} tables")
    if without:
        failures.append(_fail("sheet_tables", "sheet group without a table", groups=_cap(without)))
    if extra:
        failures.append(_fail("sheet_tables", "sheet group with more than one direct table", groups=_cap(extra)))
    if ctx.facts.sheets is not None:
        expected = [name for name, non_empty in ctx.facts.sheets if non_empty]
        names = [group.item.get("name", "") for group in sheets]
        missing = [name for name in expected if name not in names]
        if missing:
            failures.append(_fail("sheet_tables", "non-empty source sheet has no group", missing=_cap(missing),
                                  groups=_cap(names)))
    if ctx.facts.csv_rows is not None and len(sheets) == 1:
        tables = [view.arena[ref] for ref in view.children_refs(sheets[0]) if ref in view.arena and ref.startswith("#/tables/")]
        if tables:
            data = tables[0].item.get("data", {}) or {}
            rows, cols = int(data.get("num_rows", 0)), int(data.get("num_cols", 0))
            if (rows, cols) != (ctx.facts.csv_rows, ctx.facts.csv_cols):
                failures.append(_fail("sheet_tables", "csv grid does not match the source",
                                      table=f"{rows}x{cols}", source=f"{ctx.facts.csv_rows}x{ctx.facts.csv_cols}"))
    header_problems = []
    for group in sheets:
        for ref in view.children_refs(group):
            node = view.arena.get(ref)
            if node is None or node.kind != "table":
                continue
            problem = _header_problem(node.item)
            if problem:
                header_problems.append(f"{ref}: {problem}")
    if header_problems:
        failures.append(_fail("sheet_tables", "label row over numeric rows not marked column_header",
                              tables=_cap(header_problems)))
    return failures


# ---- decks, word processing, markup, mail, books ------------------------------

@check("slides", "presentations: at least one SLIDE group, one group per source slide, groups in page order, "
                 "and the deck title exactly once, on the first slide",
       applies=lambda ctx: has_view(ctx) and ctx.family == "deck")
def slides(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    groups = view.groups_labelled("slide")
    if not groups:
        return [_fail("slides", "no SLIDE group in the body", groups=len(view.doc.get("groups", []) or []))]
    failures = []
    if ctx.facts.slides is not None and ctx.facts.slides != len(groups):
        failures.append(_fail("slides", "slide group count differs from the source",
                              groups=len(groups), source=ctx.facts.slides))
    last_page = 0
    for group in groups:
        pages = [view.first_page(view.arena[ref]) for ref in view.children_refs(group) if ref in view.arena]
        pages = [p for p in pages if p > 0]
        if not pages:
            continue
        if min(pages) < last_page:
            failures.append(_fail("slides", "slide groups out of page order", group=group.ref,
                                  page=min(pages), previous=last_page))
            break
        last_page = max(last_page, min(pages))
    titles = view.titles()
    if len(titles) > 1:
        failures.append(_fail("slides", "deck title emitted more than once",
                              titles=_cap([snippet(view.text(t)) for t in titles])))
    elif titles and view.parent_ref(titles[0]) != groups[0].ref:
        failures.append(_fail("slides", "deck title is not on the first slide", parent=view.parent_ref(titles[0]),
                              first_slide=groups[0].ref))
    return failures


@check("docx_pictures", "word-processing documents: every placed picture sits on a page, follows an item on the "
                        "same or an earlier page, pictures come in page order, and a docx yields at least as "
                        "many pictures as it has inline drawings",
       applies=lambda ctx: has_view(ctx) and ctx.family == "word")
def docx_pictures(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    failures = []
    nodes = view.body.nodes
    pictures = [(index, node) for index, node in enumerate(nodes) if node.kind == "picture"]
    if ctx.facts.inline_pictures is not None and len(pictures) < ctx.facts.inline_pictures:
        failures.append(_fail("docx_pictures", "fewer pictures than the source's inline drawings",
                              pictures=len(pictures), source=ctx.facts.inline_pictures))
    last_page = 0
    for index, picture in pictures:
        page = view.first_page(picture)
        if page < 1:
            failures.append(_fail("docx_pictures", "picture without a page", ref=picture.ref))
            continue
        if page < last_page:
            failures.append(_fail("docx_pictures", "pictures out of page order", ref=picture.ref, page=page,
                                  previous=last_page))
        last_page = max(last_page, page)
        anchor = next((nodes[i] for i in range(index - 1, -1, -1) if nodes[i].kind != "picture"), None)
        if anchor is not None:
            anchor_page = view.first_page(anchor)
            if anchor_page > page:
                failures.append(_fail("docx_pictures", "picture anchored after an item on a later page",
                                      ref=picture.ref, page=page, anchor=anchor.ref, anchor_page=anchor_page))
    return failures


def _norm(text: str) -> str:
    return normalize_text(text).lower()


@check("headings", "html and markdown: at most one title; the title item is the source's <title> (or front-matter "
                   "title) or its first-level heading; every source heading appears in order with its level, a "
                   "first-level heading allowed to be the title",
       applies=lambda ctx: has_view(ctx) and ctx.family in ("html", "markdown"))
def headings(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    failures = []
    titles = view.titles()
    if len(titles) > 1:
        failures.append(_fail("headings", "more than one title item",
                              titles=_cap([snippet(view.text(t)) for t in titles])))
    source = ctx.facts.headings
    declared = _norm(ctx.facts.title or "")
    title_texts = {declared} - {""}
    if source is not None:
        title_texts |= {_norm(text) for level, text in source if level == 1}
    if ctx.facts.title is not None or source is not None:
        for title in titles:
            if _norm(view.text(title)) not in title_texts:
                failures.append(_fail("headings", "title item is neither the source title nor a first-level heading",
                                      title=snippet(view.text(title)), source_title=ctx.facts.title))
        if not titles and title_texts:
            failures.append(_fail("headings", "source declares a title but no title item was emitted",
                                  candidates=_cap(sorted(title_texts))))
    if source is None:
        return failures
    heads = view.headings()
    pointer = 0
    missing, wrong = [], []
    for level, text in source:
        key = _norm(text)
        if not key:
            continue
        found = next((i for i in range(pointer, len(heads)) if _norm(view.text(heads[i])) == key), None)
        if found is None:
            missing.append(f"h{level} {text[:SNIPPET]!r}")
            continue
        node = heads[found]
        if node.text_kind == "title":
            if level != 1:
                wrong.append(f"h{level} {text[:40]!r} became the title")
        elif view.level(node) != level:
            wrong.append(f"h{level} {text[:40]!r} -> level {view.level(node)}")
        pointer = found + 1
    if missing:
        failures.append(_fail("headings", "source heading missing or out of order", headings=_cap(missing),
                              document=_cap([f"{view.level(h)} {snippet(view.text(h))!r}" for h in heads])))
    if wrong:
        failures.append(_fail("headings", "heading level differs from the source", headings=_cap(wrong)))
    return failures


@check("email_shape", "mail: a typed EmailMeta with a sender, the subject as source_meta.title or the title "
                      "item, a non-empty body, and one attachments entry per attachment part with a name, a "
                      "media type and an item_ref that resolves",
       applies=lambda ctx: has_view(ctx) and ctx.family == "email")
def email_shape(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    doc = view.doc
    failures = []
    meta = doc.get("email")
    if not isinstance(meta, dict) or not meta.get("from"):
        failures.append(_fail("email_shape", "no typed EmailMeta with a sender", email=bool(meta)))
    subject = (doc.get("source_meta") or {}).get("title") or (view.text(view.titles()[0]) if view.titles() else "")
    if not subject:
        failures.append(_fail("email_shape", "no subject (source_meta.title or title item)"))
    if not any(node.kind == "text" and node.text_kind != "title" and view.text(node) for node in view.body.nodes):
        failures.append(_fail("email_shape", "no body text"))
    attachments = doc.get("attachments", []) or []
    if ctx.facts.attachments and len(attachments) < ctx.facts.attachments:
        failures.append(_fail("email_shape", "fewer attachments entries than attachment parts",
                              attachments=len(attachments), source=ctx.facts.attachments))
    bad = []
    for entry in attachments:
        if not entry.get("name") or not entry.get("media_type"):
            bad.append(f"{entry.get('id', '')}: name={entry.get('name', '')!r} media_type={entry.get('media_type', '')!r}")
        elif entry.get("item_ref") and entry["item_ref"] not in view.arena:
            bad.append(f"{entry.get('id', '')}: item_ref {entry['item_ref']} does not resolve")
    if bad:
        failures.append(_fail("email_shape", "attachment entry without name, media type or a resolving item_ref",
                              entries=_cap(bad)))
    return failures


@check("epub_spine", "books: one CHAPTER group per XHTML spine item, in spine order, each with content",
       applies=lambda ctx: has_view(ctx) and ctx.family == "epub")
def epub_spine(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    groups = view.groups_labelled("chapter")
    if not groups:
        return [_fail("epub_spine", "no CHAPTER group in the body", groups=len(view.doc.get("groups", []) or []))]
    failures = []
    if ctx.facts.spine is not None:
        xhtml = [href for href, media in ctx.facts.spine
                 if media in ("application/xhtml+xml", "text/html") or href.lower().endswith((".xhtml", ".html", ".htm"))]
        names = [group.item.get("name", "") or "" for group in groups]
        if len(groups) != len(xhtml):
            failures.append(_fail("epub_spine", "chapter group count differs from the spine",
                                  groups=len(groups), spine=len(xhtml), names=_cap(names)))
        elif all(names):
            in_order = all(name.endswith(href.rsplit("/", 1)[-1]) or href.endswith(name)
                           for name, href in zip(names, xhtml))
            if not in_order:
                failures.append(_fail("epub_spine", "chapter groups are not in spine order", names=_cap(names),
                                      spine=_cap(xhtml)))
    empty = [f"{group.ref} {group.item.get('name', '')!r}" for group in groups if not view.children_refs(group)]
    if empty:
        failures.append(_fail("epub_spine", "chapter group without content", groups=_cap(empty)))
    return failures


# ---- rasters and scans --------------------------------------------------------

def _ocr_scope(ctx: ObjectContext) -> bool:
    if not has_view(ctx):
        return False
    if ctx.family == "image":
        return True
    return ctx.family == "pdf" and bool(ctx.view.pages) and not pdf_has_text_layer(ctx.view.doc)


def _previews_blank(view: View) -> bool | None:
    """True when every page preview is one flat colour, False when some page
    has content, None when no preview can be judged."""
    verdicts = []
    for page in view.pages.values():
        uri = ((page.get("image") or {}).get("uri") or "")
        if not uri.startswith("data:image/png;base64,"):
            return None
        import base64

        verdict = png_is_uniform(base64.b64decode(uri.split(",", 1)[1]))
        if verdict is None:
            return None
        verdicts.append(verdict)
    return all(verdicts) if verdicts else None


@check("ocr_text", "images and scans: recognition yields at least one text item (or, for a picture-only "
                   "raster, a picture) unless every page preview is one flat colour, and every text box lies "
                   "inside its page", applies=_ocr_scope)
def ocr_text(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    texts = [node for node in view.body.nodes + view.furniture.nodes if node.kind == "text" and view.text(node)]
    pictures = [node for node in view.body.nodes if node.kind == "picture"]
    if not texts and (not pictures or ctx.family == "pdf"):
        blank = _previews_blank(view)
        if blank:
            return []
        what = "raster yields neither text nor a picture" if ctx.family != "pdf" else "scan yields no text"
        return [_fail("ocr_text", what, pages=len(view.pages), pictures=len(pictures),
                      previews="blank" if blank else "content" if blank is False else "none")]
    outside = []
    for node in texts:
        box = view.box(node)
        if box is None:
            outside.append(f"{node.ref} has no box")
            continue
        size = view.page_size(box.page)
        if size is None:
            continue
        width, height = size
        if box.left < -width * PAGE_TOLERANCE or box.right > width * (1 + PAGE_TOLERANCE) or \
                box.top < -height * PAGE_TOLERANCE or box.bottom > height * (1 + PAGE_TOLERANCE):
            outside.append(f"{node.ref} p{box.page} [{box.left:.0f},{box.top:.0f},{box.right:.0f},{box.bottom:.0f}]")
    if outside:
        return [_fail("ocr_text", "recognised text without a box inside the page", refs=_cap(outside))]
    return []


# ---- reading order --------------------------------------------------------------

def _single_column(boxes: list[Any]) -> bool:
    for i, a in enumerate(boxes):
        for b in boxes[i + 1:]:
            vertical_overlap = min(a.bottom, b.bottom) - max(a.top, b.top)
            disjoint = a.right <= b.left or b.right <= a.left
            if vertical_overlap > 0 and disjoint:
                return False
    return True


@check("reading_order", "on single-column pages where every placed body item has a box, body order is monotone "
                        "in (page, top) allowing half a line of overlap; captions, footnotes, furniture and "
                        "notes are exempt; multi-column pages are skipped",
       applies=lambda ctx: has_view(ctx) and ctx.family in PAGED - {"sheet"} and bool(ctx.view.pages))
def reading_order(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    per_page: dict[int, list[tuple[Node, Any]]] = {}
    for node in view.body.nodes:
        if node.kind == "group" or view.label(node) in ("caption", "footnote") or view.content_layer(node) == "notes":
            continue
        box = view.box(node)
        if box is None:
            continue
        per_page.setdefault(box.page, []).append((node, box))
    violations = []
    for page, placed in sorted(per_page.items()):
        if len(placed) < 3 or not _single_column([box for _, box in placed]):
            continue
        for (a_node, a), (b_node, b) in zip(placed, placed[1:]):
            if b.top < a.top - 0.5 * min(a.height, b.height):
                violations.append(f"p{page}: {b_node.ref} (top {b.top:.0f}) follows {a_node.ref} (top {a.top:.0f})"
                                  f" {snippet(view.text(a_node))!r} -> {snippet(view.text(b_node))!r}")
    if not violations:
        return []
    return [_fail("reading_order", "body order not monotone on a single-column page", violations=_cap(violations),
                  count=len(violations))]


# ---- charts ------------------------------------------------------------------------

@check("chart_composite", "a CHART picture carries exactly one bound table (its child, parented to it) and at "
                          "most one caption; a raster chart the derender leg answered carries a non-empty "
                          "tabular_chart table",
       applies=lambda ctx: has_view(ctx) and bool(ctx.view.doc.get("pictures")))
def chart_composite(ctx: ObjectContext) -> list[Failure]:
    view = ctx.view
    failures = []
    for node in view.nodes_of_kind("picture"):
        picture = node.item
        if view.label(node) == "chart":
            children = view.children_refs(node)
            tables = [ref for ref in children if ref.startswith("#/tables/")]
            bound = [ref for ref in tables if ref in view.arena and view.parent_ref(view.arena[ref]) == node.ref]
            if len(bound) != 1:
                failures.append(_fail("chart_composite", "chart picture without exactly one bound table",
                                      ref=node.ref, tables=tables, bound=bound))
            captions = [c.get("ref", "") for c in picture.get("captions", []) or []]
            if len(captions) > 1 or any(ref not in view.arena for ref in captions):
                failures.append(_fail("chart_composite", "chart caption missing or repeated", ref=node.ref,
                                      captions=captions))
            continue
        generated = any(s.get("generation") for s in picture.get("source", []) or [])
        if generated and _top_class(picture) in CHART_CLASSES:
            tables = [a.get("tabular_chart") for a in picture.get("annotations", []) or [] if a.get("tabular_chart")]
            if not tables or not _table_cells({"data": tables[0].get("chart_data", {}) or {}}):
                failures.append(_fail("chart_composite", "derendered chart without a tabular_chart table", ref=node.ref))
    return failures


# ---- determinism ---------------------------------------------------------------------

def mask_descriptive(value: Any, path: str = "") -> Any:
    """The document with its only descriptive leaf blanked: the title a VLM
    derendered onto a raster chart (annotations[*].tabular_chart.title and
    meta.tabular_chart.title), which a generative model may rephrase."""
    if isinstance(value, dict):
        out = {}
        for key, child in value.items():
            here = f"{path}.{key}" if path else key
            if key == "title" and path.endswith("tabular_chart"):
                out[key] = ""
            else:
                out[key] = mask_descriptive(child, here)
        return out
    if isinstance(value, list):
        return [mask_descriptive(child, f"{path}[]") for child in value]
    return value


@check("repeat_identical", "a second parse of the same bytes yields the same Document (protobuf JSON compared "
                           "leaf by leaf, the derendered chart title masked) and the same canonical JSON",
       applies=lambda ctx: has_view(ctx) and len(ctx.runs) >= 2)
def repeat_identical(ctx: ObjectContext) -> list[Failure]:
    first = mask_descriptive(ctx.runs[0].document)
    failures = []
    for index, run in enumerate(ctx.runs[1:], start=2):
        if run.rpc_error or run.status != SUCCESS:
            failures.append(_fail("repeat_identical", "repeat parse failed", run=index,
                                  rpc_error=run.rpc_error, status=run.status))
            continue
        diff = first_difference(first, mask_descriptive(run.document))
        if diff is not None:
            failures.append(_fail("repeat_identical", "repeat parse differs: " + normalize_cause(diff), run=index,
                                  path=diff))
            continue
        a, b = ctx.runs[0].exports.get("canonical_json", ""), run.exports.get("canonical_json", "")
        if a != b and not any(s.get("generation") for _, n in _pictures(ctx) for s in n.get("source", []) or []):
            at = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
            failures.append(_fail("repeat_identical", "canonical JSON differs between repeat parses", run=index,
                                  byte=at, context=(a[max(0, at - 40):at + 40], b[max(0, at - 40):at + 40])))
    return failures


def _pictures(ctx: ObjectContext) -> list[tuple[str, dict[str, Any]]]:
    return [(f"#/pictures/{i}", p) for i, p in enumerate(ctx.view.doc.get("pictures", []) or [])]


def run_checks(ctx: ObjectContext) -> tuple[dict[str, str], list[Failure]]:
    """Every applicable check once: (verdict per check name, failures)."""
    verdicts: dict[str, str] = {}
    failures: list[Failure] = []
    for entry in CHECKS:
        if not entry.applies(ctx):
            verdicts[entry.name] = "n/a"
            continue
        try:
            found = entry.run(ctx)
        except Exception as error:  # noqa: BLE001 - a crashing check is a failure with its traceback line
            found = [Failure(check=entry.name, cause=f"check crashed: {type(error).__name__}",
                             evidence={"error": str(error)[:200]})]
        verdicts[entry.name] = "fail" if found else "pass"
        failures.extend(found)
    return verdicts, failures
