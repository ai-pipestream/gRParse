#!/usr/bin/env python3
"""Regenerates the demo's bundled sample document (public/sample.pdf).

The sample is built to light up every feature the demo page renders:

- Page 1 is digital PDF text (title, paragraphs, list items, and a ruled
  table) authored as PostScript and converted with Ghostscript, so the
  service's native-text path and layout labelling run without OCR.
- Page 2 is a raster page composed with Pillow from the repository's test
  fixtures (bar chart and QR code), so OCR, figure classification, and
  barcode decoding all fire.

Host requirements: python3 with Pillow, Ghostscript (`gs`), and the DejaVu
fonts (stock on Ubuntu). Run from anywhere:

    python3 examples/web-demo/tools/make_sample.py
"""

import pathlib
import shutil
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFont

REPO = pathlib.Path(__file__).resolve().parents[3]
OUT = REPO / "examples" / "web-demo" / "public" / "sample.pdf"
FONTS = pathlib.Path("/usr/share/fonts/truetype/dejavu")

PAGE1_PS = r"""%!PS-Adobe-3.0
%%Pages: 1
%%Page: 1 1
/Helvetica-Bold findfont 26 scalefont setfont
72 720 moveto (gRParse sample document) show

/Helvetica findfont 12 scalefont setfont
72 690 moveto (This page carries a digital text layer, so the service extracts it natively) show
72 674 moveto (and skips OCR entirely. Boxes drawn solid in the demo are digital text;) show
72 658 moveto (dashed boxes on the next page come from RapidOCR.) show

/Helvetica-Bold findfont 15 scalefont setfont
72 620 moveto (What the stream carries) show

/Helvetica findfont 12 scalefont setfont
90 596 moveto (- One page event per page, in page order, the moment each page finishes) show
90 580 moveto (- Provenance boxes and append-only UTF offsets for every text item) show
90 564 moveto (- Table cell grids, picture classifications, and decoded barcodes) show

/Helvetica-Bold findfont 15 scalefont setfont
72 522 moveto (Pipeline stages) show

% Ruled 4x3 table: header row plus three data rows.
1 setlinewidth
newpath
72 500 moveto 468 0 rlineto 0 -96 rlineto -468 0 rlineto closepath stroke
72 476 moveto 468 0 rlineto stroke
72 452 moveto 468 0 rlineto stroke
72 428 moveto 468 0 rlineto stroke
228 500 moveto 0 -96 rlineto stroke
384 500 moveto 0 -96 rlineto stroke

/Helvetica-Bold findfont 11 scalefont setfont
80 484 moveto (Stage) show
236 484 moveto (Backing) show
392 484 moveto (Bound) show
/Helvetica findfont 11 scalefont setfont
80 460 moveto (Render) show
236 460 moveto (Poppler) show
392 460 moveto (Render queue) show
80 436 moveto (Inference) show
236 436 moveto (ONNX Runtime) show
392 436 moveto (Session pool) show
80 412 moveto (Assembly) show
236 412 moveto (Arena protobuf) show
392 412 moveto (Page window) show

/Helvetica findfont 12 scalefont setfont
72 372 moveto (The next page is a scanned-style raster: no text layer, a figure, and a QR) show
72 356 moveto (code. Watch the OCR, picture, and barcode counters move when it lands.) show
showpage
%%EOF
"""


def build_page1(workdir: pathlib.Path) -> pathlib.Path:
    ps = workdir / "page1.ps"
    ps.write_text(PAGE1_PS)
    pdf = workdir / "page1.pdf"
    subprocess.run(
        ["gs", "-q", "-dBATCH", "-dNOPAUSE", "-sDEVICE=pdfwrite",
         "-sPAPERSIZE=letter", f"-sOutputFile={pdf}", str(ps)],
        check=True,
    )
    return pdf


def build_page2(workdir: pathlib.Path) -> pathlib.Path:
    # US letter at 150 DPI.
    page = Image.new("RGB", (1275, 1650), "white")
    draw = ImageDraw.Draw(page)
    bold = ImageFont.truetype(str(FONTS / "DejaVuSans-Bold.ttf"), 44)
    body = ImageFont.truetype(str(FONTS / "DejaVuSans.ttf"), 26)

    draw.text((120, 110), "Scanned page: OCR, figures, barcode", font=bold, fill="black")
    draw.text((120, 200), "This page has no digital text layer. Every line here reaches", font=body, fill="black")
    draw.text((120, 240), "the demo through RapidOCR, so its boxes render dashed.", font=body, fill="black")

    # Both figures sit alone in their own whitespace band with a caption below,
    # the shape PubLayNet layout detection expects of a figure region.
    chart = Image.open(REPO / "tests" / "data" / "bar_chart.png").resize((640, 480))
    page.paste(chart, ((page.width - chart.width) // 2, 340))
    draw.text((280, 850), "Figure 1: quarterly volume, classified by the figure model.", font=body, fill="black")

    qr = Image.open(REPO / "tests" / "data" / "qr_code.png").resize((380, 380))
    page.paste(qr, ((page.width - qr.width) // 2, 960))
    draw.text((200, 1380), "Figure 2: the QR payload decodes in-process via ZXing and rides", font=body, fill="black")
    draw.text((200, 1420), "the picture item as a barcode annotation.", font=body, fill="black")

    pdf = workdir / "page2.pdf"
    page.save(pdf, "PDF", resolution=150.0)
    return pdf


def main() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        workdir = pathlib.Path(tmp)
        page1 = build_page1(workdir)
        page2 = build_page2(workdir)
        # Ghostscript on Ubuntu is confined and may only write under /tmp;
        # assemble there and copy the result into the repo.
        merged = workdir / "sample.pdf"
        subprocess.run(
            ["gs", "-q", "-dBATCH", "-dNOPAUSE", "-sDEVICE=pdfwrite",
             f"-sOutputFile={merged}", str(page1), str(page2)],
            check=True,
        )
        shutil.copyfile(merged, OUT)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
