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
        page = self.first_page(node)
        if page <= 0:
            return None
        size = self.page_size(page)
        height = size[1] if size else None
        left = top = right = bottom = None
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
            left = box_left if left is None else min(left, box_left)
            right = box_right if right is None else max(right, box_right)
            top = vertical[0] if top is None else min(top, vertical[0])
            bottom = vertical[1] if bottom is None else max(bottom, vertical[1])
        if left is None:
            return None
        return Box(page, left, top, right, bottom)

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
