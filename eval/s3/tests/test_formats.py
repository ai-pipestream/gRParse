from s3.formats import acceptable_mimetypes, extension_of, family_of, parser_type, pdf_has_text_layer, strip_extension
from s3.tests.fixtures import Builder, word_document


def test_extension_of_handles_compound_and_case() -> None:
    assert extension_of("fastwarc-grpc/tests/data/a.warc.gz") == "warc.gz"
    assert extension_of("x/Report.PDF") == "pdf"
    assert extension_of("x/statement.layout.json") == "layout.json"
    assert extension_of("noext") == ""
    assert extension_of("dir.v2/noext") == ""


def test_strip_extension() -> None:
    assert strip_extension("a/b/report.docx") == "report"
    assert strip_extension("a.warc.gz") == "a"
    assert strip_extension("plain") == "plain"


def test_families() -> None:
    assert family_of("docx") == "word" and family_of("csv") == "sheet" and family_of("warc.gz") == "warc"
    assert family_of("ebc") == "ebcdic" and family_of("zzz") == "other"


def test_acceptable_mimetypes_include_aliases() -> None:
    assert "text/xml" in acceptable_mimetypes("xml") and "application/xml" in acceptable_mimetypes("xml")
    assert acceptable_mimetypes("zzz") == frozenset()


def test_parser_type_names_item_collectors() -> None:
    assert parser_type(word_document()) == "libreoffice"
    assert parser_type({}) == "none"
    b = Builder("application/pdf", "a.pdf", collectors=())
    b.text("text", "x", source=[{"collector": {"collector": "grparse", "model": "poppler-text"}}])
    b.text("text", "y", source=[{"collector": {"collector": "pdf"}}])
    assert parser_type(b.build()) == "grparse-cv+pdf"


def test_pdf_text_layer_detection() -> None:
    b = Builder("application/pdf", "a.pdf", collectors=())
    b.text("text", "x", source=[{"collector": {"collector": "grparse", "model": "rapidocr"}}])
    assert not pdf_has_text_layer(b.build())
    b.text("text", "y", source=[{"collector": {"collector": "grparse", "model": "poppler-text"}}])
    assert pdf_has_text_layer(b.build())
