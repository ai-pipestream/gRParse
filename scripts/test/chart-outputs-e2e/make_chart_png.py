"""Writes a small bar-chart PNG (three bars on white) with the standard
library only, for the chart-outputs end to end test."""

import struct
import sys
import zlib

W, H = 120, 80
BARS = [(15, 120), (50, 95), (85, 80)]


def pixel(x, y):
    for left, value in BARS:
        top = H - int(value * 0.6)
        if left <= x < left + 25 and y >= top:
            return (40, 90, 200)
    return (255, 255, 255)


def chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


rows = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(W)) for y in range(H))
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
       chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
with open(sys.argv[1], "wb") as out:
    out.write(png)
