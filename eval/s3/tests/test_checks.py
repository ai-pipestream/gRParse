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
    prov,
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
    for cell in b.doc["tables"][-1]["data"]["table_cells"][2:]:
        cell["value"] = {"number": float(cell["text"])}
    found, _ = failing(b.build(), "p.csv", facts=SourceFacts(csv_rows=3, csv_cols=2))
    assert found["sheet_tables"].evidence["source"] == "3x2"


def _sheet_with(cells: list[tuple], rows: int, cols: int, header_rows: tuple[int, ...] = ()) -> dict:
    """One sheet table from (row, col, text) or (row, col, text, number) cells; header_rows get column_header."""
    b = Builder("application/vnd.ms-excel", "r.xls")
    b.page(1)
    sheet = b.group("SHEET", "r")
    ref = b.table([(spec[0], spec[1], 1, 1, spec[2]) for spec in cells], rows, cols, sheet,
                  page=1, box=(0, 0, 0, 0))
    table_cells = b.doc["tables"][int(ref.rsplit("/", 1)[1])]["data"]["table_cells"]
    for spec, cell in zip(cells, table_cells):
        if len(spec) > 3:
            cell["value"] = {"number": spec[3]}
        if cell["start_row_offset_idx"] in header_rows:
            cell["column_header"] = True
    return b.build()


def test_sheet_header_under_preamble() -> None:
    """A title and a run line above the header: the label line nearest the
    data is the one that must be marked, and row 0 is left alone."""
    cells = [(0, 0, "A3131"), (0, 1, "Expenditure Over Threshold"), (2, 0, "RUN AT 2/1/2016"),
             (4, 0, "Entity"), (4, 1, "Supplier"), (4, 2, "Amount"),
             (5, 0, "NHS"), (5, 1, "NEMS"), (5, 2, "99,761.56", 99761.56),
             (6, 0, "NHS"), (6, 1, "BMI"), (6, 2, "30,697.43", 30697.43)]
    _, checks = failing(_sheet_with(cells, 7, 3, header_rows=(4,)), "r.xls")
    assert checks["sheet_tables"] == "pass"
    found, _ = failing(_sheet_with(cells, 7, 3), "r.xls")
    assert "row 4" in found["sheet_tables"].evidence["tables"][0]
    found, _ = failing(_sheet_with(cells, 7, 3, header_rows=(0, 4)), "r.xls")
    assert "row 0" in found["sheet_tables"].evidence["tables"][0], "a title marked as a header is wrong too"


def test_sheet_header_band_two_lines() -> None:
    """A group line over a leaf line is one band: both lines must be marked, one alone fails."""
    cells = [(0, 0, "Figure 5"), (2, 0, "Age"), (2, 1, "Males"), (2, 2, "Females"),
             (3, 0, "x"), (3, 1, "lx"), (3, 2, "exo"), (3, 3, "lx"),
             (4, 0, "0", 0.0), (4, 1, "100000", 100000.0), (4, 2, "69.5", 69.5), (4, 3, "100000", 100000.0),
             (5, 0, "1", 1.0), (5, 1, "98808", 98808.0), (5, 2, "69.4", 69.4), (5, 3, "99099", 99099.0)]
    _, checks = failing(_sheet_with(cells, 6, 4, header_rows=(2, 3)), "r.xls")
    assert checks["sheet_tables"] == "pass"
    found, _ = failing(_sheet_with(cells, 6, 4, header_rows=(3,)), "r.xls")
    assert "rows 2, 3" in found["sheet_tables"].evidence["tables"][0]


def test_sheet_header_plain_and_absent() -> None:
    """A plain sheet is still held to its row-0 header; a numeric first row and a record
    without its number demand nothing; an all-text table demands its first row."""
    plain = [(0, 0, "Region"), (0, 1, "Q1"), (1, 0, "North"), (1, 1, "12", 12.0), (2, 0, "South"), (2, 1, "9", 9.0)]
    found, _ = failing(_sheet_with(plain, 3, 2), "r.xls")
    assert "row 0" in found["sheet_tables"].evidence["tables"][0]
    _, checks = failing(_sheet_with(plain, 3, 2, header_rows=(0,)), "r.xls")
    assert checks["sheet_tables"] == "pass"
    numeric = [(0, 0, "1", 1.0), (0, 1, "2", 2.0), (1, 0, "3", 3.0), (1, 1, "4", 4.0)]
    _, checks = failing(_sheet_with(numeric, 2, 2), "r.xls")
    assert checks["sheet_tables"] == "pass"
    found, _ = failing(_sheet_with(numeric, 2, 2, header_rows=(0,)), "r.xls")
    assert "no label band" in found["sheet_tables"].evidence["tables"][0]
    register = [(0, 0, "Director"), (0, 1, "Contract"), (0, 2, "Supplier"), (0, 3, "Value"),
                (1, 0, "M. Pocock"), (1, 1, "To be tendered"), (1, 2, "Muse"),
                (2, 0, "M. Pocock"), (2, 1, "C005032"), (2, 2, "Muse"), (2, 3, "8000000", 8000000.0),
                (3, 0, "M. Pocock"), (3, 1, "C005033"), (3, 2, "Herman"), (3, 3, "900000", 900000.0)]
    _, checks = failing(_sheet_with(register, 4, 4, header_rows=(0,)), "r.xls")
    assert checks["sheet_tables"] == "pass"
    found, _ = failing(_sheet_with(register, 4, 4, header_rows=(0, 1)), "r.xls")
    assert "row 1" in found["sheet_tables"].evidence["tables"][0], "the record without its number is not a header line"
    text = [(0, 0, "Country"), (0, 1, "Institution"), (1, 0, "Austria"), (1, 1, "Institute"),
            (2, 0, "Belgium"), (2, 1, "Institute"), (3, 0, "Bosnia"), (3, 1, "University")]
    found, _ = failing(_sheet_with(text, 4, 2), "r.xls")
    assert "row 0" in found["sheet_tables"].evidence["tables"][0]
    contents = [(0, 0, "JH"), (0, 1, "Contents"), (3, 0, "VT"), (3, 1, "Table 2"), (3, 2, "Breaks"),
                (5, 0, "VT"), (5, 1, "Table 4"), (5, 2, "Summary")]
    _, checks = failing(_sheet_with(contents, 6, 3), "r.xls")
    assert checks["sheet_tables"] == "pass"


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


def test_docx_pictures_do_not_follow_footnotes() -> None:
    b = Builder("application/msword", "f.doc")
    b.page(1).page(2)
    b.text("text", "body", page=1, box=(100, 100, 500, 200))
    b.text("text", "a note", page=2, box=(100, 100, 500, 200), label="FOOTNOTE")
    b.picture(page=1, box=(100, 300, 500, 600))
    _, checks = failing(b.build(), "f.doc")
    assert checks["docx_pictures"] == "pass"


def test_continued_items_fill_a_column_for_reading_order() -> None:
    b = Builder("application/msword", "s.doc")
    b.page(1).page(2)
    long = b.text("text", "left column, from page one", page=1, box=(100, 9000, 5000, 15000))
    b.node(long)["prov"].append(prov(2, 100, 1000, 5000, 2500))
    b.text("text", "left, lower", page=2, box=(100, 3000, 5000, 3300))
    b.text("text", "left, lowest", page=2, box=(100, 3400, 5000, 3700))
    b.text("text", "right column top", page=2, box=(6000, 1000, 11000, 1300))
    _, checks = failing(b.build(), "s.doc")
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
