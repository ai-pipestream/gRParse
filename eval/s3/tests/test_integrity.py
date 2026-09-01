from s3.integrity import integrity_errors
from s3.tests.fixtures import sheet_document, word_document


def test_well_formed_documents_pass() -> None:
    assert integrity_errors(word_document()) == []
    assert integrity_errors(sheet_document()) == []


def test_dangling_child_and_missing_parent_listing() -> None:
    doc = word_document()
    doc["body"]["children"].append({"ref": "#/texts/99"})
    errors = integrity_errors(doc)
    assert any("child #/texts/99 of #/body does not resolve" in e for e in errors)
    doc = word_document()
    doc["texts"][2]["text"]["base"]["parent"] = {"ref": "#/texts/0"}
    errors = integrity_errors(doc)
    assert any("does not list #/texts/2 as a child" in e for e in errors)


def test_zero_page_and_duplicate_refs() -> None:
    doc = word_document()
    doc["texts"][1]["section_header"]["base"]["prov"][0]["page_no"] = 0
    assert any("not a 1-based page" in e for e in integrity_errors(doc))
    doc = word_document()
    doc["texts"][1]["section_header"]["base"]["self_ref"] = "#/texts/0"
    assert any("duplicate self_ref" in e for e in integrity_errors(doc))


def test_page_less_provenance_is_allowed() -> None:
    doc = word_document()
    doc["texts"][2]["text"]["base"]["prov"] = [{"grid": {"row": 1, "col": 2}}]
    assert integrity_errors(doc) == []


def test_span_and_anchor_targets_must_resolve() -> None:
    doc = word_document()
    doc["texts"][2]["text"]["base"]["spans"] = [{"range": {"start": 0, "end": 3}, "target": {"ref": "#/texts/40"}}]
    doc["anchors"] = [{"name": "bm", "target": {"ref": "#/pictures/9"}}]
    errors = integrity_errors(doc)
    assert any("span target" in e for e in errors) and any("anchor target" in e for e in errors)
