"""eval/preview/build.py: geometry, agreement, image split, and one offline
build over a fake gRParse client and a Docling JSON directory."""

from __future__ import annotations

import base64
import json
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

from preview import build  # noqa: E402

PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAwS2OUAAAAABJRU5ErkJggg==")


def page(width: float, height: float, image: bool = True) -> dict:
    entry = {"size": {"width": width, "height": height}, "page_no": 1}
    if image:
        entry["image"] = {"mimetype": "image/png", "uri": "data:image/png;base64," + base64.b64encode(PNG).decode()}
    return entry


def item(label: str, left: float, t: float, r: float, b: float, origin: str, text: str = "x") -> dict:
    return {"label": label, "text": text,
            "prov": [{"page_no": 1, "bbox": {"l": left, "t": t, "r": r, "b": b, "coord_origin": origin}}]}


def ours_doc() -> dict:
    # Pixels, top-left: a 1000 x 1000 page.
    return {"pages": {"1": page(1000, 1000)},
            "texts": [item("title", 100, 100, 900, 200, "TOPLEFT"), item("text", 100, 300, 900, 500, "TOPLEFT"),
                      item("page_footer", 400, 950, 600, 990, "TOPLEFT")],
            "tables": [], "pictures": []}


def docling_doc() -> dict:
    # Points, bottom-left: the same layout on a 500 x 500 page.
    return {"pages": {"1": page(500, 500)},
            "texts": [item("section_header", 50, 450, 450, 400, "BOTTOMLEFT"),
                      item("text", 50, 350, 450, 250, "BOTTOMLEFT")],
            "tables": [], "pictures": [item("picture", 10, 100, 100, 10, "BOTTOMLEFT")]}


def test_normalized_box_flips_bottom_left() -> None:
    box = build.normalized_box({"l": 50, "t": 450, "r": 450, "b": 400, "coord_origin": "BOTTOMLEFT"}, (500, 500))
    assert [round(v, 3) for v in box] == [0.1, 0.1, 0.9, 0.2]
    same = build.normalized_box({"l": 100, "t": 100, "r": 900, "b": 200, "coord_origin": "TOPLEFT"}, (1000, 1000))
    assert [round(v, 3) for v in same] == [0.1, 0.1, 0.9, 0.2]


def test_agreement_matches_across_units_and_origins() -> None:
    result = build.agreement(ours_doc(), docling_doc())
    assert result["same_label"] == 1  # text
    assert result["other_label"] == 1  # title vs section_header
    assert result["ours_only"] == 1  # page_footer
    assert result["docling_only"] == 1  # picture
    assert result["score"] == 0.25


def test_word_provs_merge_into_one_box_per_item() -> None:
    words = {"label": "text", "text": "two words",
             "prov": [{"page_no": 1, "bbox": {"l": 100, "t": 300, "r": 480, "b": 500, "coord_origin": "TOPLEFT"}},
                      {"page_no": 1, "bbox": {"l": 520, "t": 300, "r": 900, "b": 500, "coord_origin": "TOPLEFT"}}]}
    ours = ours_doc()
    ours["texts"][1] = words
    result = build.agreement(ours, docling_doc())
    assert result["same_label"] == 1
    assert result["ours_only"] == 1  # still just the page_footer
    assert build.boxes_by_page(ours)[1][1] == ("text", (0.1, 0.3, 0.9, 0.5))


def test_split_images_writes_files_and_rewrites_uri() -> None:
    doc = ours_doc()
    with tempfile.TemporaryDirectory() as tmp:
        written = build.split_images(doc, Path(tmp), "ours")
        assert written == 1
        assert doc["pages"]["1"]["image"]["uri"] == "pages/ours-1.png"
        assert (Path(tmp) / "pages/ours-1.png").read_bytes() == PNG


class FakeClient:
    def __init__(self, document: dict) -> None:
        self.document = document

    def convert_bytes(self, data: bytes, name: str, **kwargs: object) -> SimpleNamespace:
        assert kwargs["include_page_images"] is True
        assert kwargs["formats"] == ("CANONICAL_JSON",)
        return SimpleNamespace(status="CONVERSION_STATUS_SUCCESS", elapsed_ms=12.0, rpc_error=None, errors=[],
                               exports={"canonical_json": json.dumps(self.document)})


def test_build_one_offline() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        pdfs, docling_dir, out = root / "pdfs", root / "docling", root / "out"
        (pdfs / "sub").mkdir(parents=True)
        (docling_dir / "sub").mkdir(parents=True)
        (pdfs / "sub" / "paper.pdf").write_bytes(b"%PDF-1.7 fake")
        (docling_dir / "sub" / "paper.json").write_text(json.dumps(docling_doc()))
        args = SimpleNamespace(out=out, pdf_dir=pdfs, force=False, timeout=5.0, docling_url=None,
                               docling_json_dir=docling_dir, docling_images_scale=1.0)
        entry = build.build_one(pdfs / "sub" / "paper.pdf", args, FakeClient(ours_doc()))
        doc_dir = out / "docs" / entry["id"]
        assert entry["ours"]["page_images"] == 1 and entry["docling"]["page_images"] == 1
        assert entry["ours"]["labels"] == {"page_footer": 1, "text": 1, "title": 1}
        assert entry["agreement"]["score"] == 0.25
        stored = json.loads((doc_dir / "ours.json").read_text())
        assert stored["pages"]["1"]["image"]["uri"] == "pages/ours-1.png"
        assert (doc_dir / "pages" / "docling-1.png").is_file()
        # A rerun reads the stored entry instead of converting again.
        again = build.build_one(pdfs / "sub" / "paper.pdf", args, None)
        assert again == json.loads((doc_dir / "meta.json").read_text())
        build.write_manifest(out, [entry], {"documents": 1})
        manifest = json.loads((out / "manifest.json").read_text())
        assert "pages" not in manifest["documents"][0]["agreement"]
