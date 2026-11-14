"""eval/preview/diff_catalog.py: text kinds, the known-structure bucket, themes and an offline catalog."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from preview import diff_catalog  # noqa: E402


def text_item(label: str, text: str, box: tuple[float, float, float, float], parent: str = "#/body") -> dict:
    left, top, right, bottom = box
    return {"label": label, "text": text, "parent": {"$ref": parent},
            "prov": [{"page_no": 1, "bbox": {"l": left, "t": top, "r": right, "b": bottom, "coord_origin": "TOPLEFT"}}]}


def doc(texts: list[dict], pictures: list[dict] | None = None) -> dict:
    return {"pages": {"1": {"size": {"width": 1000, "height": 1000}, "page_no": 1}},
            "texts": texts, "pictures": pictures or [], "tables": []}


def test_text_kind_names_the_normalisations() -> None:
    assert diff_catalog.text_kind("same", "same") is None
    assert diff_catalog.text_kind("nar￾ ratives", "nar ratives") == "text: differs only in U+FFFE soft-hyphen markers"
    assert diff_catalog.text_kind("CIKM ’26 07–11", "CIKM '26 07-11") == "text: differs only in quotes + dashes/minus"
    assert diff_catalog.text_kind("a b c", "c b a") == "text: same words, different order"
    assert diff_catalog.text_kind("x = 1", "") == "text: Docling text empty, gRParse has text"


def test_panel_caption_against_docling_picture_text_is_known_structure() -> None:
    figure = text_item("picture", "", (100, 100, 900, 600))
    ours = doc([text_item("caption", "(a) Data", (120, 610, 300, 630), "#/pictures/0")], [figure])
    docling = doc([text_item("text", "(a) Data", (120, 610, 300, 630), "#/pictures/0")], [dict(figure)])
    rows = {r["category"]: r["count"] for r in run_catalog(ours, docling)}
    assert rows.get("known structure: gRParse picture caption vs Docling text child of the picture") == 1
    assert not any(k.startswith("label:") for k in rows)


def test_a_body_caption_against_text_stays_a_label_difference() -> None:
    ours = doc([text_item("caption", "Figure 1: x", (100, 100, 900, 130))])
    docling = doc([text_item("text", "Figure 1: x", (100, 100, 900, 130))])
    rows = {r["category"]: r["count"] for r in run_catalog(ours, docling)}
    assert rows == {"label: gRParse caption vs Docling text": 1}


def test_themes_split_labels_from_known_structure() -> None:
    rows = [{"category": "label: gRParse title vs Docling section_header", "count": 3},
            {"category": "known structure: gRParse picture caption vs Docling text child of the picture", "count": 5}]
    counts = diff_catalog.theme_counts(rows)
    assert counts["7 Labels"] == 3 and counts["7b Known structure (panel captions)"] == 5 and counts["TOTAL"] == 8


def run_catalog(ours: dict, docling: dict) -> list[dict]:
    with tempfile.TemporaryDirectory() as name:
        root = Path(name)
        folder = root / "docs" / "abc"
        folder.mkdir(parents=True)
        (folder / "ours.json").write_text(json.dumps(ours))
        (folder / "docling.json").write_text(json.dumps(docling))
        (root / "manifest.json").write_text(json.dumps({"run": {}, "documents": [{"id": "abc", "path": "a.pdf"}]}))
        return diff_catalog.catalog(root)
