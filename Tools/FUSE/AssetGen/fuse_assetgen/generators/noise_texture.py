"""noise_texture: tileable fBm value-noise height map (16-bit grey PNG) + optional tinted albedo (RGB8).

Reference generator for the skeleton: exercises seed streams (one derived stream per octave),
parameter schemas and multi-output emission. Pure integer/float arithmetic in a fixed order, so the
bytes are identical on every platform with IEEE-754 binary64.
"""

from __future__ import annotations

import math

from ..pngio import encode_png
from ..registry import generator

PARAMS = {
    "size": {"type": "int", "enum": [16, 32, 64, 128, 256, 512], "default": 64},
    "octaves": {"type": "int", "minimum": 1, "maximum": 8, "default": 4},
    "base_cells": {"type": "int", "minimum": 1, "maximum": 64, "default": 4},
    "gain": {"type": "number", "minimum": 0.05, "maximum": 0.95, "default": 0.5},
    "albedo": {
        "type": "object",
        "properties": {
            "enabled": {"type": "bool", "default": True},
            # Linear-light endpoints; §1.6 keeps albedo within 0.02..0.90 (checked by fuse_assetcheck).
            "low": {"type": "array", "items": {"type": "number", "minimum": 0.02, "maximum": 0.9},
                    "min_items": 3, "max_items": 3, "default": [0.08, 0.07, 0.06]},
            "high": {"type": "array", "items": {"type": "number", "minimum": 0.02, "maximum": 0.9},
                     "min_items": 3, "max_items": 3, "default": [0.45, 0.42, 0.38]},
        },
        "default": {"enabled": True, "low": [0.08, 0.07, 0.06], "high": [0.45, 0.42, 0.38]},
    },
}


def _lattice(stream, cells: int):
    rng = stream.rng()
    return [rng.uniform() for _ in range(cells * cells)]


def _smooth(t: float) -> float:
    return t * t * (3.0 - 2.0 * t)


def _sample(lattice, cells: int, u: float, v: float) -> float:
    x = u * cells
    y = v * cells
    xi = int(x)
    yi = int(y)
    fx = _smooth(x - xi)
    fy = _smooth(y - yi)
    x0, x1 = xi % cells, (xi + 1) % cells  # wrap: tileable
    y0, y1 = yi % cells, (yi + 1) % cells
    a = lattice[y0 * cells + x0]
    b = lattice[y0 * cells + x1]
    c = lattice[y1 * cells + x0]
    d = lattice[y1 * cells + x1]
    top = a + (b - a) * fx
    bot = c + (d - c) * fx
    return top + (bot - top) * fy


def _pow_5_12(x: float) -> float:
    """x ** (1/2.4) for x in (0, 1] using only IEEE-exact ops (+ - * / sqrt): libm pow() may differ
    by an ulp between platforms, which would flip an 8-bit rounding now and then."""
    y = 1.0
    for _ in range(64):  # Newton for cbrt(x), monotone from above; fixed count keeps it deterministic
        y = y - (y * y * y - x) / (3.0 * y * y)
    t = math.sqrt(math.sqrt(y))  # x^(1/12); IEEE sqrt is correctly rounded (pow(y, 0.5) need not be)
    return t * t * t * t * t


def _srgb_encode(c: float) -> int:
    c = 0.0 if c < 0.0 else (1.0 if c > 1.0 else c)
    s = c * 12.92 if c <= 0.0031308 else 1.055 * _pow_5_12(c) - 0.055
    return int(s * 255.0 + 0.5)


@generator("noise_texture", version=1, params=PARAMS,
           description="Tileable fBm value-noise height map (+ tinted albedo), 16-bit grey / RGB8 PNG")
def generate(ctx) -> None:
    p = ctx.params
    size = p["size"]
    octaves = []
    cells = p["base_cells"]
    amp = 1.0
    total = 0.0
    for o in range(p["octaves"]):
        octaves.append((cells, amp, _lattice(ctx.stream.derive("octave", o), cells)))
        total += amp
        cells *= 2
        amp *= p["gain"]
    height = [0.0] * (size * size)
    for y in range(size):
        v = (y + 0.5) / size
        for x in range(size):
            u = (x + 0.5) / size
            h = 0.0
            for cells_o, amp_o, lat in octaves:
                h += amp_o * _sample(lat, cells_o, u, v)
            height[y * size + x] = h / total
    ctx.emit("height.png", encode_png(size, size, 1, [int(h * 65535.0 + 0.5) for h in height], bit_depth=16))
    alb = p["albedo"]
    if alb["enabled"]:
        lo, hi = alb["low"], alb["high"]
        rgb = []
        for h in height:
            for c in range(3):
                rgb.append(_srgb_encode(lo[c] + (hi[c] - lo[c]) * h))
        ctx.emit("albedo.png", encode_png(size, size, 3, rgb))
