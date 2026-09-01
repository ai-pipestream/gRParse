"""Integrity of the truth layer, the baselines and the gate rows they feed.

Every truth file names a document the corpus holds, validates against a strict
schema (no unknown keys, integer levels, level 0 only for a title, figures
that point at an anchor the file itself lists), every corpus document has a
recorded baseline and no baseline describes a document that left the corpus,
and every truth floor in ``baseline/_meta.json`` is a score the recorded
baseline still reproduces. That last one is the anti-drift check with teeth:
the floors are recomputed here from the committed baselines, offline, so a
floor that was raised above what was ever recorded cannot hide until the next
service run.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.corpus import REPO, load_manifest  # noqa: E402
from scorecard.truth_metrics import score_truth  # noqa: E402

TRUTH_DIR = REPO / "eval" / "scorecard" / "truth"
BASELINE_DIR = REPO / "eval" / "scorecard" / "baseline"
META = BASELINE_DIR / "_meta.json"

TRUTH_KEYS = frozenset({"doc_id", "source", "notes", "headings", "anchors", "tables", "figures"})
REQUIRED_TRUTH_KEYS = frozenset({"doc_id", "source", "headings", "anchors", "tables", "figures"})
HEADING_KEYS = frozenset({"text", "level"})
TABLE_KEYS = frozenset({"table", "cells"})
FIGURE_KEYS = frozenset({"after", "caption"})
# A recomputed score may differ from the pinned floor only by JSON rounding.
FLOOR_EPSILON = 1e-4


def _truth_files() -> list[Path]:
    return sorted(TRUTH_DIR.glob("*.json"))


def _load(path: Path) -> dict:
    return json.loads(path.read_text())


def _meta() -> dict:
    return _load(META)


def test_truth_ids_exist_in_the_corpus() -> None:
    known = {doc.doc_id for doc in load_manifest()}
    stray = [path.name for path in _truth_files() if path.stem not in known]
    assert not stray, f"truth files for documents corpus.json does not list: {stray}"


def test_truth_file_names_match_their_doc_id() -> None:
    mismatched = [path.name for path in _truth_files() if _load(path).get("doc_id") != path.stem]
    assert not mismatched, f"truth files whose doc_id is not their filename: {mismatched}"


def test_truth_files_validate_against_the_strict_schema() -> None:
    problems: list[str] = []
    for path in _truth_files():
        truth = _load(path)
        name = path.name
        unknown = sorted(set(truth) - TRUTH_KEYS)
        if unknown:
            problems.append(f"{name}: unknown keys {unknown}")
        for key in sorted(REQUIRED_TRUTH_KEYS - set(truth)):
            problems.append(f"{name}: missing key '{key}'")
        for index, heading in enumerate(truth.get("headings", [])):
            if set(heading) != HEADING_KEYS:
                problems.append(f"{name}: headings[{index}] keys {sorted(heading)}")
                continue
            level = heading["level"]
            if not isinstance(level, int) or isinstance(level, bool):
                problems.append(f"{name}: headings[{index}] level {level!r} is not an int")
            elif not 0 <= level <= 6:
                problems.append(f"{name}: headings[{index}] level {level} is outside 0..6")
            if not isinstance(heading["text"], str) or not heading["text"].strip():
                problems.append(f"{name}: headings[{index}] has no text")
        for index, anchor in enumerate(truth.get("anchors", [])):
            if not isinstance(anchor, str) or not anchor.strip():
                problems.append(f"{name}: anchors[{index}] is not a non-empty string")
        for index, table in enumerate(truth.get("tables", [])):
            if set(table) != TABLE_KEYS:
                problems.append(f"{name}: tables[{index}] keys {sorted(table)}")
                continue
            for cell_index, cell in enumerate(table["cells"]):
                if len(cell) != 3 or not isinstance(cell[0], int) or not isinstance(cell[1], int):
                    problems.append(f"{name}: tables[{index}].cells[{cell_index}] is not [row, col, text]")
                elif cell[0] < 0 or cell[1] < 0:
                    problems.append(f"{name}: tables[{index}].cells[{cell_index}] has a negative offset")
                elif not isinstance(cell[2], str):
                    problems.append(f"{name}: tables[{index}].cells[{cell_index}] text is not a string")
        for index, figure in enumerate(truth.get("figures", [])):
            if not set(figure) <= FIGURE_KEYS or "after" not in figure:
                problems.append(f"{name}: figures[{index}] keys {sorted(figure)}")
    assert not problems, "truth files that do not validate:\n  " + "\n  ".join(problems)


def test_only_the_first_heading_may_be_a_title() -> None:
    problems: list[str] = []
    for path in _truth_files():
        levels = [heading["level"] for heading in _load(path).get("headings", [])]
        titles = [index for index, level in enumerate(levels) if level == 0]
        if len(titles) > 1:
            problems.append(f"{path.name}: level 0 is the title; found {len(titles)} of them")
        elif titles and titles[0] != 0:
            problems.append(f"{path.name}: the title is at index {titles[0]}, not the head")
    assert not problems, "\n  ".join(problems)


def test_figure_anchors_reference_an_anchor_the_file_lists() -> None:
    problems: list[str] = []
    for path in _truth_files():
        truth = _load(path)
        anchors = set(truth.get("anchors", []))
        for index, figure in enumerate(truth.get("figures", [])):
            if figure["after"] not in anchors:
                problems.append(f"{path.name}: figures[{index}].after {figure['after']!r} is not one of the anchors")
    assert not problems, (
        "a figure is placed relative to a located anchor, so its 'after' has to be an anchor:\n  "
        + "\n  ".join(problems)
    )


def test_every_corpus_document_has_a_baseline() -> None:
    recorded = {path.stem for path in BASELINE_DIR.glob("*.json") if path.stem != "_meta"}
    wanted = {doc.doc_id for doc in load_manifest()}
    assert not wanted - recorded, f"corpus documents with no recorded baseline: {sorted(wanted - recorded)}"


def test_the_baseline_has_no_entries_for_removed_documents() -> None:
    recorded = {path.stem for path in BASELINE_DIR.glob("*.json") if path.stem != "_meta"}
    wanted = {doc.doc_id for doc in load_manifest()}
    assert not recorded - wanted, (
        f"baselines for documents corpus.json no longer lists: {sorted(recorded - wanted)}; "
        "delete them with the manifest entry"
    )


def test_meta_documents_and_gates_cover_the_corpus() -> None:
    meta = _meta()
    wanted = {doc.doc_id for doc in load_manifest()}
    described = set(meta.get("documents", []))
    gated = set(meta.get("gates", {}))
    assert described == wanted, (
        f"_meta.json describes {sorted(described - wanted)} that left the corpus and misses "
        f"{sorted(wanted - described)}"
    )
    assert gated == wanted, (
        f"gate rows for {sorted(gated - wanted)} that left the corpus, none for {sorted(wanted - gated)}"
    )


def test_every_truth_floor_names_a_document_with_a_truth_file() -> None:
    truth_ids = {path.stem for path in _truth_files()}
    floored = {doc for doc, row in _meta().get("gates", {}).items() if row.get("truth")}
    assert not floored - truth_ids, (
        f"truth floors for documents with no truth file: {sorted(floored - truth_ids)}"
    )


def test_no_truth_floor_sits_above_the_recorded_baseline() -> None:
    """Recompute each floored metric from the committed baseline and truth file.

    ``score_truth`` is a pure function of the two, so the score the last record
    saw is reproducible offline. A floor above it would gate every future run
    against a number the parser was never observed to reach.
    """
    problems: list[str] = []
    for doc_id, row in sorted(_meta().get("gates", {}).items()):
        floors = row.get("truth") or {}
        if not floors:
            continue
        truth_path = TRUTH_DIR / f"{doc_id}.json"
        baseline_path = BASELINE_DIR / f"{doc_id}.json"
        if not truth_path.is_file() or not baseline_path.is_file():
            problems.append(f"{doc_id}: floors recorded without both a truth file and a baseline")
            continue
        recomputed = score_truth(_load(truth_path), _load(baseline_path)).values()
        for metric, floor in sorted(floors.items()):
            value = recomputed.get(metric)
            if value is None:
                problems.append(f"{doc_id}.{metric}: floor {floor} but the baseline scores nothing")
            elif round(value, 4) < floor - FLOOR_EPSILON:
                problems.append(f"{doc_id}.{metric}: floor {floor} above the recorded {round(value, 4)}")
    assert not problems, (
        "truth floors that no recorded baseline reaches; re-record the document or lower the "
        "floor with --reason:\n  " + "\n  ".join(problems)
    )


def test_gate_rows_carry_a_latency_budget_and_a_stability_verdict() -> None:
    missing = [doc for doc, row in sorted(_meta().get("gates", {}).items())
               if "latency_ms" not in row or "stable" not in row]
    assert not missing, f"gate rows without a latency budget or stability verdict: {missing}"


def test_every_partial_re_record_states_a_reason() -> None:
    history = _meta().get("history", [])
    assert history, "_meta.json records no history"
    unreasoned = [entry["timestamp"] for entry in history
                  if entry.get("partial") and not entry.get("reason")]
    assert not unreasoned, f"partial re-records with no reason: {unreasoned}"
