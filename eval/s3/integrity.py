"""A port of ``docling_integrity_errors`` (src/docling_map.cpp) over the
protobuf JSON form, so the battery can validate a Document without the
native tool. The rules are the same: every reference resolves, parents list
their children, page-plane provenance names a 1-based page, and the anchored
references (comments, span targets, changes, anchors) point at something."""

from __future__ import annotations

from typing import Any

from .formats import text_base

ROOTS = ("#/body", "#/furniture")


def _page_less(prov: dict[str, Any]) -> bool:
    return any(key in prov for key in ("time", "byte_range", "grid", "line_range"))


def integrity_errors(document: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    refs: set[str] = set(ROOTS)
    # A page is a destination too: outline rows and link spans point at
    # "#/pages/N", which resolves whenever the document has that page.
    refs.update(f"#/pages/{number}" for number in (document.get("pages", {}) or {}))
    parents: list[tuple[str, str]] = []
    children: dict[str, set[str]] = {}
    graph_item_refs: list[tuple[str, str]] = []

    def collect(node: dict[str, Any]) -> str:
        self_ref = node.get("self_ref", "")
        if not self_ref:
            errors.append("item with empty self_ref")
            return ""
        if self_ref in refs:
            errors.append(f"duplicate self_ref {self_ref}")
        refs.add(self_ref)
        for child in node.get("children", []) or []:
            children.setdefault(self_ref, set()).add(child.get("ref", ""))
        if "parent" in node:
            parents.append((self_ref, (node.get("parent") or {}).get("ref", "")))
        return self_ref

    def check_prov(owner: str, prov_list: list[dict[str, Any]]) -> None:
        for prov in prov_list:
            page = int(prov.get("page_no", 0))
            if page < 1 and not _page_less(prov):
                errors.append(f"provenance of {owner} has page_no {page}, which is not a 1-based page")

    def collect_graph(owner: str, graph: dict[str, Any]) -> None:
        for cell in graph.get("cells", []) or []:
            prov = cell.get("prov")
            if prov is not None and int(prov.get("page_no", 0)) < 1 and not _page_less(prov):
                errors.append(f"provenance of graph cell {cell.get('cell_id', 0)} of {owner} has page_no "
                              f"{int(prov.get('page_no', 0))}, which is not a 1-based page")
            if "item_ref" in cell:
                graph_item_refs.append((owner, (cell.get("item_ref") or {}).get("ref", "")))

    for root in ROOTS:
        for child in (document.get(root[2:], {}) or {}).get("children", []) or []:
            children.setdefault(root, set()).add(child.get("ref", ""))
    for group in document.get("groups", []) or []:
        collect(group)
    for item in document.get("texts", []) or []:
        if not item:
            errors.append("text item with unset variant")
            continue
        base = text_base(item)
        owner = collect(base)
        check_prov(owner, base.get("prov", []) or [])
    for arena in ("pictures", "tables", "field_regions", "field_items"):
        for item in document.get(arena, []) or []:
            owner = collect(item)
            check_prov(owner, item.get("prov", []) or [])
    for arena in ("key_value_items", "form_items"):
        for item in document.get(arena, []) or []:
            owner = collect(item)
            check_prov(owner, item.get("prov", []) or [])
            collect_graph(owner, item.get("graph", {}) or {})

    for owner, child_refs in children.items():
        for child in sorted(child_refs):
            if child not in refs:
                errors.append(f"child {child} of {owner} does not resolve")
    for child_ref, parent_ref in parents:
        if parent_ref not in refs:
            errors.append(f"parent {parent_ref} of {child_ref} does not resolve")
            continue
        if child_ref not in children.get(parent_ref, set()):
            errors.append(f"parent {parent_ref} does not list {child_ref} as a child")
    for table in document.get("tables", []) or []:
        for comment in table.get("comments", []) or []:
            if comment.get("ref", "") not in refs:
                errors.append(f"comment ref {comment.get('ref', '')} of {table.get('self_ref', '')} does not resolve")
    for owner, item_ref in graph_item_refs:
        if item_ref not in refs:
            errors.append(f"graph cell item_ref {item_ref} of {owner} does not resolve")
    for item in document.get("texts", []) or []:
        base = text_base(item)
        if not base or next(iter(item)) == "code":
            continue
        for comment in base.get("comments", []) or []:
            if comment.get("ref", "") not in refs:
                errors.append(f"comment ref {comment.get('ref', '')} of {base.get('self_ref', '')} does not resolve")
        for span in base.get("spans", []) or []:
            target = span.get("target")
            if target is not None and target.get("ref", "") not in refs:
                errors.append(f"span target {target.get('ref', '')} of {base.get('self_ref', '')} does not resolve")
    for change in document.get("changes", []) or []:
        target = change.get("target")
        if target is not None and target.get("ref", "") not in refs:
            errors.append(f"change target {target.get('ref', '')} of {change.get('id', '')} does not resolve")
    for anchor in document.get("anchors", []) or []:
        target = anchor.get("target")
        if target is not None and target.get("ref", "") not in refs:
            errors.append(f"anchor target {target.get('ref', '')} of {anchor.get('name', '')} does not resolve")
    return errors
