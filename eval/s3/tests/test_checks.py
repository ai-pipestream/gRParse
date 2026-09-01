"""Each check on a canned Document that passes it and on a broken twin that fails it."""

import copy

from s3.checks import CHECKS, mask_descriptive, run_checks
from s3.sourcefacts import SourceFacts
from s3.tests.fixtures import (
    CV,
    Builder,
    context,
    deck_document,
    email_document,
    epub_document,
    html_document,
    result,
    scan_document,
    sheet_document,
    word_document,
)


def verdicts(document, key, **kwargs):
    return run_checks(context(document, key, **kwargs))


def failing(document, key, **kwargs):
    checks, failures = verdicts(document, key, **kwargs)
    return {f.check: f for f in failures}, checks


def test_every_check_is_documented_once() -> None:
    names = [entry.name for entry in CHECKS]
    assert len(names) == len(set(names)) and all(entry.doc for entry in CHECKS)


def test_word_document_passes_its_battery() -> None:
    checks, failures = verdicts(word_document(), "grpc-libreoffice/fixtures/report.docx",
                                facts=SourceFacts(inline_pictures=1), runs=2)
    assert failures == [], failures
    assert checks["docx_pictures"] == "pass" and checks["reading_order"] == "pass" and checks["sheet_tables"] == "n/a"
    assert checks["repeat_identical"] == "pass" and checks["parse_succeeds"] == "pass"


def test_parse_failure_is_the_only_failure_without_a_document() -> None:
    ctx = context(None, "a/b.docx")
    ctx.runs[0].rpc_error = "FAILED_PRECONDITION: no collector"
    checks, failures = run_checks(ctx)
    assert [f.check for f in failures] == ["parse_succeeds"]
    assert checks["integrity"] == "n/a" and "rpc error" in failures[0].cause


def test_integrity_and_placement_failures() -> None:
    doc = word_document()
    doc["body"]["children"].append({"ref": "#/texts/77"})
    found, _ = failing(doc, "a.docx")
    assert "integrity" in found and "placement" in found
    doc = word_document()
    doc["furniture"]["children"].append({"ref": "#/texts/2"})
    found, _ = failing(doc, "a.docx")
    assert found["placement"].cause.startswith("item reachable from both")
    doc = word_document()
    doc["texts"].append({"text": {"base": {"self_ref": "#/texts/8", "parent": {"ref": "#/body"}, "text": "lost",
                                           "label": "DOC_ITEM_LABEL_TEXT"}}})
    found, _ = failing(doc, "a.docx")
    assert "neither" in found["placement"].cause


def test_custom_field_keys_and_warnings() -> None:
    doc = word_document()
    doc["body"]["meta"] = {"custom_fields": {"cell:?": "x", "collector_warnings:pdf": ["w"], "poi:title": "t"}}
    found, _ = failing(doc, "a.docx")
    assert "poi:title" in found["custom_field_keys"].cause
    assert found["warnings_typed"].evidence["keys"] == ["collector_warnings:pdf"]
    doc["body"]["meta"] = {"custom_fields": {"cell:?": "x", "markdown.title": "t"}}
    found, _ = failing(doc, "a.docx")
    assert "custom_field_keys" not in found and "warnings_typed" not in found


def test_claims_and_sources() -> None:
    doc = word_document()
    doc["claims"].append({"source": {"collector": "mystery"}})
    doc["origin"]["field_sources"].append({"field": "uri", "source": {"collector": "grparse"}})
    found, _ = failing(doc, "a.docx")
    assert "mystery" in found["claims_resolve"].cause or "uri" in str(found["claims_resolve"].evidence)
    doc = word_document()
    doc["texts"][2]["text"]["base"]["source"] = []
    found, _ = failing(doc, "a.docx")
    assert found["collector_sources"].evidence["refs"] == ["#/texts/2"]


def test_origin_mimetype_and_sniff() -> None:
    doc = word_document()
    doc["origin"]["mimetype"] = "text/plain"
    found, _ = failing(doc, "a.docx")
    assert "declares" in found["origin_mimetype"].cause
    good = word_document()
    sniffed = result(word_document())
    _, checks = failing(good, "a.docx", sniff=sniffed)
    assert checks["sniff_route"] == "pass"
    bad = result(None, status="RPC_ERROR", rpc_error="INVALID_ARGUMENT: not a raster")
    found, _ = failing(good, "a.docx", sniff=bad)
    assert "not routed by its bytes" in found["sniff_route"].cause
    by_name = result(word_document())
    by_name.document["origin"]["mimetype_evidence"] = "extension"
    found, _ = failing(good, "a.docx", sniff=by_name)
    assert "not magic" in found["sniff_route"].cause
    ole2 = word_document()
    ole2["origin"]["mimetype"] = "application/msword"
    _, checks = failing(ole2, "a.doc", sniff=bad)
    assert checks["sniff_route"] == "n/a", "an OLE2 container's bytes cannot name its format"


def test_pages_and_boxes() -> None:
    doc = word_document()
    del doc["pages"]["2"]
    found, _ = failing(doc, "a.docx")
    assert "page the document does not have" in found["page_count"].cause
    doc = word_document()
    doc["texts"][2]["text"]["base"]["prov"][0]["bbox"]["r"] = 99999
    found, _ = failing(doc, "a.docx")
    assert found["boxes_in_page"].evidence["count"] == 1
    doc = word_document()
    del doc["texts"][2]["text"]["base"]["prov"]
    found, _ = failing(doc, "a.docx")
    assert "#/texts/2" in found["provenance_present"].evidence["refs"][0]


def test_text_checks() -> None:
    bare = html_document()
    for item in bare["texts"]:
        next(iter(item.values()))["base"]["text"] = ""
    _, checks = failing(bare, "bare.html", facts=SourceFacts(has_text=False))
    assert checks["text_present"] == "n/a"
    found, _ = failing(bare, "bare.html", facts=SourceFacts(has_text=True))
    assert "text_present" in found
    doc = word_document()
    for item in doc["texts"]:
        next(iter(item.values()))["base"]["text"] = ""
    found, _ = failing(doc, "a.docx")
    assert "text_present" in found and found["empty_text_items"].evidence["count"] == len(doc["texts"])


def test_table_grids() -> None:
    doc = sheet_document()
    assert failing(doc, "q.xlsx")[0] == {}
    doc["tables"][0]["data"]["table_cells"][1]["col_span"] = 2
    doc["tables"][0]["data"]["table_cells"][1]["end_col_offset_idx"] = 3
    found, _ = failing(doc, "q.xlsx")
    assert "outside" in found["table_grids"].cause
    doc = sheet_document()
    doc["tables"][1]["data"]["grid"].pop()
    found, _ = failing(doc, "q.xlsx")
    assert "grid has" in found["table_grids"].cause
    doc = sheet_document()
    doc["tables"][0]["data"]["table_cells"].append({"start_row_offset_idx": 1, "start_col_offset_idx": 1,
                                                   "end_row_offset_idx": 2, "end_col_offset_idx": 2,
                                                   "row_span": 1, "col_span": 1, "text": "dup"})
    found, _ = failing(doc, "q.xlsx")
    assert "overlaps" in found["table_grids"].cause


def test_sheet_tables() -> None:
    doc = sheet_document()
    _, checks = failing(doc, "q.xlsx", facts=SourceFacts(sheets=[("Revenue", True), ("Empty", False)]))
    assert checks["sheet_tables"] == "pass" and checks["chart_composite"] == "pass"
    found, _ = failing(doc, "q.xlsx", facts=SourceFacts(sheets=[("Revenue", True), ("Costs", True)]))
    assert found["sheet_tables"].evidence["missing"] == ["Costs"]
    doc = sheet_document()
    for cell in doc["tables"][0]["data"]["table_cells"]:
        cell.pop("column_header", None)
    found, _ = failing(doc, "q.xlsx")
    assert "column_header" in found["sheet_tables"].cause
    doc = sheet_document()
    doc["groups"][0]["children"] = [c for c in doc["groups"][0]["children"] if not c["ref"].startswith("#/tables")]
    found, _ = failing(doc, "q.xlsx")
    assert any(f.cause == "sheet group without a table" for f in [found["sheet_tables"]])
    b = Builder("text/csv", "p.csv")
    b.page(1)
    sheet = b.group("SHEET", "p")
    b.table([(0, 0, 1, 1, "a"), (0, 1, 1, 1, "b"), (1, 0, 1, 1, "1"), (1, 1, 1, 1, "2")], 2, 2, sheet, page=1,
            box=(0, 0, 0, 0), header_rows=1)
    found, _ = failing(b.build(), "p.csv", facts=SourceFacts(csv_rows=3, csv_cols=2))
    assert found["sheet_tables"].evidence["source"] == "3x2"


def test_chart_composite() -> None:
    doc = sheet_document()
    doc["tables"][1]["parent"] = {"ref": "#/groups/0"}
    found, _ = failing(doc, "q.xlsx")
    assert "bound table" in found["chart_composite"].cause
    doc = sheet_document()
    doc["pictures"][0]["captions"].append({"ref": "#/texts/0"})
    found, _ = failing(doc, "q.xlsx")
    assert "caption" in found["chart_composite"].cause
    b = Builder("image/png", "c.png", collectors=())
    b.page(1, 800, 600)
    b.picture(page=1, box=(0, 0, 800, 600), source=CV + [{"generation": {"model": "vlm"}}])
    b.doc["pictures"][0]["annotations"] = [{"classification": {"predicted_classes": [{"class_name": "bar_chart"}]}}]
    found, _ = failing(b.build(), "c.png")
    assert "derendered chart" in found["chart_composite"].cause


def test_slides() -> None:
    doc = deck_document()
    _, checks = failing(doc, "d.pptx", facts=SourceFacts(slides=2))
    assert checks["slides"] == "pass" and checks["provenance_present"] == "pass"
    found, _ = failing(doc, "d.pptx", facts=SourceFacts(slides=3))
    assert "count" in found["slides"].cause
    doc = deck_document()
    doc["texts"][2]["section_header"] = doc["texts"][2].pop("section_header")
    doc["texts"][2] = {"title": {"base": doc["texts"][2]["section_header"]["base"]}}
    found, _ = failing(doc, "d.pptx")
    assert "more than once" in found["slides"].cause


def test_docx_pictures() -> None:
    doc = word_document()
    found, _ = failing(doc, "r.docx", facts=SourceFacts(inline_pictures=2))
    assert "fewer pictures" in found["docx_pictures"].cause
    b = Builder("application/msword", "x.doc")
    b.page(1).page(2)
    b.text("text", "second page first", page=2, box=(100, 100, 500, 200))
    b.picture(page=1, box=(100, 300, 500, 600))
    found, _ = failing(b.build(), "x.doc")
    assert "later page" in found["docx_pictures"].cause


def test_headings() -> None:
    doc = html_document()
    good = SourceFacts(headings=[(1, "Intro"), (2, "Details")], title="Page Title")
    assert failing(doc, "p.html", facts=good)[0] == {}
    found, _ = failing(doc, "p.html", facts=SourceFacts(headings=[(1, "Intro"), (3, "Details")], title="Page Title"))
    assert "level differs" in found["headings"].cause
    found, _ = failing(doc, "p.html", facts=SourceFacts(headings=[(1, "Intro"), (2, "Missing")], title="Page Title"))
    assert "missing or out of order" in found["headings"].cause
    found, _ = failing(doc, "p.html", facts=SourceFacts(headings=[(1, "Intro"), (2, "Details")], title="Other"))
    assert "neither the source title" in found["headings"].cause
    # an h1 promoted to the title is the source's first heading, not a mismatch
    promoted = SourceFacts(headings=[(1, "Page Title"), (1, "Intro"), (2, "Details")], title=None)
    assert failing(doc, "p.html", facts=promoted)[0] == {}
    doc["texts"][0] = {"text": {"base": doc["texts"][0]["title"]["base"]}}
    found, _ = failing(doc, "p.html", facts=good)
    assert "no title item" in found["headings"].cause


def test_email_shape() -> None:
    doc = email_document()
    assert failing(doc, "m.eml", facts=SourceFacts(attachments=1))[0] == {}
    found, _ = failing(doc, "m.eml", facts=SourceFacts(attachments=2))
    assert "fewer attachments" in found["email_shape"].cause
    doc = email_document()
    doc["attachments"][0]["item_ref"] = "#/texts/9"
    found, _ = failing(doc, "m.eml")
    assert "resolv" in found["email_shape"].cause
    doc = email_document()
    del doc["email"]
    found, _ = failing(doc, "m.eml")
    assert "EmailMeta" in found["email_shape"].cause


def test_epub_spine() -> None:
    doc = epub_document()
    spine = [("OEBPS/text/chap1.xhtml", "application/xhtml+xml"), ("OEBPS/text/chap2.xhtml", "application/xhtml+xml")]
    assert failing(doc, "b.epub", facts=SourceFacts(spine=spine))[0] == {}
    found, _ = failing(doc, "b.epub", facts=SourceFacts(spine=list(reversed(spine))))
    assert "spine order" in found["epub_spine"].cause
    doc["groups"][1]["children"] = []
    found, _ = failing(doc, "b.epub")
    assert "without content" in found["epub_spine"].cause


def test_ocr_text_and_reading_order() -> None:
    doc = scan_document()
    checks, failures = verdicts(doc, "s.png")
    assert failures == [] and checks["ocr_text"] == "pass" and checks["reading_order"] == "pass"
    doc = scan_document()
    doc["body"]["children"].reverse()
    found, _ = failing(doc, "s.png")
    assert found["reading_order"].evidence["count"] == 2
    doc = scan_document()
    doc["texts"][0]["text"]["base"]["prov"][0]["bbox"]["b"] = 5000
    found, _ = failing(doc, "s.png")
    assert "inside the page" in found["ocr_text"].cause
    b = Builder("image/png", "e.png", collectors=())
    b.page(1, 100, 100)
    found, _ = failing(b.build(), "e.png")
    assert "neither text nor a picture" in found["ocr_text"].cause
    # a flat page preview says the scan is blank, which is not a recognition miss
    import base64

    from s3.tests.fixtures import flat_png
    blank = Builder("application/pdf", "blank.pdf", collectors=())
    blank.page(1, 100, 100)
    blank.doc["pages"]["1"]["image"] = {"mimetype": "image/png",
                                       "uri": "data:image/png;base64," + base64.b64encode(flat_png(4, 3, (128, 128, 128))).decode()}
    _, checks = failing(blank.build(), "blank.pdf")
    assert checks["ocr_text"] == "pass"
    blank.doc["pages"]["1"]["image"]["uri"] = "data:image/png;base64," + base64.b64encode(flat_png(4, 3, (128, 128, 128), dot=True)).decode()
    found, _ = failing(blank.build(), "blank.pdf")
    assert found["ocr_text"].evidence["previews"] == "content"


def test_two_column_pages_are_skipped_by_reading_order() -> None:
    b = Builder("application/pdf", "two.pdf", collectors=())
    src = [{"collector": {"collector": "pdf"}}]
    b.page(1, 612, 792)
    b.text("text", "left top", page=1, box=(50, 50, 300, 100), source=src)
    b.text("text", "left bottom", page=1, box=(50, 500, 300, 550), source=src)
    b.text("text", "right top", page=1, box=(320, 50, 560, 100), source=src)
    _, checks = failing(b.build(), "two.pdf")
    assert checks["reading_order"] == "pass"


def test_repeat_identical_masks_only_the_derender_title() -> None:
    doc = scan_document()
    second = copy.deepcopy(doc)
    second["texts"][1]["text"]["base"]["text"] = "Recognised line TWO"
    found, _ = failing(doc, "s.png", runs=2, second=second)
    assert "repeat parse differs" in found["repeat_identical"].cause
    chart = {"pictures": [{"source": [{"generation": {"model": "m"}}],
                           "annotations": [{"tabular_chart": {"title": "A", "chart_data": {}}}],
                           "meta": {"tabular_chart": {"title": "A"}}}]}
    other = copy.deepcopy(chart)
    other["pictures"][0]["annotations"][0]["tabular_chart"]["title"] = "B"
    other["pictures"][0]["meta"]["tabular_chart"]["title"] = "B"
    assert mask_descriptive(chart) == mask_descriptive(other)
    assert mask_descriptive({"title": "keep"}) == {"title": "keep"}
