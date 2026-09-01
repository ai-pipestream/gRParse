"""Unit tests for the pure metric functions."""

from __future__ import annotations

import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard import metrics  # noqa: E402


def approx(a: float, b: float, eps: float = 1e-6) -> bool:
    return abs(a - b) < eps


def test_text_similarity_identical_and_empty() -> None:
    assert metrics.text_similarity("The quick brown fox", "The quick brown fox") == 1.0
    assert metrics.text_similarity("", "") == 1.0
    assert metrics.text_similarity("words here", "") == 0.0
    assert metrics.text_similarity("", "words here") == 0.0


def test_text_similarity_ignores_case_and_whitespace_layout() -> None:
    assert metrics.text_similarity("Hello,  World", "hello world") == 1.0


def test_text_similarity_drops_with_lost_words() -> None:
    base = " ".join(f"w{i}" for i in range(100))
    live = " ".join(f"w{i}" for i in range(90))
    value = metrics.text_similarity(base, live)
    assert 0.9 < value < 0.98, value


def test_levenshtein_basics() -> None:
    assert metrics.levenshtein([], []) == 0
    assert metrics.levenshtein([1, 2, 3], []) == 3
    assert metrics.levenshtein(["a", "b", "c"], ["a", "x", "c"]) == 1
    assert metrics.levenshtein(["a", "b", "c"], ["b", "c", "a"]) == 2


def test_sequence_similarity_normalizes_by_longest() -> None:
    assert metrics.sequence_similarity([], []) == 1.0
    assert approx(metrics.sequence_similarity([1, 2, 3, 4], [1, 2, 3]), 0.75)
    assert approx(metrics.sequence_similarity([1, 2], [3, 4]), 0.0)


def test_prefix_key_words_or_chars() -> None:
    assert metrics.prefix_key("The  Quick brown fox jumps over the lazy dog") == "the quick brown fox jumps over"
    assert metrics.prefix_key("Short title") == "short title"
    assert metrics.prefix_key("x" * 80) == "x" * 40, "few words: first 40 characters"
    assert metrics.prefix_key("") == ""
    assert metrics.prefix_key("a b c d e f g") == metrics.prefix_key("a b c d e f Z"), "the seventh word is not part of the key"


def _entry(label: str, text: str, ref: str = "") -> dict:
    return {"label": label, "text": text[:60], "hash": str(hash(text)), "ref": ref}


def test_reading_order_ignores_edits_inside_an_item() -> None:
    paragraph = "Diffusion models are continuous noise operators over the latent space of a trained encoder"
    a = [_entry("section_header", "Intro"), _entry("text", paragraph), _entry("text", "Second paragraph here"),
         _entry("table", "3x2")]
    edited = [a[0], _entry("text", paragraph.replace("latent space", "latent manifold")), a[2], a[3]]
    assert metrics.reading_order_similarity(a, edited) == 1.0
    assert metrics.text_similarity(paragraph, edited[1]["text"]) < 1.0, "the text metric still sees the edit"


def test_reading_order_penalizes_a_real_swap() -> None:
    a = [_entry("section_header", "Intro"), _entry("text", "First paragraph of the introduction section"),
         _entry("text", "Second paragraph of the introduction section"), _entry("table", "3x2")]
    swapped = [a[0], a[2], a[1], a[3]]
    value = metrics.reading_order_similarity(a, swapped)
    assert value < 0.95, value
    relabeled = [dict(a[0], label="text")] + a[1:]
    assert approx(metrics.reading_order_similarity(a, relabeled), 1 - 1 / 4)
    assert metrics.reading_order_similarity(a, a) == 1.0


def test_order_key_prefers_stored_key() -> None:
    assert metrics.order_key({"label": "text", "key": "stored key", "text": "other text"}) == ("text", "stored key")
    assert metrics.order_key({"label": "text", "text": "Derived from text"}) == ("text", "derived from text")


def _table(cells: list[tuple[int, int, str]], rows: int = 2, cols: int = 2, spans: dict | None = None) -> dict:
    spans = spans or {}
    return {"rows": rows, "cols": cols,
            "cells": [[r, c, *spans.get((r, c), (1, 1)), text] for r, c, text in cells]}


def test_table_cell_f1_exact_partial_and_absent() -> None:
    base = [_table([(0, 0, "a"), (0, 1, "b"), (1, 0, "c"), (1, 1, "d")])]
    assert metrics.table_cell_f1(base, base) == (1.0, 1.0, 1.0)
    assert metrics.table_cell_f1([], []) is None
    live = [_table([(0, 0, "a"), (0, 1, "b"), (1, 0, "c"), (1, 1, "X")])]
    p, r, f1 = metrics.table_cell_f1(base, live)
    assert approx(p, 0.75) and approx(r, 0.75) and approx(f1, 0.75)
    p, r, f1 = metrics.table_cell_f1(base, [])
    assert (p, r, f1) == (0.0, 0.0, 0.0)
    p, r, f1 = metrics.table_cell_f1([], base)
    assert (p, r, f1) == (0.0, 0.0, 0.0)


def test_table_cell_f1_is_per_table_index() -> None:
    t1 = _table([(0, 0, "a")])
    t2 = _table([(0, 0, "b")])
    assert metrics.table_cell_f1([t1, t2], [t1, t2]) == (1.0, 1.0, 1.0)
    p, r, f1 = metrics.table_cell_f1([t1, t2], [t2, t1])
    assert f1 == 0.0, "tables that swap places do not match by index"


def test_table_structure_match_rows_cols_spans() -> None:
    base = [_table([(0, 0, "a"), (1, 0, "b")], rows=2, cols=2, spans={(0, 0): (1, 2)})]
    same = [_table([(0, 0, "A"), (1, 0, "B")], rows=2, cols=2, spans={(0, 0): (1, 2)})]
    assert metrics.table_structure_match(base, same) == 1.0, "text differences do not affect structure"
    no_span = [_table([(0, 0, "a"), (1, 0, "b")], rows=2, cols=2)]
    assert metrics.table_structure_match(base, no_span) == 0.0
    wider = [_table([(0, 0, "a"), (1, 0, "b")], rows=2, cols=3, spans={(0, 0): (1, 2)})]
    assert metrics.table_structure_match(base, wider) == 0.0
    assert metrics.table_structure_match([], []) is None
    assert metrics.table_structure_match(base, base + base) == 0.5


def test_heading_score_levels_and_text() -> None:
    base = [{"level": 1, "text": "Intro"}, {"level": 2, "text": "Method"}, {"level": 2, "text": "Results"}]
    assert metrics.heading_score(base, base) == 1.0
    assert metrics.heading_score([], []) is None
    flat = [dict(h, level=1) for h in base]
    assert approx(metrics.heading_score(base, flat), 1 - 2 / 3)
    assert metrics.heading_score(base, []) == 0.0


def _picture(parent_label: str = "group:chapter", parent_name: str = "ch1", heading: str = "Intro", page: int | None = None) -> dict:
    return {"parent_label": parent_label, "parent_name": parent_name, "preceding_heading": heading, "page": page}


def test_picture_placement_score() -> None:
    base = [_picture(), _picture(parent_name="ch2", heading="Next")]
    assert metrics.picture_placement_score(base, base) == 1.0
    assert metrics.picture_placement_score([], []) is None
    moved = [_picture(), _picture(parent_name="ch1", heading="Next")]
    assert metrics.picture_placement_score(base, moved) == 0.5
    assert metrics.picture_placement_score(base, base[:1]) == 0.5
    assert metrics.picture_placement_score(base, list(reversed(base))) == 1.0, "arrival order is not placement"
    assert metrics.picture_placement_score(base, base + base[:1]) == 2 / 3, "a duplicate is an extra picture"
    assert metrics.picture_placement_score(base, [_picture(page=3), _picture(page=4)]) == 0.0


def test_warning_delta_is_a_multiset_difference() -> None:
    assert metrics.warning_delta([], []) == (0, [])
    assert metrics.warning_delta(["a", "b"], ["b"]) == (0, []), "fewer warnings is not a regression"
    assert metrics.warning_delta(["a"], ["a", "a", "c"]) == (2, ["a", "c"])


def test_agreement_score_tracks_winners() -> None:
    base = {"winners": {"origin.mimetype": "grparse", "source_meta.generator": "libreoffice"}}
    assert metrics.agreement_score(base, base) == 1.0
    assert metrics.agreement_score(None, base) is None
    assert metrics.agreement_score({"winners": {}}, base) is None
    flipped = {"winners": {"origin.mimetype": "grparse", "source_meta.generator": "grparse"}}
    assert metrics.agreement_score(base, flipped) == 0.5
    assert metrics.agreement_score(base, None) == 0.0
    grown = {"winners": dict(base["winners"], **{"source_meta.title": "xml"})}
    assert metrics.agreement_score(base, grown) == 1.0, "new fields do not lower the score"


def test_count_delta_only_scalar_ints() -> None:
    base = {"texts": 10, "tables": 1, "by_label": {"text": 10}}
    live = {"texts": 12, "tables": 0, "by_label": {"text": 12}, "extra": 5}
    assert metrics.count_delta(base, live) == {"texts": 2, "tables": -1}
