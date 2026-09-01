from s3.sourcefacts import source_facts
from s3.tests.fixtures import epub_bytes, zip_bytes


def test_html_headings_and_title() -> None:
    page = b"<html><head><title> My &amp; Page </title><script>var h='<h4>'</script></head><body><h1>A <em>b</em></h1><H2 id=x>B</H2><!-- <h3>c</h3> --></body></html>"
    facts = source_facts("html", "html", page)
    assert facts.headings == [(1, "A b"), (2, "B")] and facts.title == "My & Page" and facts.has_text
    bare = source_facts("html", "html", b"<html><head><title>T</title></head><body><div class=a></div></body></html>")
    assert bare.has_text is False and bare.title == "T"


def test_markdown_headings_skip_fences_and_read_setext() -> None:
    text = b"---\ntitle: Front Matter\n---\nTitle\n=====\n\n# One *bold* #\n\n```\n# not a heading\n```\n\n## [Two](x)\n\nSub\n---\n"
    facts = source_facts("md", "markdown", text)
    assert facts.headings == [(1, "Title"), (1, "One bold"), (2, "Two"), (2, "Sub")] and facts.title == "Front Matter"


def test_email_attachments() -> None:
    mail = (b"From: a@b\r\nContent-Type: multipart/mixed; boundary=x\r\n\r\n--x\r\n"
            b"Content-Disposition: attachment; filename=a.pdf\r\n\r\n--x\r\nContent-Disposition: inline\r\n\r\n--x--")
    facts = source_facts("eml", "email", mail)
    assert facts.attachments == 1 and facts.multipart


def test_epub_spine_from_opf() -> None:
    facts = source_facts("epub", "epub", epub_bytes())
    assert facts.spine == [("OEBPS/text/chap1.xhtml", "application/xhtml+xml"),
                           ("OEBPS/text/chap2.xhtml", "application/xhtml+xml")]
    assert not source_facts("epub", "epub", b"not a zip").ok


def test_office_zip_facts() -> None:
    deck = zip_bytes({"ppt/slides/slide1.xml": b"<p/>", "ppt/slides/slide2.xml": b"<p/>", "ppt/slides/_rels/slide1.xml.rels": b""})
    assert source_facts("pptx", "deck", deck).slides == 2
    word = zip_bytes({"word/document.xml": b"<w:body><w:drawing><pic:pic/></w:drawing><pic:pic/></w:body>"})
    assert source_facts("docx", "word", word).inline_pictures == 2
    workbook = (b'<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
                b'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
                b'<sheets><sheet name="Data" sheetId="1" r:id="rId1"/><sheet name="Blank" sheetId="2" r:id="rId2"/></sheets></workbook>')
    rels = (b'<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            b'<Relationship Id="rId1" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Target="worksheets/sheet2.xml"/></Relationships>')
    book = zip_bytes({"xl/workbook.xml": workbook, "xl/_rels/workbook.xml.rels": rels,
                      "xl/worksheets/sheet1.xml": b'<sheetData><row><c r="A1"><v>1</v></c></row></sheetData>',
                      "xl/worksheets/sheet2.xml": b"<sheetData/>"})
    assert source_facts("xlsx", "sheet", book).sheets == [("Data", True), ("Blank", False)]


def test_csv_grid() -> None:
    facts = source_facts("csv", "sheet", b"\xef\xbb\xbfa,b,c\n1,2,3\n\n,,\n4,5\n")
    assert (facts.csv_rows, facts.csv_cols) == (3, 3)


def test_unknown_family_reads_nothing() -> None:
    facts = source_facts("pdf", "pdf", b"%PDF-1.4")
    assert facts.ok and facts.headings is None
