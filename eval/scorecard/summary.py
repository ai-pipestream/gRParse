"""Project a Document (protobuf JSON dict) into a compact structural summary.

The summary is what gets committed as a baseline, so it holds only what a
regression should be judged on: the ordered reading sequence, the heading
hierarchy, every table grid, where each picture sits, the group tree, counts,
harness-derived warnings and the cross-collector agreement section. Image
bytes are reduced to a length and a digest; timings and confidences are left
out because they vary run to run.
"""

from __future__ import annotations

import base64
import hashlib
import re
from dataclasses import dataclass, field
from typing import Any

from .agreement import agreement_section
from .metrics import prefix_key

SUMMARY_SCHEMA = 1
SHORT_TEXT = 60
WARNING_LIST_CAP = 10

ARENAS = {
    "texts": "text", "tables": "table", "pictures": "picture", "groups": "group",
    "key_value_items": "key_value", "form_items": "form",
    "field_regions": "field_region", "field_items": "field_item",
}


def normalize_text(text: str | None) -> str:
    return " ".join((text or "").split())


def short_hash(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def strip_enum(value: str | None, prefix: str) -> str:
    if not value:
        return "unspecified"
    return value[len(prefix):].lower() if value.startswith(prefix) else value.lower()


@dataclass
class Node:
    ref: str
    kind: str
    item: dict[str, Any]
    depth: int
    text_kind: str = ""

    @property
    def base(self) -> dict[str, Any]:
        """The TextItemBase for text arena entries (CodeItem carries its fields inline)."""
        if self.kind != "text":
            return self.item
        return self.item.get("base", self.item)


@dataclass
class Walk:
    nodes: list[Node] = field(default_factory=list)
    dangling: list[str] = field(default_factory=list)
    visited: set[str] = field(default_factory=set)


def _arena(document: dict[str, Any]) -> dict[str, Node]:
    arena: dict[str, Node] = {}
    for key, kind in ARENAS.items():
        for index, raw in enumerate(document.get(key, []) or []):
            ref = f"#/{key}/{index}"
            text_kind = ""
            item = raw
            if kind == "text":
                text_kind, item = next(iter(raw.items())) if raw else ("text", {})
            arena[ref] = Node(ref=ref, kind=kind, item=item, depth=0, text_kind=text_kind)
    return arena


def _walk(root: dict[str, Any], arena: dict[str, Node], walk: Walk) -> None:
    stack: list[tuple[str, int]] = [(child.get("ref", ""), 1) for child in reversed(root.get("children", []) or [])]
    while stack:
        ref, depth = stack.pop()
        if ref in walk.visited:
            continue
        node = arena.get(ref)
        if node is None:
            walk.dangling.append(ref)
            continue
        walk.visited.add(ref)
        placed = Node(ref=node.ref, kind=node.kind, item=node.item, depth=depth, text_kind=node.text_kind)
        walk.nodes.append(placed)
        children = placed.base.get("children", []) or []
        for child in reversed(children):
            stack.append((child.get("ref", ""), depth + 1))


def _label(node: Node) -> str:
    if node.kind == "text":
        return node.text_kind or "text"
    if node.kind == "group":
        return "group:" + strip_enum(node.item.get("label"), "GROUP_LABEL_")
    return node.kind


def _table_cells(table: dict[str, Any]) -> list[list[Any]]:
    data = table.get("data", {}) or {}
    raw_cells = list(data.get("table_cells", []) or [])
    if not raw_cells:
        for row in data.get("grid", []) or []:
            raw_cells.extend(row.get("cells", []) or [])
    cells = []
    seen: set[tuple[int, int]] = set()
    for cell in raw_cells:
        row = int(cell.get("start_row_offset_idx", 0))
        col = int(cell.get("start_col_offset_idx", 0))
        if (row, col) in seen:
            continue
        seen.add((row, col))
        cells.append([row, col, max(1, int(cell.get("row_span", 1) or 1)), max(1, int(cell.get("col_span", 1) or 1)),
                      normalize_text(cell.get("text"))])
    cells.sort(key=lambda c: (c[0], c[1]))
    return cells


def _table_summary(node: Node, caption_text: str) -> dict[str, Any]:
    data = node.item.get("data", {}) or {}
    cells = _table_cells(node.item)
    rows = int(data.get("num_rows", 0)) or (max((c[0] + c[2] for c in cells), default=0))
    cols = int(data.get("num_cols", 0)) or (max((c[1] + c[3] for c in cells), default=0))
    raw_cells = (data.get("table_cells") or [])
    return {
        "ref": node.ref, "rows": rows, "cols": cols, "cells": cells,
        "column_headers": sum(1 for c in raw_cells if c.get("column_header")),
        "caption": caption_text[:SHORT_TEXT],
    }


def _image_summary(image: dict[str, Any] | None) -> dict[str, Any] | None:
    if not image:
        return None
    uri = image.get("uri", "") or ""
    out: dict[str, Any] = {"mimetype": image.get("mimetype", "")}
    match = re.match(r"data:([^;,]*)(;base64)?,(.*)$", uri, flags=re.S)
    if match:
        payload = match.group(3)
        try:
            raw = base64.b64decode(payload, validate=False) if match.group(2) else payload.encode()
        except ValueError:
            raw = payload.encode()
        out.update({"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
    elif uri:
        out["uri"] = uri[:200]
    return out


def _top_class(picture: dict[str, Any]) -> str:
    for annotation in picture.get("annotations", []) or []:
        classification = annotation.get("classification")
        if classification and classification.get("predicted_classes"):
            return classification["predicted_classes"][0].get("class_name", "")
    return ""


def _derender_summary(picture: dict[str, Any]) -> dict[str, Any] | None:
    """The chart table a VLM derendered onto a picture, when the picture carries
    a GenerationSource (the chart derender leg's provenance). Office charts
    carry their tabular annotation from the live model with no generation
    source, so they never get this block; their table is a bound TableItem
    already summarized under ``tables``. The title is the one field a repeat
    run may legitimately rephrase; ``stability`` treats it as descriptive."""
    generation = next((s.get("generation") for s in picture.get("source", []) or [] if s.get("generation")), None)
    if generation is None:
        return None
    for annotation in picture.get("annotations", []) or []:
        tabular = annotation.get("tabular_chart")
        if not tabular:
            continue
        data = tabular.get("chart_data", {}) or {}
        cells = _table_cells({"data": data})
        return {
            "model": generation.get("model", ""),
            "title": normalize_text(tabular.get("title"))[:SHORT_TEXT],
            "rows": int(data.get("num_rows", 0)) or max((c[0] + c[2] for c in cells), default=0),
            "cols": int(data.get("num_cols", 0)) or max((c[1] + c[3] for c in cells), default=0),
            "cells": cells,
        }
    return None


def _collectors(item: dict[str, Any]) -> list[str]:
    names = set()
    for source in item.get("source", []) or []:
        collector = source.get("collector", {}) or {}
        if collector.get("collector"):
            names.add(collector["collector"])
    return sorted(names)


def _caption_text(item: dict[str, Any], arena: dict[str, Node]) -> str:
    parts = []
    for cap in item.get("captions", []) or []:
        node = arena.get(cap.get("ref", ""))
        if node is not None:
            parts.append(normalize_text(node.base.get("text")))
    return " ".join(parts)


def _picture_summary(node: Node, arena: dict[str, Node], preceding_heading: str, group_name: str,
                     parent_label: str) -> dict[str, Any]:
    item = node.item
    prov = item.get("prov", []) or []
    derender = _derender_summary(item)
    return ({"derender": derender} if derender else {}) | {
        "ref": node.ref, "parent": (item.get("parent") or {}).get("ref", ""),
        "parent_label": parent_label, "parent_name": group_name,
        "preceding_heading": preceding_heading[:SHORT_TEXT],
        "page": int(prov[0].get("page_no", 0)) if prov else None,
        "image": _image_summary(item.get("image")),
        "caption": _caption_text(item, arena)[:SHORT_TEXT],
        "classification": _top_class(item),
        "collectors": _collectors(item),
    }


def _reading_entry(node: Node, label: str, text: str, digest: str, level: int | None) -> dict[str, Any]:
    entry: dict[str, Any] = {"ref": node.ref, "label": label, "text": text[:SHORT_TEXT], "hash": digest,
                             "key": prefix_key(text)}
    if level is not None:
        entry["level"] = level
    if node.kind == "group" and node.item.get("name"):
        entry["name"] = node.item["name"]
    return entry


def _cap(prefix: str, refs: list[str]) -> list[str]:
    shown = [f"{prefix}:{ref}" for ref in refs[:WARNING_LIST_CAP]]
    if len(refs) > WARNING_LIST_CAP:
        shown.append(f"{prefix}:...(+{len(refs) - WARNING_LIST_CAP} more)")
    return shown


def _markdown_summary(markdown: str) -> dict[str, Any]:
    return {
        "chars": len(markdown), "sha256": hashlib.sha256(markdown.encode("utf-8")).hexdigest(),
        "headings": len(re.findall(r"^#+ ", markdown, re.M)),
        "table_rows": len(re.findall(r"^\|", markdown, re.M)),
        "images": len(re.findall(r"!\[", markdown)),
    }


def summarize(document: dict[str, Any], markdown: str, *, doc_id: str, fmt: str, content_type: str,
              status: str, errors: list[dict[str, str]], rpc_error: str | None = None) -> dict[str, Any]:
    """The committed shape of one conversion. Pure: no I/O, no timing."""
    arena = _arena(document)
    body = _walk_section(document.get("body", {}) or {}, arena)
    furniture = _walk_section(document.get("furniture", {}) or {}, arena)
    reachable = body.visited | furniture.visited

    reading: list[dict[str, Any]] = []
    headings: list[dict[str, Any]] = []
    tables: list[dict[str, Any]] = []
    pictures: list[dict[str, Any]] = []
    groups: list[dict[str, Any]] = []
    by_label: dict[str, int] = {}
    reading_text: list[str] = []
    empty_texts: list[str] = []
    level_jumps: list[str] = []
    empty_tables: list[str] = []

    group_stack: list[tuple[int, str, str]] = []
    last_heading = ""
    last_level: int | None = None
    for node in body.nodes:
        while group_stack and group_stack[-1][0] >= node.depth:
            group_stack.pop()
        label = _label(node)
        by_label[label] = by_label.get(label, 0) + 1
        level: int | None = None
        text = ""
        digest = ""
        if node.kind == "text":
            text = normalize_text(node.base.get("text"))
            digest = short_hash(text)
            if not text:
                empty_texts.append(node.ref)
            else:
                reading_text.append(text)
            if node.text_kind in ("title", "section_header"):
                level = 0 if node.text_kind == "title" else int(node.item.get("level", 0))
                headings.append({"ref": node.ref, "level": level, "text": text[:SHORT_TEXT * 2]})
                if last_level is not None and level > last_level + 1:
                    level_jumps.append(node.ref)
                last_level = level
                last_heading = text
        elif node.kind == "table":
            table = _table_summary(node, _caption_text(node.item, arena))
            tables.append(table)
            digest = short_hash("|".join(f"{c[0]},{c[1]},{c[2]},{c[3]},{c[4]}" for c in table["cells"]))
            text = f"{table['rows']}x{table['cols']}"
            if not table["cells"]:
                empty_tables.append(node.ref)
            reading_text.extend(c[4] for c in table["cells"] if c[4])
        elif node.kind == "picture":
            parent_ref = (node.item.get("parent") or {}).get("ref", "")
            parent_node = arena.get(parent_ref)
            parent_label = "body" if parent_ref == "#/body" else (_label(parent_node) if parent_node else "unknown")
            group_name = group_stack[-1][2] if group_stack else ""
            picture = _picture_summary(node, arena, last_heading, group_name, parent_label)
            pictures.append(picture)
            digest = (picture["image"] or {}).get("sha256", "")[:16]
            text = picture["caption"]
        elif node.kind == "group":
            name = node.item.get("name", "") or ""
            groups.append({"ref": node.ref, "label": strip_enum(node.item.get("label"), "GROUP_LABEL_"),
                           "name": name, "depth": node.depth, "children": len(node.item.get("children", []) or [])})
            group_stack.append((node.depth, label, name))
            text = name
            digest = short_hash(label + "\x00" + name)
        else:
            text = normalize_text(node.base.get("text"))
            digest = short_hash(text)
        reading.append(_reading_entry(node, label, text, digest, level))

    furniture_reading = []
    for node in furniture.nodes:
        text = normalize_text(node.base.get("text")) if node.kind == "text" else ""
        furniture_reading.append({"ref": node.ref, "label": _label(node), "text": text[:SHORT_TEXT],
                                  "hash": short_hash(text), "key": prefix_key(text)})

    orphans = sorted((ref for ref in arena if ref not in reachable), key=_ref_key)
    unplaced_pictures = [ref for ref in orphans if ref.startswith("#/pictures/")]
    warnings: list[str] = []
    if rpc_error:
        warnings.append(f"rpc-error:{rpc_error}")
    if status != "CONVERSION_STATUS_SUCCESS":
        warnings.append(f"status:{status}")
    warnings.extend(f"error:{e.get('module', '')}:{normalize_text(e.get('message', ''))[:120]}" for e in errors)
    if not document:
        warnings.append("no-document")
    origin = document.get("origin", {}) or {}
    if origin.get("mimetype") and origin.get("mimetype") != content_type:
        warnings.append(f"mimetype-mismatch:{content_type}!={origin.get('mimetype')}")
    if document and not reading_text:
        warnings.append("no-reading-text")
    if not markdown and document:
        warnings.append("no-markdown")
    warnings.extend(_cap("dangling-ref", sorted(set(body.dangling + furniture.dangling))))
    warnings.extend(_cap("orphan", orphans))
    warnings.extend(_cap("picture-unplaced", unplaced_pictures))
    warnings.extend(_cap("empty-text", empty_texts))
    warnings.extend(_cap("empty-table", empty_tables))
    warnings.extend(_cap("heading-level-jump", level_jumps))

    full_text = "\n".join(reading_text)
    counts = {
        "texts": len(document.get("texts", []) or []), "tables": len(document.get("tables", []) or []),
        "pictures": len(document.get("pictures", []) or []), "groups": len(document.get("groups", []) or []),
        "pages": len(document.get("pages", []) or []),
        "key_value_items": len(document.get("key_value_items", []) or []),
        "form_items": len(document.get("form_items", []) or []),
        "body_items": len(body.nodes), "furniture_items": len(furniture.nodes),
        "headings": len(headings), "reading_chars": len(full_text), "orphans": len(orphans),
        "by_label": dict(sorted(by_label.items())),
    }
    return {
        "schema": SUMMARY_SCHEMA, "doc_id": doc_id, "format": fmt, "content_type": content_type,
        "filename": origin.get("filename", ""), "origin_mimetype": origin.get("mimetype", ""),
        "status": status, "collectors": sorted({c.get("source", {}).get("collector", "") for c in document.get("claims", []) or []} - {""}),
        "counts": counts, "warnings": warnings,
        "reading": reading, "reading_text": full_text, "headings": headings, "tables": tables,
        "pictures": pictures, "groups": groups, "furniture_reading": furniture_reading,
        "markdown": _markdown_summary(markdown), "agreement": agreement_section(document),
    }


def _walk_section(root: dict[str, Any], arena: dict[str, Node]) -> Walk:
    walk = Walk()
    _walk(root, arena, walk)
    return walk


def _ref_key(ref: str) -> tuple[str, int]:
    parts = ref.rsplit("/", 1)
    try:
        return parts[0], int(parts[1])
    except (IndexError, ValueError):
        return ref, 0
