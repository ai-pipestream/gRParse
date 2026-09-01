"""What changed between two summaries, as a reader would list it.

A sequence diff of the reading sequence (and of the furniture sequence)
keyed the same way as the order metric, so an edit inside one paragraph
shows as "edited" and only a real move shows as "moved". Pure functions:
the report layer decides how many lines to show.
"""

from __future__ import annotations

import difflib
from collections import Counter
from typing import Any

from .metrics import order_key

SHOWN_TEXT = 70
JSON_CAP = 200
REPORT_LINES = 20


def _item(entry: dict[str, Any]) -> dict[str, str]:
    return {"label": entry.get("label", ""), "text": (entry.get("text", "") or "")[:SHOWN_TEXT], "ref": entry.get("ref", "")}


def sequence_changes(baseline: list[dict[str, Any]], live: list[dict[str, Any]]) -> dict[str, list[dict[str, str]]]:
    """deleted / inserted / moved / edited items between two reading sequences.

    Deleted and inserted come from the opcodes over (label, key); an item whose
    key appears on both sides is a move and is listed once (its live position);
    an aligned item whose text hash differs is an edit, not a move.
    """
    a_keys = [order_key(e) for e in baseline]
    b_keys = [order_key(e) for e in live]
    matcher = difflib.SequenceMatcher(None, a_keys, b_keys, autojunk=False)
    deleted: list[dict[str, Any]] = []
    inserted: list[dict[str, Any]] = []
    edited: list[dict[str, str]] = []
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            for a, b in zip(baseline[i1:i2], live[j1:j2]):
                if a.get("hash") != b.get("hash"):
                    edited.append(_item(b))
            continue
        if tag in ("delete", "replace"):
            deleted.extend(baseline[i1:i2])
        if tag in ("insert", "replace"):
            inserted.extend(live[j1:j2])
    deleted_keys = Counter(order_key(e) for e in deleted)
    inserted_keys = Counter(order_key(e) for e in inserted)
    moved_keys = deleted_keys & inserted_keys
    moved: list[dict[str, str]] = []
    budget = Counter(moved_keys)
    kept_inserted: list[dict[str, str]] = []
    for entry in inserted:
        key = order_key(entry)
        if budget[key] > 0:
            budget[key] -= 1
            moved.append(_item(entry))
        else:
            kept_inserted.append(_item(entry))
    budget = Counter(moved_keys)
    kept_deleted: list[dict[str, str]] = []
    for entry in deleted:
        key = order_key(entry)
        if budget[key] > 0:
            budget[key] -= 1
        else:
            kept_deleted.append(_item(entry))
    return {"deleted": kept_deleted, "inserted": kept_inserted, "moved": moved, "edited": edited}


def document_changes(baseline: dict[str, Any], live: dict[str, Any], count_delta: dict[str, int]) -> dict[str, Any]:
    """The `changes` block for one document: reading diff, furniture diff, non-zero count deltas."""
    reading = sequence_changes(baseline.get("reading", []), live.get("reading", []))
    furniture = sequence_changes(baseline.get("furniture_reading", []), live.get("furniture_reading", []))
    return {
        "reading": {k: v[:JSON_CAP] for k, v in reading.items()},
        "furniture": {k: v[:JSON_CAP] for k, v in furniture.items()},
        "count_delta": {k: v for k, v in sorted(count_delta.items()) if v},
        "total": sum(len(v) for v in reading.values()) + sum(len(v) for v in furniture.values()),
    }


def has_changes(changes: dict[str, Any] | None) -> bool:
    return bool(changes) and (changes.get("total", 0) > 0 or bool(changes.get("count_delta")))


def _line(section: str, verb: str, item: dict[str, str]) -> str:
    text = item["text"].replace("|", "\\|")
    return f'- {section} {verb}: {item["label"]} "{text}" ({item["ref"]})'


def render_lines(changes: dict[str, Any], cap: int = REPORT_LINES) -> list[str]:
    """Markdown bullet lines, capped with a '... N more' tail."""
    lines: list[str] = []
    for section in ("reading", "furniture"):
        block = changes.get(section, {})
        for verb in ("deleted", "inserted", "moved", "edited"):
            lines.extend(_line(section, verb, item) for item in block.get(verb, []))
    if changes.get("count_delta"):
        lines.append("- counts: " + ", ".join(f"{k} {v:+d}" for k, v in changes["count_delta"].items()))
    if len(lines) > cap:
        return lines[:cap] + [f"- ... {len(lines) - cap} more"]
    return lines
