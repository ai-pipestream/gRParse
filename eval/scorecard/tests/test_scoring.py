"""Unit tests for verdicts and the report table."""

from __future__ import annotations

import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.report import render_markdown  # noqa: E402
from scorecard.scoring import PASS, REGRESS, SKIP, score_document  # noqa: E402
from scorecard.tests.test_summary import _summary  # noqa: E402


def test_identical_summaries_pass_every_applicable_metric() -> None:
    base = _summary()
    score = score_document(base, base)
    assert score.verdict == PASS
    verdicts = {m.name: m.verdict for m in score.metrics}
    assert verdicts == {"text": PASS, "order": PASS, "table_cells": PASS, "table_structure": PASS, "headings": PASS,
                        "pictures": PASS, "warnings": PASS, "agreement": PASS}
    assert score.count_delta["texts"] == 0


def test_lost_table_regresses_only_table_metrics_and_verdict() -> None:
    base = _summary()
    live = dict(base, tables=[], reading=[e for e in base["reading"] if e["label"] != "table"],
                reading_text="\n".join(base["reading_text"].split("\n")[:3]))
    score = score_document(base, live)
    assert score.verdict == REGRESS
    by_name = {m.name: m for m in score.metrics}
    assert by_name["table_cells"].verdict == REGRESS and by_name["table_structure"].verdict == REGRESS
    assert by_name["headings"].verdict == PASS and by_name["pictures"].verdict == PASS


def test_new_warning_regresses_and_is_named() -> None:
    base = _summary()
    live = dict(base, warnings=base["warnings"] + ["error:layout:oops"])
    score = score_document(base, live)
    warning = next(m for m in score.metrics if m.name == "warnings")
    assert warning.verdict == REGRESS and warning.value == 1.0 and "error:layout:oops" in warning.detail


def test_structures_absent_on_both_sides_skip() -> None:
    base = _summary({})
    score = score_document(base, base)
    by_name = {m.name: m.verdict for m in score.metrics}
    assert by_name["table_cells"] == SKIP and by_name["headings"] == SKIP and by_name["pictures"] == SKIP
    assert by_name["agreement"] == SKIP and by_name["text"] == PASS and score.verdict == PASS


def test_render_markdown_one_row_per_document() -> None:
    base = _summary()
    score = score_document(base, base).as_dict()
    report = {"label": "t", "mode": "score", "target": "x:1", "service": "gRParse v", "baseline_dir": "b",
              "totals": {"scored": 1, "passed": 1, "regressed": 0, "skipped": 1, "no_baseline": 0, "recorded": 0},
              "wall_seconds": 0.5, "notes": ["n"],
              "documents": [{"doc_id": "t", "format": "epub", "status": "scored", "score": score, "summary": base, "elapsed_ms": 12.3},
                            {"doc_id": "missing", "format": "pdf", "status": "skipped", "reason": "external file missing: /x"}]}
    text = render_markdown(report)
    table = text.split("## Cross-collector agreement")[0]
    rows = [line for line in table.splitlines() if line.startswith("| t ") or line.startswith("| missing ")]
    assert len(rows) == 2
    assert "PASS" in rows[0] and "1.000 PASS" in rows[0] and rows[0].endswith("| PASS |")
    assert rows[1].endswith("| SKIP (external file missing: /x) |")
    assert "## Cross-collector agreement" in text and "| t | epub, grparse | 2 | 1 | 1 |" in text
