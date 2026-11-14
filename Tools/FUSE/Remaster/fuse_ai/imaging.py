"""Tiny, deterministic, pure-Python media helpers for the mock providers (plan §3.1 ``mock`` row).

* :class:`Image` - an RGBA8 image (``bytes`` in row-major RGBA order).
* PNG encode uses *stored* (uncompressed) deflate blocks written by hand, so the output bytes do not
  depend on the zlib build (zlib vs zlib-ng) and are byte-identical on every platform. PNG decode
  handles 8-bit grey / grey+alpha / RGB / RGBA, non-interlaced, all five filters.
* WAV encode/decode for 16-bit PCM, header written with ``struct`` (no ``wave`` module quirks).

All arithmetic is integer, so results never depend on the platform's libm.
"""

from __future__ import annotations

import hashlib
import struct
import zlib
from dataclasses import dataclass
from typing import List, Tuple

from .errors import RecipeError

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    rgba: bytes  # len == width * height * 4

    def __post_init__(self) -> None:
        if self.width <= 0 or self.height <= 0 or len(self.rgba) != self.width * self.height * 4:
            raise ValueError(f"bad image {self.width}x{self.height} with {len(self.rgba)} bytes")

    def pixel(self, x: int, y: int) -> Tuple[int, int, int, int]:
        i = (y * self.width + x) * 4
        r, g, b, a = self.rgba[i:i + 4]
        return r, g, b, a

    def luma(self) -> List[int]:
        """Rec.601 integer luma per pixel (0..255)."""
        px = self.rgba
        return [(77 * px[i] + 150 * px[i + 1] + 29 * px[i + 2] + 128) >> 8 for i in range(0, len(px), 4)]


def is_png(data: bytes) -> bool:
    return data[:8] == PNG_SIGNATURE


def _chunk(tag: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)


def _stored_zlib(raw: bytes) -> bytes:
    """A zlib stream of stored deflate blocks (deterministic regardless of the zlib implementation)."""
    out = bytearray(b"\x78\x01")
    max_block = 65535
    if not raw:
        out += b"\x01\x00\x00\xff\xff"
    for off in range(0, len(raw), max_block):
        block = raw[off:off + max_block]
        final = 1 if off + max_block >= len(raw) else 0
        out.append(final)
        out += struct.pack("<HH", len(block), len(block) ^ 0xFFFF)
        out += block
    out += struct.pack(">I", zlib.adler32(raw) & 0xFFFFFFFF)
    return bytes(out)


def encode_png(img: Image) -> bytes:
    stride = img.width * 4
    raw = bytearray()
    for y in range(img.height):
        raw.append(0)  # filter: none
        raw += img.rgba[y * stride:(y + 1) * stride]
    ihdr = struct.pack(">IIBBBBB", img.width, img.height, 8, 6, 0, 0, 0)
    return PNG_SIGNATURE + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", _stored_zlib(bytes(raw))) + _chunk(b"IEND", b"")


def decode_png(data: bytes) -> Image:
    if not is_png(data):
        raise RecipeError("not a PNG")
    pos = 8
    width = height = 0
    color_type = -1
    idat = bytearray()
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, color_type, _comp, _filt, interlace = struct.unpack(">IIBBBBB", payload)
            if depth != 8 or interlace != 0 or color_type not in (0, 2, 4, 6):
                raise RecipeError("mock PNG decoder supports 8-bit non-interlaced grey/GA/RGB/RGBA only")
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    prev = bytearray(stride)
    rows = []
    off = 0
    for _ in range(height):
        ftype = raw[off]
        line = bytearray(raw[off + 1:off + 1 + stride])
        off += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        rows.append(bytes(line))
        prev = line
    rgba = bytearray()
    for line in rows:
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if channels == 1:
                rgba += bytes((px[0], px[0], px[0], 255))
            elif channels == 2:
                rgba += bytes((px[0], px[0], px[0], px[1]))
            elif channels == 3:
                rgba += bytes((px[0], px[1], px[2], 255))
            else:
                rgba += px
    return Image(width, height, bytes(rgba))


def synthetic_image(seed: bytes, width: int = 16, height: int = 16) -> Image:
    """A deterministic test image derived from ``seed`` (used when a mock gets a non-image / absent input)."""
    h = hashlib.sha256(b"fuse_ai.synthetic_image\0" + seed).digest()
    r0, g0, b0, r1, g1, b1 = h[0], h[1], h[2], h[3], h[4], h[5]
    cell = 2 + (h[6] % 4)
    rgba = bytearray()
    for y in range(height):
        for x in range(width):
            t = (x * 255) // max(1, width - 1)
            check = ((x // cell) + (y // cell)) & 1
            r = (r0 * (255 - t) + r1 * t) // 255
            g = (g0 * (255 - t) + g1 * t) // 255
            b = (b0 * (255 - t) + b1 * t) // 255
            if check:
                r, g, b = r // 2, g // 2, b // 2
            rgba += bytes((r, g, b, 255))
    return Image(width, height, bytes(rgba))


def image_from_input(data: bytes | None, key: str) -> Image:
    """Decode a PNG input, or synthesise a deterministic image from the input's bytes / content hash."""
    if data is not None and is_png(data):
        return decode_png(data)
    return synthetic_image(data if data is not None else key.encode("utf-8"))


# ---- WAV (16-bit PCM) -----------------------------------------------------------------------------------------


def encode_wav(samples: List[int], rate: int, channels: int = 1) -> bytes:
    pcm = struct.pack("<%dh" % len(samples), *[max(-32768, min(32767, s)) for s in samples])
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * 2, channels * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm
    return b"RIFF" + struct.pack("<I", len(body)) + body


def decode_wav(data: bytes) -> Tuple[List[int], int, int]:
    """Returns (interleaved int16 samples, rate, channels). Raises RecipeError on anything but 16-bit PCM."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise RecipeError("not a WAV file")
    pos = 12
    rate = channels = bits = 0
    pcm = b""
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]
        (size,) = struct.unpack("<I", data[pos + 4:pos + 8])
        payload = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
        if tag == b"fmt ":
            fmt_tag, channels, rate, _br, _ba, bits = struct.unpack("<HHIIHH", payload[:16])
            if fmt_tag != 1 or bits != 16:
                raise RecipeError("mock WAV decoder supports 16-bit PCM only")
        elif tag == b"data":
            pcm = payload
    n = len(pcm) // 2
    return list(struct.unpack("<%dh" % n, pcm[:n * 2])), rate, channels


def synthetic_audio(seed: bytes, rate: int = 16000, ms: int = 100) -> List[int]:
    """Deterministic triangle-wave audio (integer only) derived from ``seed``."""
    h = hashlib.sha256(b"fuse_ai.synthetic_audio\0" + seed).digest()
    period = 20 + h[0] % 60
    amp = 4000 + h[1] * 40
    n = rate * ms // 1000
    out = []
    for i in range(n):
        ph = i % period
        half = period // 2
        v = (ph * 2 * amp) // half - amp if ph < half else amp - ((ph - half) * 2 * amp) // (period - half)
        out.append(v)
    return out
