"""Float RGB(A) images, colour maths, metrics and synthetic legacy textures (pure Python; W3.1, W3.4, W3.5).

Small and slow on purpose: everything here runs on ≤ 256² images in CI without numpy (fuse_ai is stdlib
only). Values are linear floats in [0, 1] per channel unless a function says otherwise.

* :class:`FImage` - planar float image (r, g, b, a lists), conversion from/to :class:`fuse_ai.imaging.Image`.
* filters: wrap-around or clamped box blur, area downsample, bilinear resize.
* colour: sRGB <-> linear, luma, CIELAB, ΔE*ab (CIE76), HyAB.
* metrics: PSNR, mean SSIM (8×8 windows), HyAB mean (the "flip" tolerance metric, see :mod:`fuse_ai.fingerprint`).
* synthetic textures (:func:`gen_texture`): albedo families (noise, bricks, planks, tiles, weave, speckle),
  tiling or not, normal maps from height fields, lightmaps, UI atlases, foliage cut-outs, baked light.
  Every generator is a pure function of a :class:`random.Random` seed.
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Sequence, Tuple

from .imaging import Image

Vec3 = Tuple[float, float, float]


@dataclass
class FImage:
    w: int
    h: int
    r: List[float]
    g: List[float]
    b: List[float]
    a: List[float] = field(default_factory=list)  # empty == opaque

    @classmethod
    def blank(cls, w: int, h: int, colour: Vec3 = (0.0, 0.0, 0.0)) -> "FImage":
        n = w * h
        return cls(w, h, [colour[0]] * n, [colour[1]] * n, [colour[2]] * n)

    @property
    def n(self) -> int:
        return self.w * self.h

    @property
    def alpha(self) -> List[float]:
        return self.a if self.a else [1.0] * self.n

    def channels(self) -> Tuple[List[float], List[float], List[float]]:
        return self.r, self.g, self.b

    def copy(self) -> "FImage":
        return FImage(self.w, self.h, list(self.r), list(self.g), list(self.b), list(self.a))

    def map(self, fn: Callable[[float, float, float], Vec3]) -> "FImage":
        r, g, b = [], [], []
        for x, y, z in zip(self.r, self.g, self.b):
            p = fn(x, y, z)
            r.append(p[0]); g.append(p[1]); b.append(p[2])
        return FImage(self.w, self.h, r, g, b, list(self.a))

    def clamp(self) -> "FImage":
        return self.map(lambda x, y, z: (min(1.0, max(0.0, x)), min(1.0, max(0.0, y)), min(1.0, max(0.0, z))))

    def luma(self) -> List[float]:
        return [0.2126 * x + 0.7152 * y + 0.0722 * z for x, y, z in zip(self.r, self.g, self.b)]

    def to_image(self) -> Image:
        out = bytearray()
        al = self.alpha
        for x, y, z, a in zip(self.r, self.g, self.b, al):
            out += bytes((_q(x), _q(y), _q(z), _q(a)))
        return Image(self.w, self.h, bytes(out))

    @classmethod
    def from_image(cls, img: Image) -> "FImage":
        px = img.rgba
        r = [px[i] / 255.0 for i in range(0, len(px), 4)]
        g = [px[i + 1] / 255.0 for i in range(0, len(px), 4)]
        b = [px[i + 2] / 255.0 for i in range(0, len(px), 4)]
        a = [px[i + 3] / 255.0 for i in range(0, len(px), 4)]
        if all(v >= 1.0 for v in a):
            a = []
        return cls(img.width, img.height, r, g, b, a)

    def quantised(self) -> "FImage":
        """Round-trip through 8 bits (what a stored texture holds)."""
        return FImage.from_image(self.to_image())


def _q(v: float) -> int:
    return 0 if v <= 0.0 else 255 if v >= 1.0 else int(v * 255.0 + 0.5)


# ---- filters ------------------------------------------------------------------------------------------------------


def box_blur(ch: List[float], w: int, h: int, radius: int, wrap: bool = True) -> List[float]:
    if radius <= 0:
        return list(ch)
    k = 2 * radius + 1
    tmp = [0.0] * (w * h)
    for y in range(h):
        row = ch[y * w:(y + 1) * w]
        for x in range(w):
            s = 0.0
            for d in range(-radius, radius + 1):
                xx = (x + d) % w if wrap else min(w - 1, max(0, x + d))
                s += row[xx]
            tmp[y * w + x] = s / k
    out = [0.0] * (w * h)
    for x in range(w):
        for y in range(h):
            s = 0.0
            for d in range(-radius, radius + 1):
                yy = (y + d) % h if wrap else min(h - 1, max(0, y + d))
                s += tmp[yy * w + x]
            out[y * w + x] = s / k
    return out


def downsample_area(ch: List[float], w: int, h: int, ow: int, oh: int) -> List[float]:
    """Exact area average when w, h are multiples of ow, oh (otherwise nearest-cell average)."""
    out = [0.0] * (ow * oh)
    cnt = [0] * (ow * oh)
    for y in range(h):
        oy = y * oh // h
        for x in range(w):
            ox = x * ow // w
            out[oy * ow + ox] += ch[y * w + x]
            cnt[oy * ow + ox] += 1
    return [s / c for s, c in zip(out, cnt)]


def resize_bilinear(ch: List[float], w: int, h: int, ow: int, oh: int) -> List[float]:
    out = [0.0] * (ow * oh)
    for oy in range(oh):
        fy = (oy + 0.5) * h / oh - 0.5
        y0 = max(0, min(h - 1, int(math.floor(fy))))
        y1 = min(h - 1, y0 + 1)
        ty = min(1.0, max(0.0, fy - y0))
        for ox in range(ow):
            fx = (ox + 0.5) * w / ow - 0.5
            x0 = max(0, min(w - 1, int(math.floor(fx))))
            x1 = min(w - 1, x0 + 1)
            tx = min(1.0, max(0.0, fx - x0))
            a = ch[y0 * w + x0] * (1 - tx) + ch[y0 * w + x1] * tx
            b = ch[y1 * w + x0] * (1 - tx) + ch[y1 * w + x1] * tx
            out[oy * ow + ox] = a * (1 - ty) + b * ty
    return out


def resize(img: FImage, ow: int, oh: int, method: str = "bilinear") -> FImage:
    if method == "area":
        f = lambda ch: downsample_area(ch, img.w, img.h, ow, oh)  # noqa: E731
    else:
        f = lambda ch: resize_bilinear(ch, img.w, img.h, ow, oh)  # noqa: E731
    return FImage(ow, oh, f(img.r), f(img.g), f(img.b), f(img.a) if img.a else [])


# ---- colour -------------------------------------------------------------------------------------------------------


def srgb_to_linear(v: float) -> float:
    v = min(1.0, max(0.0, v))
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def linear_to_srgb(v: float) -> float:
    v = min(1.0, max(0.0, v))
    return v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055


def srgb_to_lab(c: Sequence[float]) -> Vec3:
    """sRGB-encoded [0,1] -> CIELAB (D65)."""
    r, g, b = (srgb_to_linear(x) for x in c[:3])
    x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047
    y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b
    z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883

    def f(t: float) -> float:
        return t ** (1 / 3) if t > 216 / 24389 else (24389 / 27 * t + 16) / 116

    fx, fy, fz = f(x), f(y), f(z)
    return 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)


def delta_e76(a: Sequence[float], b: Sequence[float]) -> float:
    la, lb = srgb_to_lab(a), srgb_to_lab(b)
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(la, lb)))


def hyab(a: Sequence[float], b: Sequence[float]) -> float:
    la, lb = srgb_to_lab(a), srgb_to_lab(b)
    return abs(la[0] - lb[0]) + math.hypot(la[1] - lb[1], la[2] - lb[2])


def saturation(r: float, g: float, b: float) -> float:
    mx, mn = max(r, g, b), min(r, g, b)
    return 0.0 if mx <= 1e-6 else (mx - mn) / mx


# ---- metrics ------------------------------------------------------------------------------------------------------


def psnr(a: FImage, b: FImage) -> float:
    if (a.w, a.h) != (b.w, b.h):
        raise ValueError("psnr: size mismatch")
    se = 0.0
    for ca, cb in zip(a.channels(), b.channels()):
        se += sum((x - y) ** 2 for x, y in zip(ca, cb))
    mse = se / (3 * a.n)
    return float("inf") if mse <= 0 else 10 * math.log10(1.0 / mse)


def ssim(a: FImage, b: FImage, win: int = 8, step: int = 4) -> float:
    """Mean SSIM of the luma over win×win windows (C1, C2 per Wang et al. 2004, L = 1)."""
    la, lb = a.luma(), b.luma()
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    vals = []
    for y0 in range(0, max(1, a.h - win + 1), step):
        for x0 in range(0, max(1, a.w - win + 1), step):
            xs = [la[(y0 + y) * a.w + x0 + x] for y in range(min(win, a.h)) for x in range(min(win, a.w))]
            ys = [lb[(y0 + y) * a.w + x0 + x] for y in range(min(win, a.h)) for x in range(min(win, a.w))]
            n = len(xs)
            mx, my = sum(xs) / n, sum(ys) / n
            vx = sum((v - mx) ** 2 for v in xs) / n
            vy = sum((v - my) ** 2 for v in ys) / n
            cov = sum((p - mx) * (q - my) for p, q in zip(xs, ys)) / n
            vals.append(((2 * mx * my + c1) * (2 * cov + c2)) / ((mx * mx + my * my + c1) * (vx + vy + c2)))
    return sum(vals) / len(vals)


def hyab_mean(a: FImage, b: FImage) -> float:
    """Mean HyAB colour difference normalised to [0, 1] by 100 (a colour-only FLIP stand-in, [SIMPLIFIED])."""
    tot = 0.0
    for p in zip(a.r, a.g, a.b, b.r, b.g, b.b):
        tot += hyab(p[:3], p[3:])
    return tot / a.n / 100.0


# ---- synthetic textures -------------------------------------------------------------------------------------------


def value_noise(rng: random.Random, w: int, h: int, cells: int, periodic: bool = True) -> List[float]:
    """Smooth value noise with ``cells`` lattice cells across the image. Periodic noise wraps exactly; the
    non-periodic variant uses an independent lattice column/row at the far edge (so the seam does not match)."""
    gw, gh = cells + 1, cells + 1
    lat = [[rng.random() for _ in range(gw)] for _ in range(gh)]
    if periodic:
        for j in range(gh):
            lat[j][cells] = lat[j][0]
        for i in range(gw):
            lat[cells][i] = lat[0][i]
    out = [0.0] * (w * h)
    for y in range(h):
        fy = y * cells / h
        j = int(fy)
        ty = fy - j
        ty = ty * ty * (3 - 2 * ty)
        for x in range(w):
            fx = x * cells / w
            i = int(fx)
            tx = fx - i
            tx = tx * tx * (3 - 2 * tx)
            a = lat[j][i] * (1 - tx) + lat[j][i + 1] * tx
            b = lat[j + 1][i] * (1 - tx) + lat[j + 1][i + 1] * tx
            out[y * w + x] = a * (1 - ty) + b * ty
    return out


def fbm(rng: random.Random, w: int, h: int, base_cells: int = 2, octaves: int = 4, periodic: bool = True,
        gain: float = 0.5) -> List[float]:
    out = [0.0] * (w * h)
    amp, norm, cells = 1.0, 0.0, base_cells
    for _ in range(octaves):
        cells_eff = min(cells, w)
        n = value_noise(rng, w, h, cells_eff, periodic)
        for k in range(w * h):
            out[k] += amp * n[k]
        norm += amp
        amp *= gain
        cells *= 2
    return [v / norm for v in out]


def _palette(rng: random.Random, hue: Optional[float] = None, sat: Optional[float] = None) -> Tuple[Vec3, Vec3]:
    import colorsys

    h = rng.random() if hue is None else hue
    s = rng.uniform(0.15, 0.7) if sat is None else sat
    v1, v2 = rng.uniform(0.25, 0.5), rng.uniform(0.55, 0.9)
    c1 = colorsys.hsv_to_rgb(h, s, v1)
    c2 = colorsys.hsv_to_rgb((h + rng.uniform(-0.04, 0.04)) % 1.0, s * rng.uniform(0.7, 1.1), v2)
    return c1, c2


def _lerp_img(t: List[float], c1: Vec3, c2: Vec3, w: int, h: int) -> FImage:
    return FImage(w, h, [c1[0] + (c2[0] - c1[0]) * v for v in t], [c1[1] + (c2[1] - c1[1]) * v for v in t],
                  [c1[2] + (c2[2] - c1[2]) * v for v in t])


ALBEDO_KINDS = ("noise", "bricks", "planks", "tiles", "weave", "speckle")


def albedo_pattern(rng: random.Random, kind: str, w: int, h: int, periodic: bool) -> List[float]:
    """A [0,1] pattern field for one of :data:`ALBEDO_KINDS`."""
    n = fbm(rng, w, h, base_cells=rng.choice((2, 4)), octaves=4, periodic=periodic)
    # A non-tiling photo-like texture: the structure does not fit the image a whole number of times.
    sx = 1.0 if periodic else rng.uniform(1.15, 1.45)
    sy = 1.0 if periodic else rng.uniform(1.15, 1.45)
    ox, oy = rng.randrange(w), rng.randrange(h)  # structure phase: the seam is not always on a mortar line
    U = [int(x * sx) + ox for x in range(w)]
    V = [int(y * sy) + oy for y in range(h)]
    if kind == "noise":
        return [min(1.0, max(0.0, 0.5 + (v - 0.5) * 2.2)) for v in n]
    if kind == "speckle":
        fine = value_noise(rng, w, h, min(w, 32), periodic)
        return [min(1.0, max(0.0, 0.35 * v + 0.65 * f + (0.25 if f > 0.8 else 0))) for v, f in zip(n, fine)]
    if kind == "bricks":
        rows = rng.choice((4, 8))
        cols = rows // 2
        bh, bw = h // rows, w // cols
        jitter = [[rng.uniform(-0.25, 0.25) for _ in range(2 * cols + 2)] for _ in range(2 * rows + 2)]
        out = []
        for y in range(h):
            v = V[y]
            r = (v // bh) % (rows if periodic else len(jitter))
            off = (bw // 2) if r % 2 else 0
            for x in range(w):
                xx = (U[x] + off) % (w if periodic else 4 * w)
                c = (xx // bw) % len(jitter[0])
                mortar = (v % bh) < 2 or (xx % bw) < 2
                out.append(0.12 + 0.1 * n[y * w + x] if mortar else
                           min(1.0, max(0.0, 0.6 + jitter[r][c] + 0.35 * (n[y * w + x] - 0.5))))
        return out
    if kind == "planks":
        planks = rng.choice((2, 4, 8))
        ph = h // planks
        freq = rng.choice((3, 5, 7))
        jit = [rng.uniform(-0.15, 0.15) for _ in range(2 * planks + 2)]
        out = []
        for y in range(h):
            p = (V[y] // ph) % (planks if periodic else len(jit))
            for x in range(w):
                grain = 0.5 + 0.5 * math.sin(2 * math.pi * (freq * U[x] / w + 1.5 * n[y * w + x]))
                seam = (V[y] % ph) < 1
                out.append(0.1 if seam else min(1.0, max(0.0, 0.45 + jit[p] + 0.35 * grain)))
        return out
    if kind == "tiles":
        t = rng.choice((4, 8))
        ts = w // t
        jit = [[rng.uniform(-0.12, 0.12) for _ in range(2 * t + 2)] for _ in range(2 * t + 2)]
        out = []
        for y in range(h):
            for x in range(w):
                grout = (U[x] % ts) < 2 or (V[y] % ts) < 2
                tj = jit[(V[y] // ts) % (t if periodic else len(jit))][(U[x] // ts) % (t if periodic else len(jit))]
                out.append(0.85 if grout else min(1.0, max(0.0, 0.45 + tj
                                                              + 0.15 * (n[y * w + x] - 0.5))))
        return out
    if kind == "weave":
        p = rng.choice((4, 8))
        out = []
        for y in range(h):
            for x in range(w):
                over = ((U[x] // p) + (V[y] // p)) % 2
                ph = (V[y] % p) / p if over else (U[x] % p) / p
                out.append(min(1.0, max(0.0, 0.3 + 0.5 * math.sin(math.pi * ph) + 0.2 * (n[y * w + x] - 0.5))))
        return out
    raise ValueError(f"unknown albedo kind {kind!r}")


def gen_albedo(rng: random.Random, size: int = 64, kind: Optional[str] = None, periodic: bool = True,
               palette: Optional[Tuple[Vec3, Vec3]] = None) -> Tuple[FImage, str]:
    kind = kind or rng.choice(ALBEDO_KINDS)
    t = albedo_pattern(rng, kind, size, size, periodic)
    if not periodic:
        # a non-tiling texture: low-frequency content that does not continue across the edges
        gx, gy = rng.uniform(-0.35, 0.35), rng.uniform(-0.35, 0.35)
        t = [min(1.0, max(0.0, v + gx * (x / size - 0.5) + gy * (y / size - 0.5)))
             for y in range(size) for x in range(size) for v in (t[y * size + x],)]
    c1, c2 = palette or _palette(rng)
    return _lerp_img(t, c1, c2, size, size), kind


def gen_normal_map(rng: random.Random, size: int = 64, periodic: bool = True) -> FImage:
    hgt = albedo_pattern(rng, rng.choice(ALBEDO_KINDS), size, size, periodic)
    if not periodic:
        hgt = [v + 0.3 * (x / size) for y in range(size) for x in range(size) for v in (hgt[y * size + x],)]
    strength = rng.uniform(1.5, 6.0)
    flip_g = rng.random() < 0.3  # DirectX-style maps
    r, g, b = [], [], []
    for y in range(size):
        for x in range(size):
            if periodic:
                xl, xr, yu, yd = (x - 1) % size, (x + 1) % size, (y - 1) % size, (y + 1) % size
            else:
                xl, xr, yu, yd = max(0, x - 1), min(size - 1, x + 1), max(0, y - 1), min(size - 1, y + 1)
            dx = (hgt[y * size + xr] - hgt[y * size + xl]) * strength
            dy = (hgt[yd * size + x] - hgt[yu * size + x]) * strength
            nx, ny, nz = -dx, -dy, 1.0
            ln = math.sqrt(nx * nx + ny * ny + nz * nz)
            nx, ny, nz = nx / ln, ny / ln, nz / ln
            if flip_g:
                ny = -ny
            r.append(nx * 0.5 + 0.5); g.append(ny * 0.5 + 0.5); b.append(nz * 0.5 + 0.5)
    return FImage(size, size, r, g, b)


def light_field(rng: random.Random, size: int, lo: float, hi: float) -> List[float]:
    """Smooth, non-periodic illumination: a few soft light pools, a soft shadow and a gradient."""
    lights = [(rng.uniform(0, size), rng.uniform(0, size), rng.uniform(size * 0.15, size * 0.45),
               rng.uniform(0.4, 1.0)) for _ in range(rng.randint(1, 3))]
    sx0, sy0 = rng.uniform(0, size * 0.6), rng.uniform(0, size * 0.6)
    sw, sh = rng.uniform(size * 0.2, size * 0.5), rng.uniform(size * 0.2, size * 0.5)
    gx = rng.uniform(-0.3, 0.3)
    raw = []
    for y in range(size):
        for x in range(size):
            v = 0.15 + gx * (x / size - 0.5)
            for lx, ly, rad, inten in lights:
                d2 = (x - lx) ** 2 + (y - ly) ** 2
                v += inten * math.exp(-d2 / (2 * rad * rad))
            raw.append(v)
    shadow = [0.0] * (size * size)
    for y in range(size):
        for x in range(size):
            if sx0 <= x < sx0 + sw and sy0 <= y < sy0 + sh:
                shadow[y * size + x] = 1.0
    shadow = box_blur(shadow, size, size, max(1, size // 16), wrap=False)
    raw = [v * (1 - 0.6 * s) for v, s in zip(raw, shadow)]
    mn, mx = min(raw), max(raw)
    return [lo + (hi - lo) * (v - mn) / max(1e-6, mx - mn) for v in raw]


def gen_lightmap(rng: random.Random, size: int = 64) -> FImage:
    lf = light_field(rng, size, rng.uniform(0.02, 0.15), rng.uniform(0.6, 1.0))
    tint = rng.choice(((1.0, 1.0, 1.0), (1.0, 0.92, 0.8), (0.85, 0.9, 1.0), (1.0, 0.97, 0.9)))
    img = FImage(size, size, [v * tint[0] for v in lf], [v * tint[1] for v in lf], [v * tint[2] for v in lf])
    if rng.random() < 0.5:  # legacy lightmaps are often stored small and magnified
        small = resize(img, size // 4, size // 4, "area")
        img = resize(small, size, size, "bilinear")
    return img


def gen_baked(rng: random.Random, size: int = 64) -> FImage:
    """An albedo texture with lighting baked into it (multiplied by a light field)."""
    alb, _ = gen_albedo(rng, size, periodic=rng.random() < 0.5)
    lf = light_field(rng, size, rng.uniform(0.25, 0.5), rng.uniform(1.0, 1.25))
    return FImage(size, size, [v * l for v, l in zip(alb.r, lf)], [v * l for v, l in zip(alb.g, lf)],
                  [v * l for v, l in zip(alb.b, lf)]).clamp()


def gen_ui(rng: random.Random, size: int = 64) -> FImage:
    """A UI/HUD atlas: flat panels with borders, icons and 'text' strokes, usually on a transparent background."""
    import colorsys

    transparent = rng.random() < 0.7
    bg = (0.0, 0.0, 0.0) if transparent else colorsys.hsv_to_rgb(rng.random(), rng.uniform(0.2, 0.8),
                                                                    rng.uniform(0.1, 0.4))
    img = FImage.blank(size, size, bg)
    alpha = [0.0 if transparent else 1.0] * (size * size)

    def rect(x0: int, y0: int, x1: int, y1: int, c: Vec3) -> None:
        for y in range(max(0, y0), min(size, y1)):
            for x in range(max(0, x0), min(size, x1)):
                k = y * size + x
                img.r[k], img.g[k], img.b[k] = c
                alpha[k] = 1.0

    for _ in range(rng.randint(2, 4)):
        x0, y0 = rng.randrange(0, size - 12), rng.randrange(0, size - 8)
        x1, y1 = min(size, x0 + rng.randint(10, 40)), min(size, y0 + rng.randint(6, 24))
        border = colorsys.hsv_to_rgb(rng.random(), rng.uniform(0.3, 1.0), rng.uniform(0.7, 1.0))
        fill = colorsys.hsv_to_rgb(rng.random(), rng.uniform(0.3, 1.0), rng.uniform(0.3, 0.8))
        rect(x0, y0, x1, y1, border)
        rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, fill)
        ink = (1.0, 1.0, 1.0) if rng.random() < 0.6 else (0.0, 0.0, 0.0)
        tx = x0 + 3
        while tx < x1 - 4:  # 'text': vertical and horizontal strokes
            gh = rng.randint(2, max(2, min(5, y1 - y0 - 3)))
            rect(tx, y0 + 2, tx + 1, y0 + 2 + gh, ink)
            if rng.random() < 0.5:
                rect(tx, y0 + 2, tx + 2, y0 + 3, ink)
            tx += rng.randint(2, 4)
    for _ in range(rng.randint(0, 3)):  # round icons
        cx, cy, rad = rng.randrange(6, size - 6), rng.randrange(6, size - 6), rng.randint(3, 6)
        c = colorsys.hsv_to_rgb(rng.random(), 0.9, 1.0)
        for y in range(cy - rad, cy + rad + 1):
            for x in range(cx - rad, cx + rad + 1):
                if 0 <= x < size and 0 <= y < size and (x - cx) ** 2 + (y - cy) ** 2 <= rad * rad:
                    k = y * size + x
                    img.r[k], img.g[k], img.b[k] = c
                    alpha[k] = 1.0
    img.a = alpha if transparent else []
    return img


def gen_foliage(rng: random.Random, size: int = 64) -> FImage:
    """An alpha-tested foliage card: noisy leaves on a transparent background (a hard negative for UI)."""
    alb, _ = gen_albedo(rng, size, kind="noise", palette=_palette(rng, hue=rng.uniform(0.2, 0.4), sat=0.6))
    mask = value_noise(rng, size, size, 8, True)
    alb.a = [1.0 if m > 0.5 else 0.0 for m in mask]
    return alb


def gen_blue_albedo(rng: random.Random, size: int = 64) -> FImage:
    """Water / sky-like blue textures (hard negatives for normal-map detection)."""
    pal = _palette(rng, hue=rng.uniform(0.55, 0.68), sat=rng.uniform(0.3, 0.6))
    img, _ = gen_albedo(rng, size, kind=rng.choice(("noise", "speckle")), palette=pal)
    return img
