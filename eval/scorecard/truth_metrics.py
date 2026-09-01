"""Absolute metrics: a live summary against hand-written ground truth.

The baseline metrics in ``metrics.py`` measure drift from the previous run;
these measure distance from what the source document actually says. Truth
files (``truth/<doc-id>.json``) hold a few dozen facts a careful reader
derives from the source, never from parser output:

- ``headings``: ``[{"text": ..., "level": ...}]`` in document order, level 0
  for the document title, 1 for a top section, and so on;
- ``anchors``: short unique snippets from the start of paragraphs, in the
  order they must be read (across columns and pages);
- ``tables``: ``[{"table": <hint>, "cells": [[row, col, "text"], ...]}]``,
  a handful of cells per table, zero-based, spans by their top-left cell;
- ``figures``: ``[{"after": "<anchor of the text the figure follows>",
  "caption": "..."}]``.

Everything here is a pure function over that JSON and the summary produced
by ``summary.summarize``. Reading order and presence are judged on the full
``reading_text``; figure placement needs positions, so it uses the reading
entries, whose text is a 60-character prefix (anchors are therefore the
opening words of a paragraph). ``None`` means the truth file has nothing to
say about that metric.
"""

from __future__ import annotations

import unicodedata
from bisect import bisect_left
from dataclasses import dataclass
from typing import Any, Sequence

# curly quotes, en and em dashes, soft hyphen
QUOTES = {"\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u2013": "-", "\u2014": "-", "\u00ad": ""}
MIN_PREFIX = 4


def normalize(text: str | None) -> str:
    """Lowercase NFKC text with straight quotes, plain hyphens and single spaces."""
    folded = unicodedata.normalize("NFKC", text or "")
    for fancy, plain in QUOTES.items():
        folded = folded.replace(fancy, plain)
    return " ".join(folded.lower().split())


def prefix_match(truth: str, live: str) -> bool:
    """Either normalized text is a prefix of the other; both at least MIN_PREFIX long."""
    a, b = normalize(truth), normalize(live)
    if len(a) < MIN_PREFIX or len(b) < MIN_PREFIX:
        return a == b and bool(a)
    return a.startswith(b) or b.startswith(a)


def find_entry(snippet: str, entries: Sequence[dict[str, Any]], start: int = 0) -> int | None:
    """Index of the first reading entry at or after ``start`` whose text contains the snippet."""
    needle = normalize(snippet)
    if not needle:
        return None
    for index in range(start, len(entries)):
        if needle in normalize(entries[index].get("text", "")):
            return index
    return None


def longest_increasing_run(values: Sequence[int]) -> int:
    """Length of the longest strictly increasing subsequence (patience sorting)."""
    tails: list[int] = []
    for value in values:
        slot = bisect_left(tails, value)
        if slot == len(tails):
            tails.append(value)
        else:
            tails[slot] = value
    return len(tails)


@dataclass(frozen=True)
class HeadingScore:
    f1: float
    recall: float
    precision: float
    level_exact: float | None
    matched: int
    truth_total: int
    live_total: int
    missing: tuple[str, ...]


def heading_scores(truth: Sequence[dict[str, Any]], live: Sequence[dict[str, Any]]) -> HeadingScore | None:
    """Greedy in-order prefix matching of truth headings to live headings.

    Recall counts truth headings found, precision counts live headings that
    are truth headings, and level exactness is judged over the matched pairs
    only, so a heading found at the wrong depth costs the level score and
    not the text score.
    """
    if not truth:
        return None
    used: set[int] = set()
    matched = 0
    level_hits = 0
    missing: list[str] = []
    cursor = 0
    for heading in truth:
        hit = next((i for i in range(cursor, len(live)) if i not in used and prefix_match(heading["text"], live[i].get("text", ""))), None)
        if hit is None:
            hit = next((i for i in range(len(live)) if i not in used and prefix_match(heading["text"], live[i].get("text", ""))), None)
        if hit is None:
            missing.append(heading["text"])
            continue
        used.add(hit)
        cursor = hit + 1
        matched += 1
        if int(live[hit].get("level", -1)) == int(heading.get("level", -1)):
            level_hits += 1
    recall = matched / len(truth)
    precision = matched / len(live) if live else 0.0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return HeadingScore(f1=f1, recall=recall, precision=precision,
                        level_exact=(level_hits / matched) if matched else None,
                        matched=matched, truth_total=len(truth), live_total=len(live), missing=tuple(missing))


@dataclass(frozen=True)
class OrderScore:
    found: float
    order: float
    at_start: float
    offsets: tuple[int | None, ...]
    first_break: str | None


def anchor_offsets(anchors: Sequence[str], text: str) -> tuple[int | None, ...]:
    """Character offset per anchor in the normalized full reading text: the first
    hit after the previous anchor's hit, else the first hit anywhere (so a moved
    paragraph is a break, not a miss)."""
    haystack = normalize(text)
    offsets: list[int | None] = []
    cursor = 0
    for anchor in anchors:
        needle = normalize(anchor)
        hit = haystack.find(needle, cursor) if needle else -1
        if hit < 0 and needle:
            hit = haystack.find(needle)
        offsets.append(hit if hit >= 0 else None)
        if hit >= 0:
            cursor = hit + len(needle)
    return tuple(offsets)


def anchor_positions(anchors: Sequence[str], entries: Sequence[dict[str, Any]]) -> tuple[int | None, ...]:
    """Reading-entry index per anchor (the entry whose 60-character prefix holds
    it), same forward-then-anywhere rule as ``anchor_offsets``."""
    positions: list[int | None] = []
    cursor = 0
    for anchor in anchors:
        hit = find_entry(anchor, entries, cursor)
        if hit is None:
            hit = find_entry(anchor, entries)
        positions.append(hit)
        if hit is not None:
            cursor = hit + 1
    return tuple(positions)


def reading_order_scores(anchors: Sequence[str], entries: Sequence[dict[str, Any]],
                         reading_text: str) -> OrderScore | None:
    """found = anchors present anywhere in the reading text; order = longest
    in-order run over the located ones; at_start = anchors that open a reading
    entry (a paragraph boundary the parser kept), reported, not gated."""
    if not anchors:
        return None
    offsets = anchor_offsets(anchors, reading_text)
    located = [(anchor, pos) for anchor, pos in zip(anchors, offsets) if pos is not None]
    found = len(located) / len(anchors)
    starts = sum(1 for p in anchor_positions(anchors, entries) if p is not None) / len(anchors)
    if len(located) <= 1:
        return OrderScore(found=found, order=1.0 if located else 0.0, at_start=starts, offsets=offsets, first_break=None)
    order = longest_increasing_run([pos for _, pos in located]) / len(located)
    first_break = next((anchor for (anchor, pos), (_, prev) in zip(located[1:], located) if pos <= prev), None)
    return OrderScore(found=found, order=order, at_start=starts, offsets=offsets, first_break=first_break)


@dataclass(frozen=True)
class TableScore:
    f1: float
    precision: float
    recall: float
    text_found: float
    matched_tables: int
    truth_tables: int


def _cell_map(table: dict[str, Any]) -> dict[tuple[int, int], str]:
    return {(int(c[0]), int(c[1])): normalize(c[4]) for c in table.get("cells", [])}


def _hits(truth_cells: Sequence[Sequence[Any]], cells: dict[tuple[int, int], str]) -> int:
    return sum(1 for r, c, text in truth_cells if cells.get((int(r), int(c))) == normalize(text))


def table_cell_scores(truth: Sequence[dict[str, Any]], live: Sequence[dict[str, Any]]) -> TableScore | None:
    """Cell F1 over the sampled truth cells, each truth table paired with the
    live table that hits most of its cells (each live table used once).

    A truth cell is a true positive when the live cell at that position has
    the same normalized text; a false positive plus a false negative when the
    live cell there says something else; a false negative when there is no
    cell. ``text_found`` is the share of truth cell texts present anywhere in
    the paired table, which separates a shifted grid from lost data.
    """
    if not truth:
        return None
    live_maps = [_cell_map(t) for t in live]
    free = set(range(len(live_maps)))
    tp = fp = fn = 0
    found_text = 0
    total = 0
    matched_tables = 0
    for table in truth:
        cells = list(table.get("cells", []))
        total += len(cells)
        best = max(free, key=lambda i: (_hits(cells, live_maps[i]), -i), default=None)
        if best is None or _hits(cells, live_maps[best]) == 0:
            fn += len(cells)
            continue
        matched_tables += 1
        free.discard(best)
        live_cells = live_maps[best]
        live_texts = set(live_cells.values())
        for r, c, text in cells:
            want = normalize(text)
            got = live_cells.get((int(r), int(c)))
            if got == want:
                tp += 1
            elif got:
                fp += 1
                fn += 1
            else:
                fn += 1
            if want in live_texts:
                found_text += 1
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return TableScore(f1=f1, precision=precision, recall=recall, text_found=(found_text / total) if total else 0.0,
                      matched_tables=matched_tables, truth_tables=len(truth))


@dataclass(frozen=True)
class FigureScore:
    placed: float
    captions: float | None
    total: int
    misplaced: tuple[str, ...]


def figure_scores(figures: Sequence[dict[str, Any]], anchors: Sequence[str],
                  entries: Sequence[dict[str, Any]]) -> FigureScore | None:
    """A figure is placed when a picture entry sits after the entry holding its
    ``after`` snippet and before the next located reading anchor (or the end).
    ``captions`` is the share of caption texts present in some entry."""
    if not figures:
        return None
    anchor_hits = sorted(p for p in anchor_positions(anchors, entries) if p is not None)
    picture_indexes = [i for i, e in enumerate(entries) if e.get("label") == "picture"]
    placed = 0
    caption_hits = 0
    captions_given = 0
    misplaced: list[str] = []
    for figure in figures:
        start = find_entry(figure["after"], entries)
        caption = figure.get("caption") or ""
        if caption:
            captions_given += 1
            caption_hits += 1 if find_entry(caption, entries) is not None else 0
        if start is None:
            misplaced.append(figure["after"])
            continue
        end = next((p for p in anchor_hits if p > start), len(entries))
        if any(start < i <= end for i in picture_indexes):
            placed += 1
        else:
            misplaced.append(figure["after"])
    return FigureScore(placed=placed / len(figures), captions=(caption_hits / captions_given) if captions_given else None,
                       total=len(figures), misplaced=tuple(misplaced))


@dataclass(frozen=True)
class TruthScores:
    """Every absolute metric for one document; values in [0, 1] or None when the truth is silent."""

    headings: HeadingScore | None
    order: OrderScore | None
    tables: TableScore | None
    figures: FigureScore | None

    def values(self) -> dict[str, float | None]:
        return {
            "truth_headings": self.headings.f1 if self.headings else None,
            "truth_heading_levels": self.headings.level_exact if self.headings else None,
            "truth_order": self.order.order if self.order else None,
            "truth_anchors_found": self.order.found if self.order else None,
            "truth_table_cells": self.tables.f1 if self.tables else None,
            "truth_figures": self.figures.placed if self.figures else None,
        }

    def details(self) -> dict[str, str]:
        out: dict[str, str] = {}
        if self.headings:
            h = self.headings
            out["truth_headings"] = f"{h.matched}/{h.truth_total} truth, {h.live_total} live"
            if h.missing:
                out["truth_headings"] += "; missing: " + "; ".join(h.missing[:3])
            out["truth_heading_levels"] = f"over {h.matched} matched"
        if self.order:
            o = self.order
            out["truth_anchors_found"] = (f"{sum(1 for p in o.offsets if p is not None)}/{len(o.offsets)} in text, "
                                          f"{o.at_start:.2f} open a paragraph")
            out["truth_order"] = f"first break at: {o.first_break}" if o.first_break else "in order"
        if self.tables:
            t = self.tables
            out["truth_table_cells"] = (f"p={t.precision:.3f} r={t.recall:.3f} text-anywhere={t.text_found:.3f} "
                                        f"tables {t.matched_tables}/{t.truth_tables}")
        if self.figures:
            f = self.figures
            out["truth_figures"] = f"{f.total} figures"
            if f.captions is not None:
                out["truth_figures"] += f", captions found {f.captions:.2f}"
            if f.misplaced:
                out["truth_figures"] += "; not after: " + "; ".join(f.misplaced[:2])
        return out


def score_truth(truth: dict[str, Any], summary: dict[str, Any]) -> TruthScores:
    entries = summary.get("reading", []) or []
    anchors = truth.get("anchors", []) or []
    return TruthScores(
        headings=heading_scores(truth.get("headings", []) or [], summary.get("headings", []) or []),
        order=reading_order_scores(anchors, entries, summary.get("reading_text", "") or ""),
        tables=table_cell_scores(truth.get("tables", []) or [], summary.get("tables", []) or []),
        figures=figure_scores(truth.get("figures", []) or [], anchors, entries),
    )
