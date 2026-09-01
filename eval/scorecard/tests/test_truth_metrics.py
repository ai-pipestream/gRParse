"""Unit tests for the absolute (truth) metrics, all on hand-built inputs."""

from __future__ import annotations

import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.truth_metrics import (  # noqa: E402
    anchor_offsets, anchor_positions, figure_scores, find_entry, heading_scores, longest_increasing_run, normalize,
    prefix_match, reading_order_scores, score_truth, table_cell_scores,
)


def entry(label: str, text: str, level: int | None = None) -> dict:
    e = {"label": label, "text": text[:60]}
    if level is not None:
        e["level"] = level
    return e


def test_normalize_folds_quotes_dashes_case_and_space() -> None:
    assert normalize("  \u201cWhenever\u201d\u2014he\u00adsaid  ") == '"whenever"-hesaid'
    assert normalize(None) == ""


def test_prefix_match_either_direction_and_short_texts() -> None:
    assert prefix_match("1 Introduction", "1 INTRODUCTION and more words")
    assert prefix_match("1 Introduction and more", "1 Introduction")
    assert not prefix_match("Abstract", "Abs")
    assert prefix_match("II", "II") and not prefix_match("II", "III")


def test_find_entry_uses_start_and_substring() -> None:
    entries = [entry("text", "alpha beta"), entry("text", "gamma delta"), entry("text", "alpha again")]
    assert find_entry("alpha", entries) == 0
    assert find_entry("alpha", entries, start=1) == 2
    assert find_entry("omega", entries) is None
    assert find_entry("", entries) is None


def test_longest_increasing_run() -> None:
    assert longest_increasing_run([]) == 0
    assert longest_increasing_run([1, 2, 3]) == 3
    assert longest_increasing_run([3, 1, 2]) == 2
    assert longest_increasing_run([5, 5, 5]) == 1


def test_heading_scores_recall_precision_and_levels() -> None:
    truth = [{"text": "1 Introduction", "level": 1}, {"text": "1.1 Scope", "level": 2}, {"text": "2 Method", "level": 1}]
    live = [{"text": "1 INTRODUCTION", "level": 1}, {"text": "Anonymous authors", "level": 4}, {"text": "2 METHOD", "level": 2}]
    score = heading_scores(truth, live)
    assert score is not None
    assert score.matched == 2 and score.recall == 2 / 3 and score.precision == 2 / 3
    assert abs(score.f1 - 2 / 3) < 1e-9
    assert score.level_exact == 0.5, "2 Method matched at the wrong level"
    assert score.missing == ("1.1 Scope",)


def test_heading_scores_none_without_truth_and_zero_without_live() -> None:
    assert heading_scores([], [{"text": "x", "level": 1}]) is None
    score = heading_scores([{"text": "Only", "level": 1}], [])
    assert score is not None and score.f1 == 0.0 and score.level_exact is None


def test_anchor_positions_prefer_forward_hits() -> None:
    entries = [entry("text", "b second"), entry("text", "a first"), entry("text", "b second again")]
    assert anchor_positions(["a first", "b second"], entries) == (1, 2)
    assert anchor_positions(["b second", "a first"], entries) == (0, 1)


def test_anchor_offsets_prefer_forward_hits_in_text() -> None:
    text = "b second\na first\nb second again"
    assert anchor_offsets(["a first", "b second"], text) == (9, 17)
    assert anchor_offsets(["b second", "a first"], text) == (0, 9)
    assert anchor_offsets(["", "zzz"], text) == (None, None)


def text_of(entries: list[dict]) -> str:
    return "\n".join(e["text"] for e in entries if e["label"] != "picture")


def test_reading_order_in_order_and_swapped() -> None:
    entries = [entry("text", "one"), entry("text", "two"), entry("text", "three"), entry("text", "four")]
    good = reading_order_scores(["one", "two", "three", "four"], entries, text_of(entries))
    assert good is not None and good.found == 1.0 and good.order == 1.0 and good.first_break is None
    assert good.at_start == 1.0
    column_swap = reading_order_scores(["one", "three", "two", "four"], entries, text_of(entries))
    assert column_swap is not None and column_swap.order == 0.75 and column_swap.first_break == "two"


def test_reading_order_missing_anchor_counts_in_found_not_order() -> None:
    entries = [entry("text", "one"), entry("text", "two")]
    score = reading_order_scores(["one", "lost", "two"], entries, text_of(entries))
    assert score is not None and abs(score.found - 2 / 3) < 1e-9 and score.order == 1.0
    assert score.offsets == (0, None, 4)
    assert reading_order_scores([], entries, "") is None
    lone = reading_order_scores(["one"], entries, text_of(entries))
    assert lone is not None and lone.order == 1.0
    none_found = reading_order_scores(["zzz"], entries, text_of(entries))
    assert none_found is not None and none_found.found == 0.0 and none_found.order == 0.0


def test_reading_order_merged_paragraph_is_found_but_not_at_start() -> None:
    entries = [entry("text", "Heading text merged with the first words of the body paragraph that follows it")]
    text = "Heading text merged with the first words of the body paragraph that follows it"
    score = reading_order_scores(["Heading text", "the body paragraph that follows"], entries, text)
    assert score is not None and score.found == 1.0 and score.order == 1.0
    assert score.at_start == 0.5, "the second anchor is beyond the 60-character entry prefix"


def table(cells: list[tuple[int, int, str]]) -> dict:
    return {"cells": [[r, c, 1, 1, text] for r, c, text in cells]}


def test_table_cells_exact_wrong_and_missing() -> None:
    truth = [{"table": 0, "cells": [[0, 0, "Name"], [0, 1, "Role"], [1, 0, "Ada"], [2, 1, "Engine"]]}]
    live = [table([(0, 0, "Name"), (0, 1, "Role"), (1, 0, "Lovelace"), (1, 1, "Analyst")])]
    score = table_cell_scores(truth, live)
    assert score is not None
    # Name, Role exact; Ada wrong content (fp+fn); Engine missing (fn).
    assert score.precision == 2 / 3 and score.recall == 0.5
    assert score.text_found == 0.5 and score.matched_tables == 1


def test_table_cells_pairs_best_live_table_and_handles_none() -> None:
    truth = [{"table": "second", "cells": [[0, 0, "x"], [0, 1, "y"]]}]
    live = [table([(0, 0, "a")]), table([(0, 0, "x"), (0, 1, "y")])]
    score = table_cell_scores(truth, live)
    assert score is not None and score.f1 == 1.0
    assert table_cell_scores([], live) is None
    empty = table_cell_scores(truth, [])
    assert empty is not None and empty.f1 == 0.0 and empty.matched_tables == 0


def test_figure_placed_between_anchor_and_next_anchor() -> None:
    entries = [entry("text", "intro paragraph"), entry("picture", ""), entry("text", "Figure 1: caption"),
               entry("text", "next paragraph"), entry("picture", "")]
    anchors = ["intro paragraph", "next paragraph"]
    placed = figure_scores([{"after": "intro paragraph", "caption": "Figure 1"}], anchors, entries)
    assert placed is not None and placed.placed == 1.0 and placed.captions == 1.0 and placed.misplaced == ()
    late = figure_scores([{"after": "intro paragraph"}, {"after": "next paragraph"}],
                         ["intro paragraph", "next paragraph"],
                         [entry("text", "intro paragraph"), entry("text", "next paragraph"), entry("picture", ""), entry("picture", "")])
    assert late is not None and late.placed == 0.5 and late.misplaced == ("intro paragraph",)
    assert late.captions is None
    assert figure_scores([], anchors, entries) is None


def test_score_truth_values_and_details_shape() -> None:
    truth = {"headings": [{"text": "Title", "level": 0}], "anchors": ["first words", "second words"],
             "tables": [], "figures": [{"after": "first words", "caption": "Figure 1"}]}
    reading = [entry("title", "Title", 0), entry("text", "first words here"), entry("picture", ""),
               entry("text", "Figure 1: shown"), entry("text", "second words here")]
    summary = {"headings": [{"text": "Title", "level": 0}], "reading": reading, "reading_text": text_of(reading)}
    values = score_truth(truth, summary).values()
    assert values == {"truth_headings": 1.0, "truth_heading_levels": 1.0, "truth_order": 1.0,
                      "truth_anchors_found": 1.0, "truth_table_cells": None, "truth_figures": 1.0}
    details = score_truth(truth, summary).details()
    assert details["truth_anchors_found"] == "2/2 in text, 1.00 open a paragraph" and "truth_table_cells" not in details
