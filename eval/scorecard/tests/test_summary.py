"""Unit tests for the Document projection and the agreement section."""

from __future__ import annotations

import base64
import hashlib
import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.agreement import agreement_section, flatten  # noqa: E402
from scorecard.summary import normalize_text, summarize  # noqa: E402

PNG_BYTES = b"\x89PNG\r\n\x1a\nfake-image-bytes"


def _text(kind: str, text: str, parent: str = "#/body", level: int | None = None, children: list | None = None) -> dict:
    base = {"self_ref": "", "parent": {"ref": parent}, "label": "DOC_ITEM_LABEL_TEXT", "text": text}
    if children:
        base["children"] = [{"ref": c} for c in children]
    inner: dict = {"base": base}
    if level is not None:
        inner["level"] = level
    return {kind: inner}


def sample_document() -> dict:
    """A document exercising groups, headings, a spanning table, a picture under a text item, claims."""
    return {
        "origin": {"mimetype": "application/epub+zip", "filename": "book.epub",
                   "field_sources": [{"field": "mimetype", "source": {"collector": "grparse"}}]},
        "source_meta": {"title": "Book", "generator": "Pandoc",
                        "field_sources": [{"field": "generator", "source": {"collector": "epub"}}]},
        "body": {"children": [{"ref": "#/groups/0"}, {"ref": "#/texts/3"}, {"ref": "#/tables/0"}, {"ref": "#/texts/99"}]},
        "furniture": {"children": [{"ref": "#/texts/4"}]},
        "groups": [{"self_ref": "#/groups/0", "parent": {"ref": "#/body"}, "label": "GROUP_LABEL_CHAPTER", "name": "ch1",
                    "children": [{"ref": "#/texts/0"}, {"ref": "#/texts/1"}, {"ref": "#/texts/2"}]}],
        "texts": [
            _text("section_header", "  Chapter   One ", parent="#/groups/0", level=1),
            _text("text", "First paragraph.", parent="#/groups/0", children=["#/pictures/0"]),
            _text("section_header", "Deep", parent="#/groups/0", level=3),
            _text("text", "", parent="#/body"),
            _text("text", "page 1", parent="#/furniture"),
            _text("text", "orphan paragraph", parent="#/body"),
        ],
        "pictures": [{"self_ref": "#/pictures/0", "parent": {"ref": "#/texts/1"}, "label": "DOC_ITEM_LABEL_PICTURE",
                      "prov": [{"page_no": 2}],
                      "image": {"mimetype": "image/png", "uri": "data:image/png;base64," + base64.b64encode(PNG_BYTES).decode()},
                      "annotations": [{"classification": {"predicted_classes": [{"class_name": "chart"}]}}],
                      "source": [{"collector": {"collector": "epub"}}]},
                     {"self_ref": "#/pictures/1", "parent": {"ref": "#/body"}, "label": "DOC_ITEM_LABEL_PICTURE"}],
        "tables": [{"self_ref": "#/tables/0", "parent": {"ref": "#/body"}, "label": "DOC_ITEM_LABEL_TABLE",
                    "data": {"num_rows": 2, "num_cols": 2, "table_cells": [
                        {"text": "head", "col_span": 2, "end_col_offset_idx": 2, "end_row_offset_idx": 1, "column_header": True},
                        {"text": " a ", "start_row_offset_idx": 1, "end_row_offset_idx": 2, "end_col_offset_idx": 1},
                        {"text": "b", "start_row_offset_idx": 1, "start_col_offset_idx": 1, "end_row_offset_idx": 2, "end_col_offset_idx": 2},
                    ]}}],
        "claims": [
            {"source": {"collector": "grparse"}, "origin": {"mimetype": "application/epub+zip", "filename": "book.epub"}},
            {"source": {"collector": "epub"}, "origin": {"mimetype": "application/epub+zip", "filename": "other.epub"},
             "source_meta": {"title": "Book", "generator": "Pandoc"}},
        ],
    }


def _summary(document: dict | None = None, **overrides) -> dict:
    kwargs = dict(doc_id="t", fmt="epub", content_type="application/epub+zip", status="CONVERSION_STATUS_SUCCESS",
                  errors=[], rpc_error=None)
    kwargs.update(overrides)
    return summarize(document if document is not None else sample_document(), "# Chapter One\n\n| a | b |\n", **kwargs)


def test_normalize_text() -> None:
    assert normalize_text("  a \n\t b  ") == "a b"
    assert normalize_text(None) == ""


def test_reading_sequence_is_depth_first_and_labelled() -> None:
    s = _summary()
    labels = [e["label"] for e in s["reading"]]
    assert labels == ["group:chapter", "section_header", "text", "picture", "section_header", "text", "table"]
    assert s["reading"][0]["name"] == "ch1"
    assert s["reading"][1]["text"] == "Chapter One" and s["reading"][1]["level"] == 1
    assert s["reading_text"] == "Chapter One\nFirst paragraph.\nDeep\nhead\na\nb"


def test_headings_and_level_jump_warning() -> None:
    s = _summary()
    assert [(h["level"], h["text"]) for h in s["headings"]] == [(1, "Chapter One"), (3, "Deep")]
    assert "heading-level-jump:#/texts/2" in s["warnings"]


def test_table_grid_normalizes_spans_and_text() -> None:
    table = _summary()["tables"][0]
    assert (table["rows"], table["cols"], table["column_headers"]) == (2, 2, 1)
    assert table["cells"] == [[0, 0, 1, 2, "head"], [1, 0, 1, 1, "a"], [1, 1, 1, 1, "b"]]


def test_picture_placement_and_image_digest() -> None:
    pictures = _summary()["pictures"]
    assert len(pictures) == 1, "only placed pictures enter the reading projection"
    picture = pictures[0]
    assert picture["parent_label"] == "text" and picture["parent_name"] == "ch1"
    assert picture["preceding_heading"] == "Chapter One" and picture["page"] == 2
    assert picture["image"] == {"mimetype": "image/png", "bytes": len(PNG_BYTES), "sha256": hashlib.sha256(PNG_BYTES).hexdigest()}
    assert picture["classification"] == "chart" and picture["collectors"] == ["epub"]
    assert "base64" not in repr(_summary()), "no raw image payload survives into the summary"


def test_warnings_cover_dangling_orphans_empty_and_status() -> None:
    s = _summary(status="CONVERSION_STATUS_PARTIAL_SUCCESS", errors=[{"module": "layout", "message": "boom  now"}])
    for expected in ("status:CONVERSION_STATUS_PARTIAL_SUCCESS", "error:layout:boom now", "dangling-ref:#/texts/99",
                     "orphan:#/pictures/1", "orphan:#/texts/5", "picture-unplaced:#/pictures/1", "empty-text:#/texts/3"):
        assert expected in s["warnings"], (expected, s["warnings"])
    assert not any(w.startswith("mimetype-mismatch") for w in s["warnings"])
    mismatch = _summary(content_type="text/html")
    assert "mimetype-mismatch:text/html!=application/epub+zip" in mismatch["warnings"]


def test_counts_groups_and_furniture() -> None:
    s = _summary()
    counts = s["counts"]
    assert counts["texts"] == 6 and counts["tables"] == 1 and counts["pictures"] == 2 and counts["groups"] == 1
    assert counts["body_items"] == 7 and counts["furniture_items"] == 1 and counts["orphans"] == 2
    assert counts["by_label"] == {"group:chapter": 1, "picture": 1, "section_header": 2, "table": 1, "text": 2}
    assert s["groups"] == [{"ref": "#/groups/0", "label": "chapter", "name": "ch1", "depth": 1, "children": 3}]
    assert s["furniture_reading"][0]["text"] == "page 1"
    assert s["markdown"]["headings"] == 1 and s["markdown"]["table_rows"] == 1
    assert s["collectors"] == ["epub", "grparse"]


def test_empty_document_is_flagged_not_crashed() -> None:
    s = _summary({}, rpc_error="UNKNOWN: boom")
    assert s["reading"] == [] and s["tables"] == [] and s["agreement"] is None
    assert "rpc-error:UNKNOWN: boom" in s["warnings"] and "no-document" in s["warnings"]


def test_summary_is_deterministic() -> None:
    assert _summary() == _summary()


def test_flatten_paths_and_lists() -> None:
    leaves = flatten({"a": {"b": 1, "c": [1, 2]}, "field_sources": [{"x": 1}], "d": None}, "root")
    assert leaves == {"root.a.b": 1, "root.a.c": "[1, 2]"}


def test_agreement_section_shared_agreed_conflicts_winners() -> None:
    section = agreement_section(sample_document())
    assert section["collectors"] == ["epub", "grparse"]
    assert section["claimed_fields"] == {"epub": 4, "grparse": 2}
    assert section["shared"] == 2 and section["agreed"] == 1
    assert section["conflicts"] == [{"field": "origin.filename", "values": {"epub": "other.epub", "grparse": "book.epub"}, "winner": ""}]
    assert section["winners"] == {"origin.mimetype": "grparse", "source_meta.generator": "epub"}
    assert agreement_section({"claims": []}) is None
