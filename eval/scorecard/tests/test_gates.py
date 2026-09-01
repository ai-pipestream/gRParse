"""Unit tests for truth floors, the latency budget and the stability verdict."""

from __future__ import annotations

import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.gates import latency_result, median_ms, ratchet, stability_result, truth_results  # noqa: E402
from scorecard.scoring import PASS, REGRESS, SKIP  # noqa: E402


def test_truth_results_floor_tolerance() -> None:
    floors = {"truth_headings": 0.90, "truth_order": 0.95}
    results = {r.name: r for r in truth_results(
        {"truth_headings": 0.885, "truth_order": 0.92, "truth_figures": 0.5, "truth_table_cells": None}, floors)}
    assert results["truth_headings"].verdict == PASS, "0.015 below the floor is inside the tolerance"
    assert results["truth_order"].verdict == REGRESS and results["truth_order"].threshold == 0.93
    assert results["truth_figures"].verdict == PASS and "no floor" in results["truth_figures"].detail
    assert results["truth_table_cells"].verdict == SKIP


def test_latency_budget_ratio_and_slack() -> None:
    assert latency_result(1000.0, 1250.0).verdict == PASS
    assert latency_result(1000.0, 1501.0).verdict == REGRESS, "budget is max(1.25x, +500) = 1500"
    assert latency_result(100.0, 590.0).verdict == PASS and latency_result(100.0, 601.0).verdict == REGRESS
    assert latency_result(None, 300.0).verdict == SKIP
    assert latency_result(1000.0, 5000.0, enabled=False).verdict == SKIP


def test_stability_verdicts() -> None:
    assert stability_result(True, None, None).verdict == SKIP
    assert stability_result(None, False, "pictures[0]").verdict == PASS
    assert stability_result(True, True, None).verdict == PASS
    assert stability_result(False, False, "pictures[0].image.sha256").verdict == PASS
    flipped = stability_result(False, True, None)
    assert flipped.verdict == PASS and "re-record" in flipped.detail
    regressed = stability_result(True, False, "reading[3].hash")
    assert regressed.verdict == REGRESS and "reading[3].hash" in regressed.detail


def test_median_ms() -> None:
    assert median_ms([]) is None
    assert median_ms([300.0]) == 300.0
    assert median_ms([100.0, 900.0, 200.0]) == 200.0


def test_ratchet_seeds_raises_and_keeps_without_reason() -> None:
    row, changes = ratchet("d", None, {"truth_headings": 0.8, "truth_order": None}, latency_ms=120.0, stable=True, reason="")
    assert row == {"truth": {"truth_headings": 0.8}, "latency_ms": 120.0, "stable": True} and changes == []
    row, changes = ratchet("d", row, {"truth_headings": 0.9}, latency_ms=None, stable=None, reason="")
    assert row["truth"]["truth_headings"] == 0.9 and row["latency_ms"] == 120.0 and row["stable"] is True
    assert [c.reason for c in changes] == ["raised"]
    row, changes = ratchet("d", row, {"truth_headings": 0.7}, latency_ms=None, stable=None, reason="")
    assert row["truth"]["truth_headings"] == 0.9, "a lower score without a reason keeps the floor"
    assert changes[0].after == 0.9 and changes[0].reason.startswith("kept")


def test_ratchet_lowers_with_reason_and_tracks_stability_flip() -> None:
    row = {"truth": {"truth_headings": 0.9}, "latency_ms": 120.0, "stable": True}
    new, changes = ratchet("d", row, {"truth_headings": 0.7}, latency_ms=240.0, stable=False, reason="truth file corrected")
    assert new["truth"]["truth_headings"] == 0.7 and new["latency_ms"] == 240.0 and new["stable"] is False
    assert {(c.metric, c.before, c.after) for c in changes} == {("truth_headings", 0.9, 0.7), ("stable", True, False)}
    assert all(c.reason == "truth file corrected" for c in changes)
    assert row == {"truth": {"truth_headings": 0.9}, "latency_ms": 120.0, "stable": True}, "input not mutated"
