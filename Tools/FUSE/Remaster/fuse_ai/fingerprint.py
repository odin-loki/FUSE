"""W3.5: recipe rebuild with tolerance + perceptual fingerprints (plan §3.3 "Determinism").

GPU and cloud outputs are not bit-stable, so a recipe carries ``output.tolerance`` and a rebuild on the user's
machine passes when it is close enough to the author's result. Two references are possible:

* the **author's image** (kept only in the author's cache, never shipped when it is a derivative): the
  rebuild is compared with the recipe's metric - ``flip`` (mean HyAB colour difference / 100, a colour-only
  FLIP stand-in **[SIMPLIFIED: no CSF filtering]**), ``psnr`` (min dB), ``ssim`` (min) or ``mae`` (max, 0-1);
* the **fingerprint** shipped in the patch instead (``output.fingerprint``), which identifies an output
  without reproducing it.

**Fingerprint design (deviation from the §3.3 sketch).** The plan sketched "a 16x16 downsample plus a
histogram" [SPECULATIVE: adequacy]. Measured here: a bilinear upsample of that 16x16 thumbnail reconstructs
256-pixel legacy-style textures at roughly 18-38 dB PSNR, i.e. it *does* reproduce the derivative at low
frequency (:func:`thumbnail16_psnr` keeps that measurement in the tests). A 64-bit DCT perceptual hash was
tried next and still leaked 2-4 dB of layout. ``fuse.ai-fingerprint/1`` therefore keeps **no phase / layout
information at all**:

* ``spectrum16_db`` - the radially averaged power spectrum of a 32x32 luma downsample in 16 bands (dB):
  how much detail at which scale, not where;
* ``orient8_permille`` - the histogram of gradient orientations (8 bins, magnitude-weighted): bricks vs
  planks vs noise, not where;
* ``luma_hist32_permille`` - the tone distribution;
* ``lab_mean_q`` - the mean CIELAB colour, rounded to 2 units.

A fingerprint necessarily leaks the mean colour, so a flat mean-colour image is the floor of any
reconstruction; the < 15 dB gate therefore holds for textures with real contrast and cannot hold for nearly
flat ones, which the fingerprint reveals no more than their mean colour does. :func:`attack_psnr` runs the
strongest reconstructions we know from a fingerprint alone (the flat mean colour; random-phase synthesis
matched to the spectrum, the tone histogram and the colour) and the gate takes the best of them.
"""

from __future__ import annotations

import hashlib
import math
from typing import Any, Dict, List, Optional

from . import fimage as F
from .errors import GateFailure, RecipeError
from .fimage import FImage
from .imaging import decode_png, is_png

FINGERPRINT_SCHEMA = "fuse.ai-fingerprint/1"
HIST_BINS = 32
SPEC_N = 32
SPEC_BANDS = 16
DEFAULT_FP_TOLERANCE = {"max_spectrum_db": 2.0, "max_orient_l1": 0.15, "max_hist_l1": 0.15, "max_lab_mean_de": 4.0}


def _img(data: bytes) -> FImage:
    if not is_png(data):
        raise RecipeError("fingerprints and tolerance metrics need PNG image outputs")
    return FImage.from_image(decode_png(data))


def _dft2_power(lum: List[float], n: int) -> List[float]:
    """|DFT|^2 of an n x n real block (separable, pure Python; n = 32)."""
    tw = [complex(math.cos(-2 * math.pi * k / n), math.sin(-2 * math.pi * k / n)) for k in range(n)]
    rows = [[sum(lum[y * n + x] * tw[(u * x) % n] for x in range(n)) for u in range(n)] for y in range(n)]
    power = [0.0] * (n * n)
    for u in range(n):
        for v in range(n):
            c = sum(rows[y][u] * tw[(v * y) % n] for y in range(n))
            power[v * n + u] = (c.real * c.real + c.imag * c.imag) / (n * n)
    return power


def _band(fu: float, fv: float) -> int:
    return min(SPEC_BANDS - 1, int(math.hypot(fu, fv) / (SPEC_N // 2 * 1.4143) * SPEC_BANDS))


def spectrum_bands(img: FImage) -> List[float]:
    small = F.downsample_area(img.luma(), img.w, img.h, SPEC_N, SPEC_N)
    mean = sum(small) / len(small)
    power = _dft2_power([v - mean for v in small], SPEC_N)
    bands = [0.0] * SPEC_BANDS
    counts = [0] * SPEC_BANDS
    half = SPEC_N // 2
    for v in range(SPEC_N):
        for u in range(SPEC_N):
            fu, fv = (u if u <= half else u - SPEC_N), (v if v <= half else v - SPEC_N)
            if fu == 0 and fv == 0:
                continue
            k = _band(fu, fv)
            bands[k] += power[v * SPEC_N + u]
            counts[k] += 1
    return [10 * math.log10(b / c + 1e-9) if c else -90.0 for b, c in zip(bands, counts)]


def orientation_hist(img: FImage) -> List[int]:
    lum = img.luma()
    w, h = img.w, img.h
    hist = [0.0] * 8
    for y in range(h):
        for x in range(w):
            gx = lum[y * w + (x + 1) % w] - lum[y * w + (x - 1) % w]
            gy = lum[((y + 1) % h) * w + x] - lum[((y - 1) % h) * w + x]
            mag = math.hypot(gx, gy)
            if mag > 1e-6:
                ang = math.atan2(gy, gx) % math.pi  # orientation, not direction
                hist[min(7, int(ang / math.pi * 8))] += mag
    tot = sum(hist) or 1.0
    return [round(1000 * v / tot) for v in hist]


def fingerprint(img: FImage) -> Dict[str, Any]:
    hist = [0] * HIST_BINS
    for v in img.luma():
        hist[min(HIST_BINS - 1, int(v * HIST_BINS))] += 1
    labs = [F.srgb_to_lab(p) for p in zip(img.r, img.g, img.b)]
    mean = [sum(lab[i] for lab in labs) / len(labs) for i in range(3)]
    return {"schema": FINGERPRINT_SCHEMA, "width": img.w, "height": img.h,
            "spectrum16_db": [round(v, 1) for v in spectrum_bands(img)],
            "orient8_permille": orientation_hist(img),
            "luma_hist32_permille": [round(1000 * c / img.n) for c in hist],
            "lab_mean_q": [round(v / 2) * 2 for v in mean]}


def compare(fp_a: Dict[str, Any], fp_b: Dict[str, Any]) -> Dict[str, float]:
    if (fp_a["width"], fp_a["height"]) != (fp_b["width"], fp_b["height"]):
        inf = float("inf")
        return {"spectrum_db": inf, "orient_l1": inf, "hist_l1": inf, "lab_mean_de": inf}
    spec = sum(abs(a - b) for a, b in zip(fp_a["spectrum16_db"], fp_b["spectrum16_db"])) / SPEC_BANDS
    ori = sum(abs(a - b) for a, b in zip(fp_a["orient8_permille"], fp_b["orient8_permille"])) / 1000.0
    l1 = sum(abs(a - b) for a, b in zip(fp_a["luma_hist32_permille"], fp_b["luma_hist32_permille"])) / 1000.0
    de = math.sqrt(sum((a - b) ** 2 for a, b in zip(fp_a["lab_mean_q"], fp_b["lab_mean_q"])))
    return {"spectrum_db": round(spec, 4), "orient_l1": ori, "hist_l1": l1, "lab_mean_de": de}


def _lab_to_srgb(lab: List[float]) -> List[float]:
    L, a, b = lab
    fy = (L + 16) / 116
    fx, fz = fy + a / 500, fy - b / 200

    def finv(t: float) -> float:
        return t ** 3 if t ** 3 > 216 / 24389 else (116 * t - 16) / (24389 / 27)

    X, Y, Z = finv(fx) * 0.95047, finv(fy), finv(fz) * 1.08883
    r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z
    g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z
    bb = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z
    return [F.linear_to_srgb(v) for v in (r, g, bb)]


def attack_reconstructions(fp: Dict[str, Any], seeds: int = 2) -> Dict[str, FImage]:
    """Reconstructions from a fingerprint alone (see the module docstring)."""
    import random

    w, h = fp["width"], fp["height"]
    mean_rgb = _lab_to_srgb(fp["lab_mean_q"])
    out = {"mean_colour": FImage.blank(w, h, (mean_rgb[0], mean_rgb[1], mean_rgb[2]))}
    half = SPEC_N // 2
    amp = [math.sqrt(10 ** (db / 10)) for db in fp["spectrum16_db"]]
    hist = fp["luma_hist32_permille"]
    tot = max(1, sum(hist))
    cdf, acc = [], 0
    for cnt in hist:
        acc += cnt
        cdf.append(acc / tot)
    mean_l = 0.2126 * mean_rgb[0] + 0.7152 * mean_rgb[1] + 0.0722 * mean_rgb[2]
    ratio = [mr / max(1e-4, mean_l) for mr in mean_rgb]
    cos_x = {u: [math.cos(2 * math.pi * u * x / SPEC_N) for x in range(SPEC_N)] for u in range(-half, half + 1)}
    sin_x = {u: [math.sin(2 * math.pi * u * x / SPEC_N) for x in range(SPEC_N)] for u in range(-half, half + 1)}
    for s in range(seeds):
        rng = random.Random(s)
        small = [0.0] * (SPEC_N * SPEC_N)
        for v in range(-half + 1, half):  # random-phase synthesis of the band amplitudes
            for u in range(0, half):
                if u == 0 and v <= 0:
                    continue
                a = amp[_band(u, v)]
                ph = rng.uniform(0, 2 * math.pi)
                for y in range(SPEC_N):
                    t = 2 * math.pi * v * y / SPEC_N + ph
                    ct, st = math.cos(t), math.sin(t)
                    row = y * SPEC_N
                    cx, sx = cos_x[u], sin_x[u]
                    for x in range(SPEC_N):
                        small[row + x] += a * (cx[x] * ct - sx[x] * st)
        up = F.resize_bilinear(small, SPEC_N, SPEC_N, w, h)
        order = sorted(range(len(up)), key=lambda k: up[k])
        lum = [0.0] * len(up)
        b = 0
        for rank_i, k in enumerate(order):  # histogram matching onto the shipped tone distribution
            q = (rank_i + 0.5) / len(up)
            while b < HIST_BINS - 1 and cdf[b] < q:
                b += 1
            lum[k] = (b + 0.5) / HIST_BINS
        out[f"random_phase_{s}"] = FImage(w, h, [min(1.0, v * ratio[0]) for v in lum],
                                          [min(1.0, v * ratio[1]) for v in lum], [min(1.0, v * ratio[2]) for v in lum])
    return out


def attack_psnr(original: FImage) -> Dict[str, float]:
    fp = fingerprint(original)
    return {name: F.psnr(original, rec) for name, rec in attack_reconstructions(fp).items()}


def thumbnail16_psnr(original: FImage) -> float:
    """The plan's original sketch (16x16 downsample, bilinear upsample), measured to document the replacement."""
    th = F.resize(original, 16, 16, "area")
    return F.psnr(original, F.resize(th, original.w, original.h, "bilinear"))


# ---- metrics + verification ---------------------------------------------------------------------------------------

METRICS = {
    "flip": ("max_mean", lambda a, b: F.hyab_mean(a, b)),
    "mae": ("max_mean", lambda a, b: sum(abs(x - y) for ca, cb in zip(a.channels(), b.channels())
                                         for x, y in zip(ca, cb)) / (3 * a.n)),
    "psnr": ("min", lambda a, b: F.psnr(a, b)),
    "ssim": ("min", lambda a, b: F.ssim(a, b)),
}


def within_tolerance(rebuilt: FImage, reference: FImage, tolerance: Dict[str, Any]) -> Dict[str, Any]:
    metric = tolerance.get("metric", "flip")
    if metric not in METRICS:
        raise RecipeError(f"unknown tolerance metric {metric!r} ({', '.join(METRICS)})")
    kind, fn = METRICS[metric]
    if (rebuilt.w, rebuilt.h) != (reference.w, reference.h):
        return {"metric": metric, "value": None, "ok": False, "reason": "size differs"}
    value = fn(rebuilt, reference)
    if kind == "max_mean":
        limit = float(tolerance.get("max_mean", 0.01))
        ok = value <= limit
    else:
        limit = float(tolerance.get("min", 0.0))
        ok = value >= limit
    return {"metric": metric, "value": value, "limit": limit, "ok": ok}


def verify_rebuild(output: Dict[str, Any], rebuilt: bytes, *, reference: Optional[bytes] = None) -> Dict[str, Any]:
    """Accept a rebuilt primary output against a recipe ``output`` block (see the module docstring)."""
    sha = hashlib.sha256(rebuilt).hexdigest()
    if output.get("sha256") == sha:
        return {"ok": True, "method": "exact", "sha256": sha}
    img = _img(rebuilt)
    if reference is not None:
        tol = output.get("tolerance")
        if not tol:
            raise GateFailure("rebuild differs from the recorded output and the recipe has no tolerance")
        res = within_tolerance(img, _img(reference), tol)
        if not res["ok"]:
            raise GateFailure(f"rebuild outside tolerance: {res}")
        return {"ok": True, "method": "tolerance", **res}
    fp = output.get("fingerprint")
    if not fp:
        raise GateFailure("rebuild differs from the recorded output, and there is neither a reference image nor "
                          "a fingerprint to compare with")
    tol = dict(DEFAULT_FP_TOLERANCE, **(output.get("fingerprint_tolerance") or {}))
    d = compare(fingerprint(img), fp)
    ok = (d["spectrum_db"] <= tol["max_spectrum_db"] and d["orient_l1"] <= tol["max_orient_l1"]
          and d["hist_l1"] <= tol["max_hist_l1"] and d["lab_mean_de"] <= tol["max_lab_mean_de"])
    if not ok:
        raise GateFailure(f"rebuild does not match the shipped fingerprint: {d} (tolerance {tol})")
    return {"ok": True, "method": "fingerprint", **d}


def legacy_texture_set(seed: int, size: int = 256, contrast: float = 3.0) -> List[FImage]:
    """High-contrast legacy-style textures (hand-painted era palettes: deep shadows, bright highlights)."""
    import colorsys
    import random

    rng = random.Random(seed)
    out = []
    for kind in F.ALBEDO_KINDS:
        hue = rng.random()
        pal = (colorsys.hsv_to_rgb(hue, rng.uniform(0.3, 0.7), rng.uniform(0.04, 0.15)),
               colorsys.hsv_to_rgb((hue + 0.03) % 1.0, rng.uniform(0.2, 0.5), rng.uniform(0.85, 1.0)))
        img = F.gen_albedo(rng, size, kind=kind, palette=pal)[0]
        lum = img.luma()
        m = sum(lum) / len(lum)
        k = contrast  # hand-painted era textures: strong local contrast around the mean
        img = img.map(lambda r, g, b: (r + (r - m) * (k - 1), g + (g - m) * (k - 1), b + (b - m) * (k - 1))).clamp()
        out.append(img.quantised())
    return out
