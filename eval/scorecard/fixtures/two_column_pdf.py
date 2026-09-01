#!/usr/bin/env python3
"""Generate ``two-column.pdf``: a two-column reading-order fixture from Gatsby prose.

    uv run python eval/scorecard/fixtures/two_column_pdf.py [out_dir]

The script writes a Flat ODT (``two-column.fodt``, kept next to this script so
the layout is inspectable as XML) and converts it with ``soffice --headless``.
The layout is chosen to trip every classic PDF reading-order failure:

- a two-line title over the full page width, then a byline;
- a two-column section with numbered headings (1., 1.1, 1.2, 2., 2.1);
- a running header on every page and "Page N" in the footer;
- two footnotes anchored in the body text;
- justified 10pt text with hyphenation on, so lines end in hyphens.

Prose is ``gatsby_excerpt.txt`` (public domain, chapter I and the start of
chapter II of The Great Gatsby, Project Gutenberg transcription). The output
is deterministic apart from the PDF metadata dates LibreOffice stamps.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from xml.sax.saxutils import escape

HERE = Path(__file__).resolve().parent
DEFAULT_OUT = HERE.parents[2] / "tests" / "golden" / "corpus"
CHAPTER_MARK = "@@CHAPTER II"

TITLE = "The Great Gatsby, Chapter One, Reset in Two Columns with Footnotes and a Running Header"
BYLINE = "F. Scott Fitzgerald (1925). Public domain text from the Project Gutenberg transcription."
HEADER = "The Great Gatsby, a two-column reading-order fixture"

# (heading text, outline level, first paragraph index, last paragraph index inclusive)
SECTIONS = (
    ("1. West Egg", 1, 0, 19),
    ("1.1 The Buchanan House", 2, 20, 51),
    ("1.2 Dinner at the Buchanans", 2, 52, 103),
    ("2. The Valley of Ashes", 1, 105, 112),
    ("2.1 The Garage on the Edge of the Waste Land", 2, 113, 120),
)
FOOTNOTES = {
    2: "The narrator gives his family history as fact within the novel; the Dukes of Buccleuch are a real Scottish title.",
    105: "The valley of ashes is usually identified with the Corona ash dumps in Queens, levelled for the 1939 World's Fair.",
}

NAMESPACES = (
    'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
    'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
    'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
    'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
    'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
    'office:version="1.3" office:mimetype="application/vnd.oasis.opendocument.text"'
)

STYLES = """
 <office:font-face-decls>
  <style:font-face style:name="Liberation Serif" svg:font-family="'Liberation Serif'" style:font-family-generic="roman"/>
 </office:font-face-decls>
 <office:styles>
  <style:default-style style:family="paragraph">
   <style:paragraph-properties fo:hyphenation-ladder-count="no-limit"/>
   <style:text-properties style:font-name="Liberation Serif" fo:font-size="10pt" fo:language="en" fo:country="US"
    fo:hyphenate="true" fo:hyphenation-remain-char-count="2" fo:hyphenation-push-char-count="2"/>
  </style:default-style>
  <style:style style:name="Standard" style:family="paragraph"/>
  <style:style style:name="Body" style:family="paragraph" style:parent-style-name="Standard">
   <style:paragraph-properties fo:text-align="justify" fo:margin-bottom="0.07in" fo:text-indent="0.15in"/>
   <style:text-properties fo:hyphenate="true" fo:hyphenation-remain-char-count="2" fo:hyphenation-push-char-count="2"/>
  </style:style>
  <style:style style:name="Title" style:family="paragraph" style:parent-style-name="Standard">
   <style:paragraph-properties fo:text-align="center" fo:margin-bottom="0.1in"/>
   <style:text-properties fo:font-size="20pt" fo:font-weight="bold"/>
  </style:style>
  <style:style style:name="Byline" style:family="paragraph" style:parent-style-name="Standard">
   <style:paragraph-properties fo:text-align="center" fo:margin-bottom="0.25in"/>
   <style:text-properties fo:font-size="10pt" fo:font-style="italic"/>
  </style:style>
  <style:style style:name="Heading_20_1" style:display-name="Heading 1" style:family="paragraph"
   style:parent-style-name="Standard" style:default-outline-level="1">
   <style:paragraph-properties fo:margin-top="0.16in" fo:margin-bottom="0.06in" fo:keep-with-next="always"/>
   <style:text-properties fo:font-size="13pt" fo:font-weight="bold"/>
  </style:style>
  <style:style style:name="Heading_20_2" style:display-name="Heading 2" style:family="paragraph"
   style:parent-style-name="Standard" style:default-outline-level="2">
   <style:paragraph-properties fo:margin-top="0.12in" fo:margin-bottom="0.05in" fo:keep-with-next="always"/>
   <style:text-properties fo:font-size="11pt" fo:font-weight="bold" fo:font-style="italic"/>
  </style:style>
  <style:style style:name="Header" style:family="paragraph" style:parent-style-name="Standard">
   <style:paragraph-properties fo:text-align="center"/>
   <style:text-properties fo:font-size="8pt" fo:font-variant="small-caps"/>
  </style:style>
  <style:style style:name="Footer" style:family="paragraph" style:parent-style-name="Standard">
   <style:paragraph-properties fo:text-align="center"/>
   <style:text-properties fo:font-size="8pt"/>
  </style:style>
  <style:style style:name="Footnote" style:family="paragraph" style:parent-style-name="Standard">
   <style:paragraph-properties fo:margin-left="0.15in" fo:text-indent="-0.15in"/>
   <style:text-properties fo:font-size="8pt"/>
  </style:style>
  <text:notes-configuration text:note-class="footnote" style:num-format="1" text:start-value="0"
   text:footnotes-position="page" text:start-numbering-at="document"/>
 </office:styles>
 <office:automatic-styles>
  <style:page-layout style:name="pm1">
   <style:page-layout-properties fo:page-width="8.5in" fo:page-height="11in" fo:margin-top="0.6in"
    fo:margin-bottom="0.6in" fo:margin-left="0.8in" fo:margin-right="0.8in"/>
   <style:header-style><style:header-footer-properties fo:min-height="0.25in" fo:margin-bottom="0.15in"/></style:header-style>
   <style:footer-style><style:header-footer-properties fo:min-height="0.25in" fo:margin-top="0.15in"/></style:footer-style>
  </style:page-layout>
  <style:style style:name="TwoColumns" style:family="section">
   <style:section-properties text:dont-balance-text-columns="true">
    <style:columns fo:column-count="2" fo:column-gap="0.3in"/>
   </style:section-properties>
  </style:style>
 </office:automatic-styles>
 <office:master-styles>
  <style:master-page style:name="Standard" style:page-layout-name="pm1">
   <style:header><text:p text:style-name="Header">{header}</text:p></style:header>
   <style:footer><text:p text:style-name="Footer">Page <text:page-number text:select-page="current">1</text:page-number></text:p></style:footer>
  </style:master-page>
 </office:master-styles>
"""


def load_paragraphs(path: Path = HERE / "gatsby_excerpt.txt") -> list[str]:
    return [" ".join(p.split()) for p in path.read_text(encoding="utf-8").split("\n\n") if p.strip()]


def footnote(number: int, text: str) -> str:
    return (f'<text:note text:id="ftn{number}" text:note-class="footnote"><text:note-citation>{number}'
            f'</text:note-citation><text:note-body><text:p text:style-name="Footnote">{escape(text)}</text:p>'
            f"</text:note-body></text:note>")


def body_paragraph(index: int, text: str, note_numbers: dict[int, int]) -> str:
    content = escape(text)
    if index in FOOTNOTES:
        # Anchor the note after the first sentence so it sits mid-paragraph.
        head, sep, tail = content.partition(". ")
        note = footnote(note_numbers[index], FOOTNOTES[index])
        content = f"{head}.{note} {tail}" if sep else f"{content}{note}"
    return f'  <text:p text:style-name="Body">{content}</text:p>'


def render(paragraphs: list[str]) -> str:
    note_numbers = {index: n for n, index in enumerate(sorted(FOOTNOTES), start=1)}
    lines = ['<?xml version="1.0" encoding="UTF-8"?>', f"<office:document {NAMESPACES}>",
             STYLES.replace("{header}", escape(HEADER)), " <office:body>", "  <office:text>",
             f'  <text:p text:style-name="Title">{escape(TITLE)}</text:p>',
             f'  <text:p text:style-name="Byline">{escape(BYLINE)}</text:p>',
             '  <text:section text:style-name="TwoColumns" text:name="Body">']
    for heading, level, first, last in SECTIONS:
        lines.append(f'  <text:h text:style-name="Heading_20_{level}" text:outline-level="{level}">{escape(heading)}</text:h>')
        for index in range(first, last + 1):
            if paragraphs[index] == CHAPTER_MARK:
                continue
            lines.append(body_paragraph(index, paragraphs[index], note_numbers))
    lines.extend(["  </text:section>", "  </office:text>", " </office:body>", "</office:document>", ""])
    return "\n".join(lines)


def convert_to_pdf(fodt: Path, out_dir: Path) -> Path:
    subprocess.run(["soffice", "--headless", "--convert-to", "pdf", "--outdir", str(out_dir), str(fodt)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    pdf = out_dir / (fodt.stem + ".pdf")
    if not pdf.is_file():
        raise SystemExit(f"soffice produced no {pdf}")
    return pdf


def main(argv: list[str]) -> int:
    out_dir = Path(argv[0]) if argv else DEFAULT_OUT
    out_dir.mkdir(parents=True, exist_ok=True)
    paragraphs = load_paragraphs()
    fodt = HERE / "two-column.fodt"
    fodt.write_text(render(paragraphs), encoding="utf-8")
    pdf = convert_to_pdf(fodt, out_dir)
    print(f"{pdf} ({pdf.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
