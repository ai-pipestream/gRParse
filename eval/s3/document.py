"""A read-only view over one Document (protobuf JSON dict) for the checks:
the arena, the body and furniture walks, geometry in one coordinate frame.
Walks and node shapes come from the scorecard summary so the two tools read
a Document the same way."""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

EVAL_DIR = Path(__file__).resolve().parents[1]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.summary import Node, Walk, _arena, _label, _walk_section, normalize_text, strip_enum  # noqa: E402

from .formats import item_collectors  # noqa: E402


@dataclass(frozen=True)
class Box:
    """An axis-aligned box measured downward from the page's top edge."""

    page: int
    left: float
    top: float
    right: float
    bottom: float

    @property
    def height(self) -> float:
        return self.bottom - self.top

    @property
    def width(self) -> float:
        return self.right - self.left


def top_down(bbox: dict[str, Any], page_height: float | None) -> tuple[float, float] | None:
    """(top, bottom) from the page's top edge whichever origin the box states."""
    t = float(bbox.get("t", 0.0))
    b = float(bbox.get("b", 0.0))
    origin = bbox.get("coord_origin", "")
    bottom_left = origin == "COORD_ORIGIN_BOTTOMLEFT" or (origin in ("", None) and t > b)
    if bottom_left:
        if page_height is None:
            return None
        return page_height - max(t, b), page_height - min(t, b)
    return min(t, b), max(t, b)


class View:
    def __init__(self, document: dict[str, Any]) -> None:
        self.doc = document
        self.arena: dict[str, Node] = _arena(document)
        self.body: Walk = _walk_section(document.get("body", {}) or {}, self.arena)
        self.furniture: Walk = _walk_section(document.get("furniture", {}) or {}, self.arena)
        self.pages: dict[int, dict[str, Any]] = {
            int(number): page for number, page in (document.get("pages", {}) or {}).items()}

    # ---- item accessors -------------------------------------------------

    @staticmethod
    def text(node: Node) -> str:
        return normalize_text(node.base.get("text")) if node.kind == "text" else ""

    @staticmethod
    def label(node: Node) -> str:
        """The item's DocItemLabel without its prefix, or "group:<label>"."""
        if node.kind == "group":
            return _label(node)
        return strip_enum(node.base.get("label"), "DOC_ITEM_LABEL_")

    @staticmethod
    def variant(node: Node) -> str:
        return _label(node)

    @staticmethod
    def level(node: Node) -> int | None:
        if node.kind == "text" and node.text_kind in ("section_header", "title"):
            return 0 if node.text_kind == "title" else int(node.item.get("level", 0))
        return None

    @staticmethod
    def prov(node: Node) -> list[dict[str, Any]]:
        return list(node.base.get("prov", []) or [])

    @staticmethod
    def collectors(node: Node) -> set[str]:
        return item_collectors(node.base)

    @staticmethod
    def content_layer(node: Node) -> str:
        return strip_enum(node.base.get("content_layer"), "CONTENT_LAYER_")

    @staticmethod
    def parent_ref(node: Node) -> str:
        return (node.base.get("parent") or {}).get("ref", "")

    @staticmethod
    def children_refs(node: Node) -> list[str]:
        return [child.get("ref", "") for child in node.base.get("children", []) or []]

    # ---- pages and geometry ---------------------------------------------

    def page_size(self, page_no: int) -> tuple[float, float] | None:
        page = self.pages.get(page_no)
        size = (page or {}).get("size") or {}
        if not size:
            return None
        return float(size.get("width", 0.0)), float(size.get("height", 0.0))

    def first_page(self, node: Node) -> int:
        pages = [int(p.get("page_no", 0)) for p in self.prov(node) if int(p.get("page_no", 0)) > 0]
        return min(pages) if pages else 0

    def box(self, node: Node) -> Box | None:
        """The union of the item's boxes on its first page, top-down; None
        when it has no page, no box, or a zero-area box only."""
        return self.box_on(node, self.first_page(node))

    def pages_of(self, node: Node) -> list[int]:
        """Every page the item has provenance on, in order."""
        return sorted({int(entry.get("page_no", 0)) for entry in self.prov(node)} - {0})

    def boxes_on(self, node: Node, page: int) -> list[Box]:
        """Each of the item's boxes on one page, top-down, in provenance
        order; zero-area boxes are left out. An item merged across a column
        break has one box per column."""
        if page <= 0:
            return []
        size = self.page_size(page)
        height = size[1] if size else None
        boxes = []
        for entry in self.prov(node):
            if int(entry.get("page_no", 0)) != page or "bbox" not in entry:
                continue
            bbox = entry["bbox"]
            vertical = top_down(bbox, height)
            if vertical is None:
                continue
            box_left, box_right = float(bbox.get("l", 0.0)), float(bbox.get("r", 0.0))
            if box_right - box_left <= 0 or vertical[1] - vertical[0] <= 0:
                continue
            boxes.append(Box(page, box_left, vertical[0], box_right, vertical[1]))
        return boxes

    def box_on(self, node: Node, page: int) -> Box | None:
        """The union of the item's boxes on one page, top-down; None when it
        has no box there."""
        boxes = self.boxes_on(node, page)
        if not boxes:
            return None
        return Box(page, min(b.left for b in boxes), min(b.top for b in boxes),
                   max(b.right for b in boxes), max(b.bottom for b in boxes))

    # ---- collections ----------------------------------------------------

    def body_nodes(self) -> list[Node]:
        return list(self.body.nodes)

    def nodes_of_kind(self, kind: str) -> list[Node]:
        return [node for node in self.arena.values() if node.kind == kind]

    def groups_labelled(self, label: str) -> list[Node]:
        return [node for node in self.body.nodes if node.kind == "group" and self.label(node) == f"group:{label}"]

    def titles(self) -> list[Node]:
        return [node for node in self.body.nodes if node.kind == "text" and node.text_kind == "title"]

    def headings(self) -> list[Node]:
        return [node for node in self.body.nodes
                if node.kind == "text" and node.text_kind in ("title", "section_header")]

    def custom_field_keys(self) -> list[tuple[str, str]]:
        """(json path, key) for every custom_fields entry anywhere in the document."""
        found: list[tuple[str, str]] = []

        def walk(value: Any, path: str) -> None:
            if isinstance(value, dict):
                for key, child in value.items():
                    here = f"{path}.{key}" if path else key
                    if key == "custom_fields" and isinstance(child, dict):
                        found.extend((here, field) for field in child)
                        continue
                    walk(child, here)
            elif isinstance(value, list):
                for index, child in enumerate(value):
                    walk(child, f"{path}[{index}]")

        walk(self.doc, "")
        return found
