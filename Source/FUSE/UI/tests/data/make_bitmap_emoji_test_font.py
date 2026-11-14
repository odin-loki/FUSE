#!/usr/bin/env python3
"""Builds bitmap_cbdt_test.ttf (CBDT / CBLC) and bitmap_sbix_test.ttf (sbix), tiny PNG-strike colour fonts for the
fuse_text bitmap emoji gate (needs fontTools; PNGs are written with zlib + struct).

Both fonts: 1000 upem, glyph order .notdef, A (outline box), e100, e101; cmap U+0041 -> A, U+E100 -> e100,
U+E101 -> e101. Three strikes, ppem P = 16 (red), 32 (green), 64 (blue). The bitmap glyphs have empty outlines.

U+E100, every strike: a P x P image, top edge 0.8 P (rounded) above the baseline, left edge on the origin.
  1-px transparent border; the inner pixels are the strike colour, opaque, except the bottom-right quadrant
  (x >= P/2 and y >= P/2, y down) which is the strike colour at alpha 128.
U+E101, CBDT: a P/2 x P image, left edge P/4 right of the origin, same top; 1-px transparent border, inner pixels
  the strike colour, opaque. U+E101, sbix: a 'dupe' of U+E100 (identical rendering).

CBDT layout: strike 16 = index subtable format 1 + image format 17, strike 32 = index format 3 + image format 17,
strike 64 = index format 1 + image format 18. sbix: graphicType 'png ', origin (0, round(0.8 P) - P).
Usage: python make_bitmap_emoji_test_font.py <outdir>
"""
import os
import struct
import sys
import zlib

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables.DefaultTable import DefaultTable

STRIKES = [(16, (255, 0, 0)), (32, (0, 255, 0)), (64, (0, 0, 255))]
NAMES = [".notdef", "A", "e100", "e101"]
FIRST_BITMAP_GLYPH = 2


def png(w, h, pixels):
    """RGBA8 straight-alpha PNG; pixels is a list of rows of (r, g, b, a)."""
    raw = b"".join(b"\x00" + b"".join(bytes(p) for p in row) for row in pixels)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def image_e100(p, rgb):
    rows = []
    for y in range(p):
        row = []
        for x in range(p):
            if x in (0, p - 1) or y in (0, p - 1):
                row.append((0, 0, 0, 0))
            elif x >= p // 2 and y >= p // 2:
                row.append(rgb + (128,))
            else:
                row.append(rgb + (255,))
        rows.append(row)
    return png(p, p, rows)


def image_e101(p, rgb):
    w = p // 2
    rows = [[(0, 0, 0, 0) if x in (0, w - 1) or y in (0, p - 1) else rgb + (255,) for x in range(w)] for y in range(p)]
    return png(w, p, rows)


def top(p):
    return round(0.8 * p)


def base_font():
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(NAMES)
    fb.setupCharacterMap({0x41: "A", 0xE100: "e100", 0xE101: "e101"})
    pen = TTGlyphPen(None)
    pen.moveTo((100, 0))
    pen.lineTo((100, 700))
    pen.lineTo((600, 700))
    pen.lineTo((600, 0))
    pen.closePath()
    box = pen.glyph()
    empty = TTGlyphPen(None).glyph()
    fb.setupGlyf({".notdef": box, "A": box, "e100": empty, "e101": empty})
    fb.setupHorizontalMetrics({".notdef": (700, 100), "A": (700, 100), "e100": (1000, 0), "e101": (1000, 0)})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "FUSE Bitmap Emoji Test", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    return fb


def raw_table(tag, data):
    t = DefaultTable(tag)
    t.data = data
    return t


def build_cbdt(path):
    fb = base_font()
    cbdt = bytearray(struct.pack(">HH", 3, 0))
    strikes = []  # (ppem, index subtable bytes, index format)
    for p, rgb in STRIKES:
        index_format, image_format = {16: (1, 17), 32: (3, 17), 64: (1, 18)}[p]
        image_data_offset = len(cbdt)
        offsets = []
        for name in ("e100", "e101"):
            if name == "e100":
                w, bx, data = p, 0, image_e100(p, rgb)
            else:
                w, bx, data = p // 2, p // 4, image_e101(p, rgb)
            offsets.append(len(cbdt) - image_data_offset)
            if image_format == 17:  # smallGlyphMetrics: height, width, bearingX, bearingY, advance
                rec = struct.pack(">BBbbB", p, w, bx, top(p), p)
            else:  # bigGlyphMetrics: height, width, horiBearingX/Y, horiAdvance, vertBearingX/Y, vertAdvance
                rec = struct.pack(">BBbbBbbB", p, w, bx, top(p), p, -(w // 2), 0, p)
            cbdt += rec + struct.pack(">I", len(data)) + data
        offsets.append(len(cbdt) - image_data_offset)
        fmt = ">I" if index_format == 1 else ">H"
        sub = struct.pack(">HHI", index_format, image_format, image_data_offset)
        sub += b"".join(struct.pack(fmt, o) for o in offsets)
        sub += b"\x00" * (-len(sub) % 4)
        strikes.append((p, sub))

    n = len(strikes)
    cblc = bytearray(struct.pack(">HHI", 3, 0, n))
    tail = bytearray()
    tail_base = 8 + 48 * n
    for p, sub in strikes:
        array_offset = tail_base + len(tail)
        array = struct.pack(">HHI", FIRST_BITMAP_GLYPH, FIRST_BITMAP_GLYPH + 1, 8)  # one IndexSubTableArray entry
        tail += array + sub
        hori = struct.pack(">bbBbbbbbbbbb", top(p), top(p) - p, p, 1, 0, 0, 0, 0, top(p), top(p) - p, 0, 0)
        vert = struct.pack(">bbBbbbbbbbbb", p // 2, -(p // 2), p, 0, 1, 0, 0, 0, 0, 0, 0, 0)
        cblc += struct.pack(">IIII", array_offset, len(array) + len(sub), 1, 0) + hori + vert
        cblc += struct.pack(">HHBBBb", FIRST_BITMAP_GLYPH, FIRST_BITMAP_GLYPH + 1, p, p, 32, 1)
    cblc += tail
    fb.font["CBDT"] = raw_table("CBDT", bytes(cbdt))
    fb.font["CBLC"] = raw_table("CBLC", bytes(cblc))
    fb.save(path)


def build_sbix(path):
    fb = base_font()
    num_glyphs = len(NAMES)
    strikes = []
    for p, rgb in STRIKES:
        records = [b"", b""]  # .notdef, A: no bitmap
        records.append(struct.pack(">hh4s", 0, top(p) - p, b"png ") + image_e100(p, rgb))
        records.append(struct.pack(">hh4sH", 0, 0, b"dupe", FIRST_BITMAP_GLYPH))
        header = 4 + 4 * (num_glyphs + 1)
        offsets, pos = [], header
        for r in records:
            offsets.append(pos)
            pos += len(r)
        offsets.append(pos)
        strike = struct.pack(">HH", p, 72) + b"".join(struct.pack(">I", o) for o in offsets) + b"".join(records)
        strikes.append(strike + b"\x00" * (-len(strike) % 4))
    sbix = bytearray(struct.pack(">HHI", 1, 1, len(strikes)))
    pos = 8 + 4 * len(strikes)
    for s in strikes:
        sbix += struct.pack(">I", pos)
        pos += len(s)
    for s in strikes:
        sbix += s
    fb.font["sbix"] = raw_table("sbix", bytes(sbix))
    fb.save(path)


def main():
    out = sys.argv[1]
    build_cbdt(os.path.join(out, "bitmap_cbdt_test.ttf"))
    build_sbix(os.path.join(out, "bitmap_sbix_test.ttf"))


if __name__ == "__main__":
    main()
