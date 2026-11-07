#!/usr/bin/env python3
"""Generate the per-format fixtures under ``tests/golden/corpus/formats``.

    uv run --with olefile python tests/golden/build_format_fixtures.py [out_dir]

One small synthetic fixture for each ``InputFormat`` the scorecard corpus
had none of (ai-pipestream/gRParse#257), plus ``table-picture.pdf``, the
Docling parity fixture that carries a table and a picture. Every byte is
written by this script or by a tool it runs on text this script wrote:
nothing is downloaded and nothing is copied from another document.

Needs LibreOffice (``soffice``) for the office formats and the PDF, and
``ffmpeg`` for the audio and video clips, and ``olefile`` (BSD) to read
back the .ppt LibreOffice writes; everything else is the Python standard
library. LibreOffice and ffmpeg stamp creation times into their
output, so a rerun moves those files' bytes; the content stays the same.

After a rerun, update ``tests/golden/corpus_manifest.textproto`` if a
fixture was added, renamed or removed (``golden-corpus-test`` fails until
it agrees with the directory), and say in the commit why the bytes moved.

Several fixtures carry only what the sniff needs (the iWork containers and
the AFP stream): no free generator writes those formats, so the manifest
marks them ``header_only`` and no full golden is possible for them.
"""

from __future__ import annotations

import base64
import gzip
import io
import json
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_OUT = HERE / "corpus" / "formats"
FIXED_ZIP_TIME = (2000, 1, 1, 0, 0, 0)
FIXED_MTIME = 946684800  # 2000-01-01T00:00:00Z

PROSE = (
    "This fixture was written for the gRParse golden corpus. It holds one heading, "
    "one short paragraph and one small table, so a parser has a little of each to read."
)
TABLE = (("Item", "Count", "Colour"), ("Apples", "3", "Red"), ("Pears", "5", "Green"))


# ---- tiny raster ------------------------------------------------------------

def png_bars(width: int = 96, height: int = 64) -> bytes:
    """A small RGB PNG of three coloured bars on white, drawn pixel by pixel."""
    bars = ((10, 30, 48, (43, 108, 176)), (38, 58, 24, (192, 86, 33)), (66, 86, 36, (56, 161, 105)))
    rows = []
    for y in range(height):
        row = bytearray([0])  # filter: none
        for x in range(width):
            colour = (255, 255, 255)
            for left, right, top, rgb in bars:
                if left <= x < right and y >= height - top - 4 and y < height - 4:
                    colour = rgb
            row.extend(colour)
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 9)

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", raw) + chunk(b"IEND", b"")


# ---- flat OpenDocument sources, converted by LibreOffice ---------------------

ODF_NS = (
    'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
    'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
    'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
    'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
    'xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" '
    'xmlns:presentation="urn:oasis:names:tc:opendocument:xmlns:presentation:1.0" '
    'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
    'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
    'xmlns:xlink="http://www.w3.org/1999/xlink"'
)


def writer_table() -> str:
    rows = []
    for row in TABLE:
        cells = "".join(
            f'<table:table-cell office:value-type="string"><text:p>{cell}</text:p></table:table-cell>'
            for cell in row)
        rows.append(f"<table:table-row>{cells}</table:table-row>")
    return ('<table:table table:name="Fruit"><table:table-column table:number-columns-repeated="3"/>'
            + "".join(rows) + "</table:table>")


def fodt(title: str, with_picture: bool) -> str:
    picture = ""
    if with_picture:
        data = base64.b64encode(png_bars()).decode("ascii")
        picture = (
            '<text:p text:style-name="Standard"><draw:frame draw:name="Bars" text:anchor-type="as-char" '
            'svg:width="2.4in" svg:height="1.6in"><draw:image><office:binary-data>'
            f"{data}</office:binary-data></draw:image></draw:frame></text:p>"
            '<text:p text:style-name="Standard">Figure 1: Three coloured bars drawn for this fixture.</text:p>'
        )
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<office:document {ODF_NS} office:version="1.3" '
        'office:mimetype="application/vnd.oasis.opendocument.text">'
        "<office:styles>"
        '<style:style style:name="Standard" style:family="paragraph"/>'
        '<style:style style:name="Heading_20_1" style:display-name="Heading 1" style:family="paragraph" '
        'style:default-outline-level="1"><style:text-properties fo:font-size="16pt" fo:font-weight="bold"/>'
        "</style:style></office:styles>"
        "<office:body><office:text>"
        f'<text:h text:style-name="Heading_20_1" text:outline-level="1">{title}</text:h>'
        f'<text:p text:style-name="Standard">{PROSE}</text:p>'
        f"{writer_table()}{picture}"
        "</office:text></office:body></office:document>\n"
    )


def fods() -> str:
    rows = []
    for row in TABLE:
        cells = []
        for cell in row:
            if cell.isdigit():
                cells.append(f'<table:table-cell office:value-type="float" office:value="{cell}">'
                             f"<text:p>{cell}</text:p></table:table-cell>")
            else:
                cells.append(f'<table:table-cell office:value-type="string"><text:p>{cell}</text:p>'
                             "</table:table-cell>")
        rows.append("<table:table-row>" + "".join(cells) + "</table:table-row>")
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<office:document {ODF_NS} office:version="1.3" '
        'office:mimetype="application/vnd.oasis.opendocument.spreadsheet">'
        "<office:body><office:spreadsheet>"
        '<table:table table:name="Fruit"><table:table-column table:number-columns-repeated="3"/>'
        + "".join(rows) + "</table:table></office:spreadsheet></office:body></office:document>\n"
    )


def fodp() -> str:
    def slide(name: str, title: str, body: str) -> str:
        return (
            f'<draw:page draw:name="{name}">'
            '<draw:frame presentation:class="title" svg:x="1cm" svg:y="1cm" svg:width="24cm" svg:height="3cm">'
            f"<draw:text-box><text:p>{title}</text:p></draw:text-box></draw:frame>"
            '<draw:frame presentation:class="outline" svg:x="1cm" svg:y="5cm" svg:width="24cm" svg:height="10cm">'
            f"<draw:text-box><text:p>{body}</text:p></draw:text-box></draw:frame>"
            "</draw:page>"
        )

    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<office:document {ODF_NS} office:version="1.3" '
        'office:mimetype="application/vnd.oasis.opendocument.presentation">'
        "<office:body><office:presentation>"
        + slide("page1", "Golden corpus deck", "Two slides written for the gRParse golden corpus.")
        + slide("page2", "Second slide", "Each slide has a title and one line of body text.")
        + "</office:presentation></office:body></office:document>\n"
    )


def soffice_convert(source: Path, target_ext: str, out_dir: Path, filter_name: str | None = None) -> Path:
    spec = f"{target_ext}:{filter_name}" if filter_name else target_ext
    profile = out_dir / "lo-profile"
    settings = profile / "user" / "registrymodifications.xcu"
    if not settings.exists():
        # No preview thumbnail: LibreOffice's .ppt export stores a full-size
        # one in SummaryInformation, which would make the deck 40 times larger.
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<oor:items xmlns:oor="http://openoffice.org/2001/registry" '
            'xmlns:xs="http://www.w3.org/2001/XMLSchema" '
            'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
            '<item oor:path="/org.openoffice.Office.Common/Save/Document">'
            '<prop oor:name="GenerateThumbnail" oor:op="fuse"><value>false</value></prop></item>'
            "</oor:items>\n", encoding="utf-8")
    subprocess.run(
        ["soffice", f"-env:UserInstallation=file://{profile}", "--headless", "--norestore",
         "--convert-to", spec, "--outdir", str(out_dir), str(source)],
        check=True, timeout=180, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    produced = out_dir / f"{source.stem}.{target_ext}"
    if not produced.is_file():
        raise SystemExit(f"soffice did not write {produced}")
    return produced


# ---- OLE compound files -----------------------------------------------------

def compound_file(streams: list[tuple[str, bytes]], root_clsid: bytes = bytes(16)) -> bytes:
    """A version 3 compound file ([MS-CFB]) holding `streams` directly under the root.

    512-byte sectors, streams under 4096 bytes in the mini stream, the root's
    children as a balanced binary tree in the order [MS-CFB] 2.6.4 sorts names.
    """
    sector, mini_sector, cutoff = 512, 64, 4096
    free, end, fat_sect, no_stream = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD, 0xFFFFFFFF
    sectors: list[bytes] = []
    fat: list[int] = []

    def allocate(data: bytes) -> int:
        if not data:
            return end
        count = (len(data) + sector - 1) // sector
        start = len(fat)
        for i in range(count):
            fat.append(start + i + 1 if i < count - 1 else end)
            sectors.append(data[i * sector:(i + 1) * sector].ljust(sector, b"\0"))
        return start

    mini = bytearray()
    mini_fat: list[int] = []
    placed: list[tuple[str, int, int]] = []
    for name, data in streams:
        if len(data) < cutoff:
            count = (len(data) + mini_sector - 1) // mini_sector
            start = len(mini_fat) if data else end
            for i in range(count):
                mini_fat.append(start + i + 1 if i < count - 1 else end)
            mini += data.ljust(count * mini_sector, b"\0")
            placed.append((name, start, len(data)))
        else:
            placed.append((name, allocate(data), len(data)))
    mini_start = allocate(bytes(mini))

    order = sorted(range(len(placed)), key=lambda i: (len(placed[i][0]), placed[i][0].upper()))
    links = {i: [no_stream, no_stream] for i in range(len(placed))}

    def tree(span: list[int]) -> int:
        if not span:
            return no_stream
        middle = len(span) // 2
        node = span[middle]
        links[node] = [tree(span[:middle]), tree(span[middle + 1:])]
        return node + 1  # directory index: the root entry is 0

    top = tree(order)

    def entry(name: str, kind: int, left: int, right: int, child: int, clsid: bytes,
              start: int, size: int) -> bytes:
        encoded = (name.encode("utf-16-le") + b"\0\0") if name else b""
        return (encoded.ljust(64, b"\0") + struct.pack("<HBB", len(encoded), kind, 1)
                + struct.pack("<III", left, right, child) + clsid + bytes(4 + 16)
                + struct.pack("<IQ", start, size))

    directory = entry("Root Entry", 5, no_stream, no_stream, top, root_clsid, mini_start, len(mini))
    for i, (name, start, size) in enumerate(placed):
        left, right = links[i]
        directory += entry(name, 2, left, right, no_stream, bytes(16), start, size)
    while len(directory) % sector:
        directory += entry("", 0, no_stream, no_stream, no_stream, bytes(16), 0, 0)
    directory_start = allocate(directory)
    mini_fat_bytes = b"".join(struct.pack("<I", v) for v in mini_fat)
    mini_fat_bytes = mini_fat_bytes.ljust(-(-len(mini_fat_bytes) // sector) * sector, b"\xff")
    mini_fat_start = allocate(mini_fat_bytes)
    mini_fat_sectors = len(mini_fat_bytes) // sector

    fat_sectors = 1
    while (len(fat) + fat_sectors) > fat_sectors * (sector // 4):
        fat_sectors += 1
    if fat_sectors > 109:
        raise ValueError("compound file too large for a header-only DIFAT")
    fat_start = len(fat)
    fat.extend([fat_sect] * fat_sectors)
    fat.extend([free] * (fat_sectors * (sector // 4) - len(fat)))
    fat_bytes = b"".join(struct.pack("<I", v) for v in fat)
    for i in range(fat_sectors):
        sectors.append(fat_bytes[i * sector:(i + 1) * sector])

    difat = [fat_start + i for i in range(fat_sectors)] + [free] * (109 - fat_sectors)
    header = (b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1" + bytes(16)
              + struct.pack("<HHHHH", 0x3E, 3, 0xFFFE, 9, 6) + bytes(6)
              + struct.pack("<IIIIIIIII", 0, fat_sectors, directory_start, 0, cutoff,
                            mini_fat_start if mini_fat else end, mini_fat_sectors, end, 0)
              + b"".join(struct.pack("<I", v) for v in difat))
    return header + b"".join(sectors)


def without_summary_thumbnail(path: Path) -> bytes:
    """The compound file at `path` rewritten without its SummaryInformation stream.

    LibreOffice's .ppt export stores a 384x288 bitmap preview there whatever
    the thumbnail setting says; every other stream is copied byte for byte.
    """
    import olefile  # noqa: PLC0415 - only this rewrite needs it

    with olefile.OleFileIO(str(path)) as ole:
        names = ["/".join(parts) for parts in ole.listdir(streams=True, storages=False)]
        if any("/" in name for name in names):
            raise SystemExit(f"{path} has nested storages; the rewrite only copies root streams")
        streams = [(name, ole.openstream(name).read()) for name in names
                   if name != "\x05SummaryInformation"]
        clsid = bytes.fromhex(ole.root.clsid.replace("-", "")) if ole.root.clsid else bytes(16)
    return compound_file(streams, root_clsid=_clsid_bytes(clsid))


def _clsid_bytes(big_endian: bytes) -> bytes:
    """olefile prints a CLSID in its textual (mixed-endian) form; store it as the wire does."""
    if big_endian == bytes(16):
        return big_endian
    return (big_endian[3::-1] + big_endian[5:3:-1] + big_endian[7:5:-1] + big_endian[8:])


# ---- archives ---------------------------------------------------------------

def stored_zip(entries: list[tuple[str, bytes]], compress_after_first: bool = True) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for index, (name, data) in enumerate(entries):
            info = zipfile.ZipInfo(name, date_time=FIXED_ZIP_TIME)
            info.compress_type = (zipfile.ZIP_DEFLATED if compress_after_first and index > 0
                                  else zipfile.ZIP_STORED)
            archive.writestr(info, data)
    return buffer.getvalue()


def tar_gz(members: list[tuple[str, bytes]]) -> bytes:
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for name, data in members:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = FIXED_MTIME
            info.mode = 0o644
            archive.addfile(info, io.BytesIO(data))
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=FIXED_MTIME, filename="") as gz:
        gz.write(raw.getvalue())
    return out.getvalue()


# ---- text formats -----------------------------------------------------------

ASCIIDOC = """= Golden Corpus Notes
:author: gRParse fixtures

This AsciiDoc fixture was written for the gRParse golden corpus.

== A Section

A paragraph under the first section, with *bold* and _italic_ words.

[cols="1,1"]
|===
|Item |Count

|Apples
|3

|Pears
|5
|===
"""

LATEX = r"""\documentclass{article}
\title{Golden Corpus Notes}
\begin{document}
\maketitle
\section{Introduction}
This \LaTeX{} fixture was written for the gRParse golden corpus.
\begin{tabular}{lr}
Item & Count \\
Apples & 3 \\
Pears & 5 \\
\end{tabular}
\end{document}
"""

VTT = """WEBVTT

00:00:00.000 --> 00:00:01.500
This caption track was written for the golden corpus.

00:00:01.500 --> 00:00:03.000
It has two cues and nothing else.
"""

CSV = "Item,Count,Colour\r\nApples,3,Red\r\nPears,5,Green\r\n"

XBRL = """<?xml version="1.0" encoding="UTF-8"?>
<xbrli:xbrl xmlns:xbrli="http://www.xbrl.org/2003/instance"
            xmlns:link="http://www.xbrl.org/2003/linkbase"
            xmlns:xlink="http://www.w3.org/1999/xlink"
            xmlns:iso4217="http://www.xbrl.org/2003/iso4217"
            xmlns:ex="http://example.com/golden/2026">
  <link:schemaRef xlink:type="simple" xlink:href="golden-2026.xsd"/>
  <xbrli:context id="FY2026">
    <xbrli:entity><xbrli:identifier scheme="http://example.com/id">GOLDEN</xbrli:identifier></xbrli:entity>
    <xbrli:period><xbrli:startDate>2026-01-01</xbrli:startDate><xbrli:endDate>2026-12-31</xbrli:endDate></xbrli:period>
  </xbrli:context>
  <xbrli:unit id="USD"><xbrli:measure>iso4217:USD</xbrli:measure></xbrli:unit>
  <ex:Revenue contextRef="FY2026" unitRef="USD" decimals="0">1200</ex:Revenue>
  <ex:Expenses contextRef="FY2026" unitRef="USD" decimals="0">800</ex:Expenses>
</xbrli:xbrl>
"""

DOCLANG = """<?xml version="1.0" encoding="UTF-8"?>
<doclang xmlns="http://docling-project.org/ns/doclang/v1">
  <title>Golden Corpus Notes</title>
  <section-header level="1">A Section</section-header>
  <paragraph>This DocLang fixture was written for the gRParse golden corpus.</paragraph>
</doclang>
"""

METS = """<?xml version="1.0" encoding="UTF-8"?>
<METS:mets xmlns:METS="http://www.loc.gov/METS/" xmlns:xlink="http://www.w3.org/1999/xlink"
           PROFILE="gbs" OBJID="golden-0001">
  <METS:fileSec>
    <METS:fileGrp ID="TXTGRP" USE="ocr">
      <METS:file ID="TXT00000001" MIMETYPE="text/plain">
        <METS:FLocat LOCTYPE="URL" xlink:href="00000001.txt"/>
      </METS:file>
    </METS:fileGrp>
  </METS:fileSec>
  <METS:structMap TYPE="physical">
    <METS:div TYPE="volume">
      <METS:div TYPE="page" ORDER="1"><METS:fptr FILEID="TXT00000001"/></METS:div>
    </METS:div>
  </METS:structMap>
</METS:mets>
"""

MHTML = (
    "From: <Saved by gRParse fixtures>\r\n"
    "Subject: Golden corpus page\r\n"
    "Date: Sat, 01 Jan 2000 00:00:00 -0000\r\n"
    "MIME-Version: 1.0\r\n"
    'Content-Type: multipart/related;\r\n\ttype="text/html";\r\n\tboundary="----golden-boundary"\r\n'
    "\r\n"
    "------golden-boundary\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Transfer-Encoding: 8bit\r\n"
    "Content-Location: https://example.com/golden.html\r\n"
    "\r\n"
    "<!DOCTYPE html><html><head><title>Golden corpus page</title></head>"
    "<body><h1>Golden corpus page</h1><p>A saved web page written for the gRParse golden corpus.</p>"
    '<img src="https://example.com/bars.png" alt="Three bars"></body></html>\r\n'
    "------golden-boundary\r\n"
    "Content-Type: image/png\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "Content-Location: https://example.com/bars.png\r\n"
    "\r\n"
)


def docling_json() -> str:
    """A minimal serialized DoclingDocument: a title and one paragraph."""
    def text(index: int, label: str, value: str) -> dict:
        return {"self_ref": f"#/texts/{index}", "parent": {"$ref": "#/body"}, "children": [],
                "content_layer": "body", "label": label, "prov": [], "orig": value, "text": value}

    document = {
        "schema_name": "DoclingDocument",
        "version": "1.7.0",
        "name": "sample",
        "origin": {"mimetype": "text/markdown", "binary_hash": 1, "filename": "sample.md"},
        "furniture": {"self_ref": "#/furniture", "children": [], "content_layer": "furniture",
                      "name": "_root_", "label": "unspecified"},
        "body": {"self_ref": "#/body", "children": [{"$ref": "#/texts/0"}, {"$ref": "#/texts/1"}],
                 "content_layer": "body", "name": "_root_", "label": "unspecified"},
        "groups": [],
        "texts": [text(0, "title", "Golden Corpus Notes"),
                  text(1, "text", "This serialized DoclingDocument was written for the gRParse golden corpus.")],
        "pictures": [], "tables": [], "key_value_items": [], "form_items": [], "pages": {},
    }
    return json.dumps(document, indent=1) + "\n"


def boxnote() -> str:
    """A minimal BoxNote: the Etherpad-style atext with an empty attribute pool."""
    body = "Golden corpus note\nThis Box Note was written for the gRParse golden corpus.\n"
    note = {"version": 1, "lastEditTimestamp": FIXED_MTIME * 1000,
            "atext": {"text": body, "attribs": f"|2+{len(body)}"},
            "pool": {"numToAttrib": {}, "attribToNum": {}, "nextNum": 0}}
    return json.dumps(note, indent=1) + "\n"


# ---- binary formats with no free writer: just what the sniff needs ----------

def ebcdic_record() -> bytes:
    """One 80-byte fixed-length record in EBCDIC code page 037."""
    fields = "00001" + "GOLDEN CORPUS".ljust(25) + "2026-11-07" + "0000120000" + "USD"
    return fields.ljust(80).encode("cp037")


def afp_stream() -> bytes:
    """A MO:DCA print stream holding only Begin Document and End Document.

    Each structured field is the 0x5A carriage-control byte, a two-byte
    length, the three-byte field identifier, a flag byte, two sequence bytes
    and the field's data.
    """
    def field(identifier: bytes, data: bytes) -> bytes:
        length = 8 + len(data)
        return b"\x5a" + struct.pack(">H", length) + identifier + b"\x00\x00\x00" + data

    name = "GOLDEN01".encode("cp037")
    return field(b"\xd3\xa8\xa8", name) + field(b"\xd3\xa9\xa8", name)


def iwork(slides: bool) -> bytes:
    entries = [("Index/Document.iwa", b"\x00"), ("Index/Metadata.iwa", b"\x00")]
    if slides:
        entries += [("Index/MasterSlide-1.iwa", b"\x00"), ("Index/Slide-1.iwa", b"\x00")]
    else:
        entries += [("Index/CalculationEngine.iwa", b"\x00")]
    entries.append(("Metadata/Properties.plist", b'<?xml version="1.0"?><plist version="1.0"><dict/></plist>'))
    return stored_zip(entries, compress_after_first=False)


# ---- media ------------------------------------------------------------------

def ffmpeg(args: list[str]) -> None:
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args],
                   check=True, timeout=120)


def main(argv: list[str]) -> int:
    out = Path(argv[0]).resolve() if argv else DEFAULT_OUT
    out.mkdir(parents=True, exist_ok=True)
    for tool in ("soffice", "ffmpeg"):
        if shutil.which(tool) is None:
            print(f"{tool} is required", file=sys.stderr)
            return 2

    def write(name: str, data: bytes | str) -> None:
        payload = data.encode("utf-8") if isinstance(data, str) else data
        (out / name).write_bytes(payload)

    write("sample.adoc", ASCIIDOC)
    write("sample.tex", LATEX)
    write("captions.vtt", VTT)
    write("table.csv", CSV)
    write("instance.xbrl", XBRL)
    write("sample.dclg", DOCLANG)
    write("sample.dclx", stored_zip([("document.xml", DOCLANG.encode("utf-8"))]))
    write("mets-gbs.tar.gz", tar_gz([("golden-0001/golden-0001.xml", METS.encode("utf-8")),
                                     ("golden-0001/00000001.txt",
                                      b"Page one of a scanned volume written for the golden corpus.\n")]))
    write("page.mhtml", MHTML + base64.encodebytes(png_bars()).decode("ascii").replace("\n", "\r\n")
          + "\r\n------golden-boundary--\r\n")
    write("sample.json", docling_json())
    write("sample.boxnote", boxnote())
    write("record.ebcdic", ebcdic_record())
    write("statement.afp", afp_stream())
    write("sample.pages", iwork(slides=False))
    write("sample.numbers", iwork(slides=False))
    write("sample.key", iwork(slides=True))

    with tempfile.TemporaryDirectory() as scratch_name:
        scratch = Path(scratch_name)
        writer = scratch / "sample.fodt"
        writer.write_text(fodt("Golden Corpus Notes", with_picture=False), encoding="utf-8")
        for ext, filt in (("odt", None), ("doc", "MS Word 97"), ("rtf", "Rich Text Format")):
            shutil.copyfile(soffice_convert(writer, ext, scratch, filt), out / f"sample.{ext}")
        calc = scratch / "sample.fods"
        calc.write_text(fods(), encoding="utf-8")
        for ext, filt in (("ods", None), ("xls", "MS Excel 97")):
            shutil.copyfile(soffice_convert(calc, ext, scratch, filt), out / f"sample.{ext}")
        impress = scratch / "sample.fodp"
        impress.write_text(fodp(), encoding="utf-8")
        shutil.copyfile(soffice_convert(impress, "odp", scratch), out / "sample.odp")
        write("sample.ppt", without_summary_thumbnail(
            soffice_convert(impress, "ppt", scratch, "MS PowerPoint 97")))
        parity = scratch / "table-picture.fodt"
        parity.write_text(fodt("Table and Picture", with_picture=True), encoding="utf-8")
        shutil.copyfile(soffice_convert(parity, "pdf", scratch, "writer_pdf_Export"),
                        out / "table-picture.pdf")

    # A one-second 440 Hz tone, 8 kHz mono 16-bit, and a one-second 64x48
    # colour-bar clip with the same tone, both generated by ffmpeg's sources.
    ffmpeg(["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=8000:duration=1",
            "-ac", "1", "-c:a", "pcm_s16le", "-fflags", "+bitexact", "-flags", "+bitexact",
            "-map_metadata", "-1", str(out / "tone.wav")])
    ffmpeg(["-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=1",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=8000:duration=1",
            "-c:v", "mpeg4", "-q:v", "10", "-c:a", "aac", "-b:a", "16k", "-shortest",
            "-fflags", "+bitexact", "-flags", "+bitexact", "-map_metadata", "-1",
            str(out / "clip.mp4")])

    total = sum(path.stat().st_size for path in out.iterdir() if path.is_file())
    print(f"{out}: {sum(1 for p in out.iterdir() if p.is_file())} fixtures, {total} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
