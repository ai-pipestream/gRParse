"""Pure metric functions over two summaries (baseline first, live second).

Nothing here reads files or talks to a service; every function takes plain
lists, strings and dicts from ``summary.summarize`` and returns numbers, so
each one is unit-testable in isolation. ``None`` means "not applicable":
neither side has the structure the metric is about.
"""

from __future__ import annotations

import difflib
import re
from collections import Counter
from collections.abc import Hashable, Sequence
from typing import Any

WORD = re.compile(r"\w+", re.UNICODE)


def words(text: str) -> list[str]:
    return WORD.findall(text.lower())


def text_similarity(baseline: str, live: str) -> float:
    """Word-sequence similarity in [0, 1]; identical text is 1.0 without a diff."""
    if baseline == live:
        return 1.0
    a, b = words(baseline), words(live)
    if not a and not b:
        return 1.0
    if not a or not b:
        return 0.0
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()


def levenshtein(a: Sequence[Hashable], b: Sequence[Hashable]) -> int:
    """Edit distance over arbitrary hashable elements, two-row DP."""
    if a == b:
        return 0
    if not a:
        return len(b)
    if not b:
        return len(a)
    previous = list(range(len(b) + 1))
    for i, item in enumerate(a, start=1):
        current = [i]
        for j, other in enumerate(b, start=1):
            cost = 0 if item == other else 1
            current.append(min(previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost))
        previous = current
    return previous[-1]


def sequence_similarity(baseline: Sequence[Hashable], live: Sequence[Hashable]) -> float:
    """1 - normalized edit distance; two empty sequences are identical."""
    longest = max(len(baseline), len(live))
    if longest == 0:
        return 1.0
    return 1.0 - levenshtein(baseline, live) / longest


KEY_WORDS = 6
KEY_CHARS = 40
KEY_SOURCE_CHARS = 60


def prefix_key(text: str) -> str:
    """A stable identity for an item: its first 6 normalized words, or its
    first 40 characters when it has fewer words. Read from the summary's
    60-character prefix so old and new baselines key identically."""
    source = " ".join((text or "")[:KEY_SOURCE_CHARS].split()).lower()
    tokens = WORD.findall(source)
    if len(tokens) >= KEY_WORDS:
        return " ".join(tokens[:KEY_WORDS])
    return source[:KEY_CHARS]


def order_key(entry: dict[str, Any]) -> tuple[str, str]:
    """(label, prefix key): what the order metric and the change diff align on."""
    return entry.get("label", ""), entry.get("key") or prefix_key(entry.get("text", ""))


def reading_order_similarity(baseline: list[dict[str, Any]], live: list[dict[str, Any]]) -> float:
    """Edit similarity over the (label, prefix key) reading sequence, so an
    edit inside an item is not a move; the full hash stays with the text metric."""
    return sequence_similarity([order_key(e) for e in baseline], [order_key(e) for e in live])


def _cell_triples(tables: list[dict[str, Any]]) -> Counter:
    triples: Counter = Counter()
    for index, table in enumerate(tables):
        for cell in table.get("cells", []):
            triples[(index, cell[0], cell[1], cell[4])] += 1
    return triples


def table_cell_f1(baseline: list[dict[str, Any]], live: list[dict[str, Any]]) -> tuple[float, float, float] | None:
    """(precision, recall, f1) over (table index, row, col, text) multisets."""
    a, b = _cell_triples(baseline), _cell_triples(live)
    if not a and not b:
        return None
    overlap = sum((a & b).values())
    precision = overlap / sum(b.values()) if b else 0.0
    recall = overlap / sum(a.values()) if a else 0.0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return precision, recall, f1


def _structure(table: dict[str, Any]) -> tuple[int, int, frozenset]:
    spans = frozenset((c[0], c[1], c[2], c[3]) for c in table.get("cells", []) if c[2] > 1 or c[3] > 1)
    return int(table.get("rows", 0)), int(table.get("cols", 0)), spans


def table_structure_match(baseline: list[dict[str, Any]], live: list[dict[str, Any]]) -> float | None:
    """Fraction of index-paired tables whose rows, cols and span layout are identical."""
    longest = max(len(baseline), len(live))
    if longest == 0:
        return None
    matched = sum(1 for a, b in zip(baseline, live) if _structure(a) == _structure(b))
    return matched / longest


def heading_score(baseline: list[dict[str, Any]], live: list[dict[str, Any]]) -> float | None:
    """Edit similarity over the (level, text) heading sequence."""
    if not baseline and not live:
        return None
    key = lambda h: (int(h.get("level", 0)), h.get("text", ""))  # noqa: E731
    return sequence_similarity([key(h) for h in baseline], [key(h) for h in live])


def _placement(picture: dict[str, Any]) -> tuple[str, str, str, Any]:
    return (picture.get("parent_label", ""), picture.get("parent_name", ""),
            picture.get("preceding_heading", ""), picture.get("page"))


def picture_placement_score(baseline: list[dict[str, Any]], live: list[dict[str, Any]]) -> float | None:
    """Multiset overlap of (parent, group, preceding heading, page) placements over the larger side.

    Pictures are matched as a multiset, not by index: the service appends
    model-detected figures in whatever order the pages finished, and that
    order is not a placement fact.
    """
    longest = max(len(baseline), len(live))
    if longest == 0:
        return None
    a, b = Counter(_placement(p) for p in baseline), Counter(_placement(p) for p in live)
    return sum((a & b).values()) / longest


def warning_delta(baseline: list[str], live: list[str]) -> tuple[int, list[str]]:
    """(count, list) of warnings present in live beyond the baseline multiset."""
    extra = Counter(live) - Counter(baseline)
    new = sorted(extra.elements())
    return len(new), new


def agreement_score(baseline: dict[str, Any] | None, live: dict[str, Any] | None) -> float | None:
    """Fraction of baseline field winners that the live document awards to the same collector."""
    if not baseline or not baseline.get("winners"):
        return None
    live_winners = (live or {}).get("winners", {}) or {}
    kept = sum(1 for path, collector in baseline["winners"].items() if live_winners.get(path) == collector)
    return kept / len(baseline["winners"])


def count_delta(baseline: dict[str, Any], live: dict[str, Any]) -> dict[str, int]:
    """live minus baseline for every scalar count both sides carry."""
    return {key: int(live[key]) - int(baseline[key]) for key in baseline
            if key in live and isinstance(baseline[key], int) and isinstance(live[key], int)}
