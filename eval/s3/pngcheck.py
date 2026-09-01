"""Whether a PNG is one flat colour, from its bytes alone (no imaging
library): the page previews gRParse embeds are enough to tell a blank scan
from a page recognition missed. Supports the non-interlaced 8-bit forms the
producer emits; anything else is "unknown" (None)."""

from __future__ import annotations

import struct
import zlib

SIGNATURE = b"\x89PNG\r\n\x1a\n"
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def _unfilter(raw: bytes, width: int, height: int, bpp: int) -> bytes | None:
    stride = width * bpp
    if len(raw) < height * (stride + 1):
        return None
    previous = bytearray(stride)
    out = bytearray()
    offset = 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1: offset + 1 + stride])
        offset += stride + 1
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = previous[i]
            c = previous[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + a) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + b) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[i] = (line[i] + predictor) & 0xFF
            elif kind != 0:
                return None
        out += line
        previous = line
    return bytes(out)


def png_is_uniform(data: bytes) -> bool | None:
    """True when every pixel equals the first, False when not, None when the
    PNG is not one this reader decodes."""
    if not data.startswith(SIGNATURE):
        return None
    offset = 8
    width = height = depth = color = interlace = None
    idat = bytearray()
    while offset + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[offset: offset + 8])
        body = data[offset + 8: offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body[:13])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if width is None or depth != 8 or interlace != 0 or color not in CHANNELS:
        return None
    try:
        raw = zlib.decompress(bytes(idat))
    except zlib.error:
        return None
    bpp = CHANNELS[color]
    pixels = _unfilter(raw, width, height, bpp)
    if pixels is None or not pixels:
        return None
    first = pixels[:bpp]
    return all(pixels[i: i + bpp] == first for i in range(0, len(pixels), bpp))
