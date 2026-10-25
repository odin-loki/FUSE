#!/usr/bin/env python3
"""Write Samples/unification/common/sprite_maps/HeroSheet.png (8-frame horizontal strip)."""
import os
import struct
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "Samples", "unification", "common", "sprite_maps", "HeroSheet.png")

w, h = 64, 8
rows = []
for _y in range(h):
    row = bytearray([0])
    for x in range(w):
        frame = x // 8
        row.extend([50 + frame * 25, 100, max(0, 200 - frame * 10), 255])
    rows.append(bytes(row))
raw = b"".join(rows)
comp = zlib.compress(raw, 9)


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


png = (
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    + chunk(b"IDAT", comp)
    + chunk(b"IEND", b"")
)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "wb") as f:
    f.write(png)
print(OUT, len(png))
