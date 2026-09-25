"""Deterministic PNG writer / reader (8- and 16-bit grey, RGB, RGBA).

The writer uses filter 0 and *stored* deflate blocks with an Adler-32 trailer, so the bytes depend
only on the pixels (not on the zlib build: zlib-ng and classic zlib compress differently). Files are
larger than compressed PNGs; they are cache intermediates that the texture cook re-encodes.
The reader accepts any non-interlaced PNG with filter types 0-4 (uses zlib to inflate).
"""

from __future__ import annotations

import struct
import zlib

_SIG = b"\x89PNG\r\n\x1a\n"
_CHANNELS = {0: 1, 2: 3, 6: 4}  # colour type -> channels
_COLOR_TYPE = {1: 0, 3: 2, 4: 6}


def _chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def stored_deflate(data: bytes) -> bytes:
    """zlib stream (CMF/FLG 0x78 0x01) of stored blocks <= 65535 bytes."""
    out = bytearray(b"\x78\x01")
    n = len(data)
    pos = 0
    while True:
        block = data[pos:pos + 65535]
        pos += len(block)
        final = 1 if pos >= n else 0
        out += bytes([final]) + struct.pack("<HH", len(block), len(block) ^ 0xFFFF) + block
        if final:
            break
    out += struct.pack(">I", zlib.adler32(data) & 0xFFFFFFFF)
    return bytes(out)


def encode_png(width: int, height: int, channels: int, pixels, bit_depth: int = 8) -> bytes:
    """`pixels`: flat sequence of width*height*channels ints (0..255 or 0..65535), row 0 at the top."""
    if channels not in _COLOR_TYPE:
        raise ValueError("channels must be 1, 3 or 4")
    if bit_depth not in (8, 16):
        raise ValueError("bit_depth must be 8 or 16")
    if width <= 0 or height <= 0 or len(pixels) != width * height * channels:
        raise ValueError("pixel buffer does not match width * height * channels")
    maxv = (1 << bit_depth) - 1
    row_values = width * channels
    raw = bytearray()
    fmt = ">%dH" % row_values if bit_depth == 16 else None
    for y in range(height):
        row = pixels[y * row_values:(y + 1) * row_values]
        for v in row:
            if not 0 <= v <= maxv:
                raise ValueError(f"pixel value {v} out of range for {bit_depth}-bit")
        raw.append(0)
        raw += struct.pack(fmt, *row) if fmt else bytes(row)
    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, _COLOR_TYPE[channels], 0, 0, 0)
    return _SIG + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", stored_deflate(bytes(raw))) + _chunk(b"IEND", b"")


def decode_png(data: bytes):
    """Returns (width, height, channels, bit_depth, pixels list)."""
    if data[:8] != _SIG:
        raise ValueError("not a PNG")
    pos = 8
    idat = bytearray()
    width = height = depth = ctype = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if interlace or ctype not in _CHANNELS or depth not in (8, 16):
                raise ValueError("unsupported PNG layout")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    ch = _CHANNELS[ctype]
    bpp = ch * depth // 8
    stride = width * bpp
    raw = zlib.decompress(bytes(idat))
    out = bytearray()
    prev = bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out += line
        prev = line
    if depth == 16:
        pixels = list(struct.unpack(">%dH" % (len(out) // 2), bytes(out)))
    else:
        pixels = list(out)
    return width, height, ch, depth, pixels
