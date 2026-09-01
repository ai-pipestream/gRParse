"""Unit tests for the per-document change listing."""

from __future__ import annotations

import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.changes import REPORT_LINES, document_changes, has_changes, render_lines, sequence_changes  # noqa: E402


def _entry(label: str, text: str, ref: str) -> dict:
    return {"label": label, "text": text[:60], "hash": str(hash(text)), "ref": ref}


BASE = [_entry("section_header", "Intro", "#/texts/0"),
        _entry("text", "First paragraph of the introduction section", "#/texts/1"),
        _entry("text", "Second paragraph of the introduction section", "#/texts/2"),
        _entry("table", "3x2", "#/tables/0"),
        _entry("text", "Closing remarks for the section", "#/texts/3")]


def _refs(items: list[dict]) -> list[str]:
    return [i["ref"] for i in items]


def test_identical_sequences_have_no_changes() -> None:
    changes = sequence_changes(BASE, BASE)
    assert changes == {"deleted": [], "inserted": [], "moved": [], "edited": []}
    assert not has_changes(document_changes({"reading": BASE}, {"reading": BASE}, {"texts": 0}))


def test_edit_inside_item_is_edited_not_moved() -> None:
    live = list(BASE)
    live[1] = _entry("text", "First paragraph of the introduction section, now longer", "#/texts/1")
    changes = sequence_changes(BASE, live)
    assert _refs(changes["edited"]) == ["#/texts/1"]
    assert changes["moved"] == [] and changes["deleted"] == [] and changes["inserted"] == []


def test_swap_is_reported_as_moved_once() -> None:
    live = [BASE[0], BASE[2], BASE[1], BASE[3], BASE[4]]
    changes = sequence_changes(BASE, live)
    assert len(changes["moved"]) == 1 and changes["moved"][0]["ref"] in ("#/texts/1", "#/texts/2")
    assert changes["deleted"] == [] and changes["inserted"] == []


def test_delete_and_insert() -> None:
    live = [BASE[0], BASE[1], _entry("text", "A brand new paragraph", "#/texts/9"), BASE[3], BASE[4]]
    changes = sequence_changes(BASE, live)
    assert _refs(changes["deleted"]) == ["#/texts/2"] and _refs(changes["inserted"]) == ["#/texts/9"]
    assert changes["moved"] == []


def test_document_changes_covers_furniture_and_counts() -> None:
    base = {"reading": BASE, "furniture_reading": [_entry("text", "page 1", "#/texts/50")]}
    live = {"reading": BASE, "furniture_reading": []}
    changes = document_changes(base, live, {"texts": -1, "tables": 0})
    assert _refs(changes["furniture"]["deleted"]) == ["#/texts/50"]
    assert changes["count_delta"] == {"texts": -1} and changes["total"] == 1 and has_changes(changes)
    lines = render_lines(changes)
    assert lines == ['- furniture deleted: text "page 1" (#/texts/50)', "- counts: texts -1"]


def test_render_lines_caps_with_tail() -> None:
    many = [_entry("text", f"Paragraph number {i} in the corpus", f"#/texts/{i}") for i in range(30)]
    changes = document_changes({"reading": many}, {"reading": []}, {})
    lines = render_lines(changes)
    assert len(lines) == REPORT_LINES + 1 and lines[-1] == f"- ... {30 - REPORT_LINES} more"
    assert lines[0].startswith('- reading deleted: text "Paragraph number 0 in the corpus" (#/texts/0)')
