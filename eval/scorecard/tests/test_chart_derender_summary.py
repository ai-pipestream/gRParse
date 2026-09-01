"""Unit tests for the derendered-chart block in the summary and its stability rule.

The chart derender leg (GRPARSE_ENRICH_TARGET) folds a VLM-produced table
onto a raster chart picture with a GenerationSource. The summary exposes it
as ``pictures[i].derender`` so the baseline carries the cells, and the
stability comparison treats only the derendered title as descriptive: a
generative model may rephrase "Revenue by region" as "Revenue per region"
between two runs while reading the same bars, and that must not paint a
document red, whereas a different cell count or a different number is a
real instability and must.
"""

from __future__ import annotations

import sys
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from scorecard.stability import first_difference, stability  # noqa: E402
from scorecard.summary import summarize  # noqa: E402


def _cell(row: int, col: int, text: str, number: float | None = None) -> dict:
    cell = {"start_row_offset_idx": row, "end_row_offset_idx": row + 1, "start_col_offset_idx": col,
            "end_col_offset_idx": col + 1, "row_span": 1, "col_span": 1, "text": text}
    if number is not None:
        cell["value"] = {"number": number}
    return cell


def _document(title: str, second_value: str, *, derendered: bool = True) -> dict:
    picture = {
        "self_ref": "#/pictures/0", "parent": {"ref": "#/body"}, "label": "DOC_ITEM_LABEL_PICTURE",
        "prov": [{"page_no": 1}],
        "annotations": [
            {"classification": {"predicted_classes": [{"class_name": "bar_chart", "confidence": 0.9}]}},
            {"tabular_chart": {"kind": "tabular_chart_data", "title": title, "chart_data": {
                "num_rows": 3, "num_cols": 2,
                "table_cells": [_cell(0, 0, "Region"), _cell(0, 1, "Q1"), _cell(1, 0, "North"),
                                _cell(1, 1, "120", 120.0), _cell(2, 0, "South"), _cell(2, 1, second_value, float(second_value))]}}},
        ],
    }
    if derendered:
        picture["source"] = [{"generation": {"model": "fake-vlm", "endpoint": "http://vlm.test:8085"}}]
        picture["meta"] = {"tabular_chart": {"created_by": "fake-vlm", "title": title}}
    else:
        picture["source"] = [{"collector": {"collector": "libreoffice"}}]
    return {"body": {"self_ref": "#/body", "children": [{"ref": "#/pictures/0"}]}, "furniture": {"self_ref": "#/furniture"},
            "pictures": [picture], "origin": {"filename": "chart.png", "mimetype": "image/png"}}


def _summary(document: dict) -> dict:
    return summarize(document, "", doc_id="png-bar-chart", fmt="png", content_type="image/png",
                     status="CONVERSION_STATUS_SUCCESS", errors=[])


def test_summary_exposes_the_derendered_table_with_its_model() -> None:
    picture = _summary(_document("Revenue by region", "80"))["pictures"][0]
    block = picture["derender"]
    assert block["model"] == "fake-vlm"
    assert block["title"] == "Revenue by region"
    assert (block["rows"], block["cols"]) == (3, 2)
    assert block["cells"] == [[0, 0, 1, 1, "Region"], [0, 1, 1, 1, "Q1"], [1, 0, 1, 1, "North"],
                              [1, 1, 1, 1, "120"], [2, 0, 1, 1, "South"], [2, 1, 1, 1, "80"]]
    assert picture["classification"] == "bar_chart"


def test_office_chart_without_generation_source_has_no_derender_block() -> None:
    picture = _summary(_document("Revenue by region", "80", derendered=False))["pictures"][0]
    assert "derender" not in picture, "a live-model chart table is not a derender"


def test_rephrased_title_is_stable_but_a_different_number_is_not() -> None:
    first = _summary(_document("Revenue by region", "80"))
    rephrased = _summary(_document("Revenue per region", "80"))
    misread = _summary(_document("Revenue by region", "88"))
    assert first_difference(first, rephrased) is None, "the VLM title is descriptive"
    assert stability([first, rephrased]) == (True, None)
    stable, where = stability([first, misread])
    assert stable is False and where == "pictures[0].derender.cells[5][4]", where


def test_descriptive_rule_is_scoped_to_the_derender_title() -> None:
    assert first_difference({"pictures": [{"derender": {"title": "a"}}]}, {"pictures": [{"derender": {"title": "b"}}]}) is None
    assert first_difference({"pictures": [{"caption": "a"}]}, {"pictures": [{"caption": "b"}]}) == "pictures[0].caption"
    assert first_difference({"tables": [{"caption": "a"}]}, {"tables": [{"caption": "b"}]}) == "tables[0].caption"
    assert first_difference({"pictures": [{"derender": {"rows": 3}}]}, {"pictures": [{"derender": {"rows": 4}}]}) == "pictures[0].derender.rows"
