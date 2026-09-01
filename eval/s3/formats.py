"""File types, families, declared mimetypes and parser types.

The extension table mirrors ``src/content_sniff.cpp`` (extension_mimetype):
the FileSource wire declares no content type, so the key's extension is the
declaration gRParse sees, and the origin must agree with it.
"""

from __future__ import annotations

from typing import Any, Iterable

# Extension (without the dot, lower-case, compound suffixes kept) to the
# mimetype the extension declares. Mirror of content_sniff.cpp plus the
# compressed WARC forms the router recognises by suffix.
EXTENSION_MIMETYPES: dict[str, str] = {
    "pdf": "application/pdf",
    "jpg": "image/jpeg", "jpeg": "image/jpeg",
    "tif": "image/tiff", "tiff": "image/tiff",
    "png": "image/png", "gif": "image/gif", "webp": "image/webp", "bmp": "image/bmp",
    "docx": "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    "xlsx": "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    "pptx": "application/vnd.openxmlformats-officedocument.presentationml.presentation",
    "odt": "application/vnd.oasis.opendocument.text",
    "ods": "application/vnd.oasis.opendocument.spreadsheet",
    "odp": "application/vnd.oasis.opendocument.presentation",
    "doc": "application/msword", "xls": "application/vnd.ms-excel", "ppt": "application/vnd.ms-powerpoint",
    "csv": "text/csv", "rtf": "application/rtf", "epub": "application/epub+zip",
    "eml": "message/rfc822", "msg": "application/vnd.ms-outlook",
    "xml": "application/xml", "nxml": "application/xml", "xbrl": "application/xml",
    "html": "text/html", "htm": "text/html", "xhtml": "application/xhtml+xml",
    "md": "text/markdown", "markdown": "text/markdown", "txt": "text/plain", "json": "application/json",
    "warc": "application/warc", "warc.gz": "application/warc", "warc.zst": "application/warc",
    "warc.lz4": "application/warc",
    "mp3": "audio/mpeg", "wav": "audio/wav", "m4a": "audio/mp4", "flac": "audio/flac",
    "ogg": "audio/ogg", "oga": "audio/ogg", "opus": "audio/ogg",
    "mp4": "video/mp4", "m4v": "video/mp4", "mkv": "video/x-matroska", "webm": "video/webm",
    "mov": "video/quicktime",
}

# Mimetypes the origin may carry for an extension besides the declared one:
# an XML declaration is text/xml to some producers; a gzip-wrapped WARC is
# sniffed as gzip before the router opens it.
MIMETYPE_ALIASES: dict[str, frozenset[str]] = {
    "xml": frozenset({"text/xml"}), "nxml": frozenset({"text/xml"}), "xbrl": frozenset({"text/xml"}),
    "warc.gz": frozenset({"application/gzip"}),
    "htm": frozenset(), "jpeg": frozenset(),
}

# Extensions whose bytes do not name their format: OLE2 compound files carry
# one signature for .doc, .xls, .ppt and .msg alike, and a compressed WARC
# is a gzip/zstd/lz4 stream until it is opened. Their name is their
# declaration, so the extension-less sniff run is not a fair test of them.
NOT_SELF_DESCRIBING = frozenset({"doc", "xls", "ppt", "msg", "warc.gz", "warc.zst", "warc.lz4"})

COMPOUND_SUFFIXES = ("warc.gz", "warc.zst", "warc.lz4", "tar.gz", "layout.json")

FAMILIES: dict[str, frozenset[str]] = {
    "pdf": frozenset({"pdf"}),
    "image": frozenset({"png", "jpg", "jpeg", "tif", "tiff", "gif", "bmp", "webp"}),
    "word": frozenset({"docx", "doc", "odt", "rtf", "docm", "dot", "dotx", "fodt", "ott"}),
    "sheet": frozenset({"xlsx", "xls", "ods", "csv", "xlsm", "xlsb", "fods", "ots"}),
    "deck": frozenset({"pptx", "ppt", "odp", "pptm", "fodp", "otp"}),
    "html": frozenset({"html", "htm", "xhtml"}),
    "markdown": frozenset({"md", "markdown", "mdown"}),
    "xml": frozenset({"xml", "nxml", "xbrl"}),
    "email": frozenset({"eml", "msg"}),
    "epub": frozenset({"epub"}),
    "text": frozenset({"txt"}),
    "warc": frozenset({"warc", "warc.gz", "warc.zst", "warc.lz4"}),
    "audio": frozenset({"mp3", "wav", "m4a", "flac", "ogg", "oga", "opus"}),
    "video": frozenset({"mp4", "m4v", "mkv", "webm", "mov"}),
    "ebcdic": frozenset({"ebc"}),
    "json": frozenset({"json"}),
}

# Families whose parse must yield readable text (a PDF joins when its text
# layer was read, see ``pdf_has_text_layer``).
TEXT_BEARING = frozenset({"word", "deck", "html", "markdown", "xml", "email", "epub", "text"})
# Families whose items sit on pages and must say which one.
PAGED = frozenset({"pdf", "image", "word", "sheet", "deck"})

# Every collector name the fleet stamps on items or claims (AGENTS.md table
# plus the in-process ones and the shell peers the merge already ranks).
KNOWN_COLLECTORS = frozenset({
    "grparse", "libreoffice", "pdf", "email", "xml", "epub", "markup", "ebcdic", "lol-html",
    "asr", "fastwarc", "confluence", "poi", "calamine", "enrich",
})

ARENAS = ("texts", "tables", "pictures", "groups", "key_value_items", "form_items",
          "field_regions", "field_items")


def extension_of(key: str) -> str:
    """The lower-case extension of an object key, compound suffixes kept
    ("warc.gz"), empty when the last path element has none."""
    name = key.rsplit("/", 1)[-1].lower()
    for suffix in COMPOUND_SUFFIXES:
        if name.endswith("." + suffix):
            return suffix
    if "." not in name or name.startswith(".") and name.count(".") == 1:
        return ""
    return name.rsplit(".", 1)[1]


def strip_extension(name: str) -> str:
    """The object name with its extension removed, the name a sniff run sends."""
    ext = extension_of(name)
    base = name.rsplit("/", 1)[-1]
    return base[: -(len(ext) + 1)] if ext else base


def family_of(ext: str) -> str:
    for family, members in FAMILIES.items():
        if ext in members:
            return family
    return "other"


def declared_mimetype(ext: str) -> str | None:
    return EXTENSION_MIMETYPES.get(ext)


def acceptable_mimetypes(ext: str) -> frozenset[str]:
    declared = declared_mimetype(ext)
    accepted = set(MIMETYPE_ALIASES.get(ext, frozenset()))
    if declared:
        accepted.add(declared)
    return frozenset(accepted)


def text_base(item: dict[str, Any]) -> dict[str, Any]:
    """The TextItemBase of one ``texts`` entry (CodeItem inlines its fields)."""
    if not item:
        return {}
    kind, inner = next(iter(item.items()))
    if kind == "code":
        return inner
    return inner.get("base", inner) if isinstance(inner, dict) else {}


def item_collectors(node: dict[str, Any]) -> set[str]:
    names: set[str] = set()
    for source in node.get("source", []) or []:
        collector = (source.get("collector") or {}).get("collector")
        if collector:
            names.add(collector)
    return names


def arena_nodes(document: dict[str, Any]) -> Iterable[tuple[str, dict[str, Any]]]:
    """(ref, node) for every arena item, the text base unwrapped."""
    for arena in ARENAS:
        for index, raw in enumerate(document.get(arena, []) or []):
            node = text_base(raw) if arena == "texts" else raw
            yield f"#/{arena}/{index}", node


def collectors_of(document: dict[str, Any]) -> set[str]:
    """The collectors that answered with items: the parser type's ingredients."""
    names: set[str] = set()
    for _, node in arena_nodes(document):
        names |= item_collectors(node)
    return names


def parser_type(document: dict[str, Any]) -> str:
    names = collectors_of(document)
    if not names:
        return "none"
    return "+".join("grparse-cv" if name == "grparse" else name for name in sorted(names))


def pdf_has_text_layer(document: dict[str, Any]) -> bool:
    """True when the PDF's text came from its text layer: the inspector's
    fold ("pdf") or the CV path's poppler-text engine."""
    for _, node in arena_nodes(document):
        for source in node.get("source", []) or []:
            collector = source.get("collector") or {}
            if collector.get("collector") == "pdf":
                return True
            if collector.get("collector") == "grparse" and collector.get("model") == "poppler-text":
                return True
    return False
