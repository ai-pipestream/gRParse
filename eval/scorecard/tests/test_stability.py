"""Unit tests for repeat-run comparison and the memory note."""

from __future__ import annotations

import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.memory import memory_note, parse_rss  # noqa: E402
from scorecard.stability import first_difference, stability  # noqa: E402


def test_first_difference_paths() -> None:
    a = {"counts": {"texts": 3}, "pictures": [{"image": {"sha256": "x"}}, {"image": {"sha256": "y"}}]}
    b = {"counts": {"texts": 3}, "pictures": [{"image": {"sha256": "x"}}, {"image": {"sha256": "z"}}]}
    assert first_difference(a, a) is None
    assert first_difference(a, b) == "pictures[1].image.sha256"
    assert first_difference({"a": [1, 2]}, {"a": [1]}) == "a[len 2 != 1]"
    assert first_difference({"a": 1}, {"b": 1}) == "a"
    assert first_difference(1, 2) == "<root>"


def test_stability_over_summaries() -> None:
    assert stability([{"x": 1}]) == (None, None)
    assert stability([{"x": 1}, {"x": 1}, {"x": 1}]) == (True, None)
    assert stability([{"x": 1}, {"x": 1}, {"x": 2}]) == (False, "x")


def test_parse_rss_and_note() -> None:
    text = "# HELP process_resident_memory_bytes Resident memory size in bytes.\nprocess_resident_memory_bytes 1.048576e+08\n"
    assert parse_rss(text) == 104857600
    assert parse_rss("grparse_pages_waiting 0\n") is None
    assert memory_note((None, "no metrics endpoint (set EVAL_METRICS_URL)"), (None, "")) == "memory: no metrics endpoint (set EVAL_METRICS_URL)"
    note = memory_note((100 * 2**20, "u"), (164 * 2**20, "u"))
    assert note == "memory: rss 100 MiB before, 164 MiB after (delta +64 MiB), informational"
