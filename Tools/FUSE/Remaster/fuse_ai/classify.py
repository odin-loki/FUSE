"""W3.1: CPU texture classifiers (plan §4.1 "Local first", §4.2 step 1).

Five independent, explainable detectors on a float image (no model, no training run; thresholds were set on
generator seeds outside the gate's seed range):

==============  ===============================================================================================
detector        signal
==============  ===============================================================================================
normal_map      decoded vectors ``2c - 1`` are unit length with z > 0 for most pixels, and the mean x/y are ≈ 0
                (slopes average out) while the mean blue is high
lightmap        almost no high-frequency energy (|Laplacian| small relative to the low-frequency spread), low
                saturation, and a real low-frequency luminance range (light pools, shadows)
ui              most opaque pixels sit in flat regions (equal to their 4 neighbours) with few distinct colours;
                transparency alone is not enough (alpha-tested foliage is noisy inside the mask)
tiling          the wrap-around seam step is no larger than the interior steps at the same phase of the
                texture's dominant period (or the median step when there is none)
baked_light     a heavily blurred luma field still varies a lot relative to the mean (stationary materials blur
                to a constant; baked light keeps its pools and shadows); in the borderline band the local
                detail amplitude must also follow the local brightness (light is multiplicative) - the asset
                plan's AO/albedo-correlation idea
==============  ===============================================================================================

:func:`classify` returns every score and the boolean decisions, plus a coarse ``role`` (normal / lightmap / ui /
albedo) for the §4.2 strategy choice. Lightmaps are dropped downstream (FUSE relights the scene).
"""

from __future__ import annotations

import math
from typing import Any, Dict, List

from .fimage import FImage, box_blur, saturation

THRESHOLDS = {
    "normal_unit_frac": 0.85, "normal_xy_mean": 0.07, "normal_b_mean": 0.72,
    "lightmap_hf": 0.10, "lightmap_sat": 0.25, "lightmap_range": 0.18,
    "ui_flat_frac": 0.45, "ui_soft_edge_frac": 0.5,
    "tiling_seam_ratio": 1.5,
    "baked_cv": 0.10, "baked_cv_strong": 0.13, "baked_corr": 0.2,
}


def _stats(v: List[float]) -> Dict[str, float]:
    n = len(v)
    m = sum(v) / n
    var = sum((x - m) ** 2 for x in v) / n
    return {"mean": m, "std": math.sqrt(var)}


def normal_map_score(img: FImage) -> Dict[str, float]:
    unit = 0
    for r, g, b in zip(img.r, img.g, img.b):
        x, y, z = 2 * r - 1, 2 * g - 1, 2 * b - 1
        if z > 0 and abs(math.sqrt(x * x + y * y + z * z) - 1.0) < 0.12:
            unit += 1
    n = img.n
    return {"unit_frac": unit / n, "x_mean": sum(img.r) / n - 0.5, "y_mean": sum(img.g) / n - 0.5,
            "b_mean": sum(img.b) / n}


def is_normal_map(s: Dict[str, float]) -> bool:
    t = THRESHOLDS
    return (s["unit_frac"] >= t["normal_unit_frac"] and abs(s["x_mean"]) <= t["normal_xy_mean"]
            and abs(s["y_mean"]) <= t["normal_xy_mean"] and s["b_mean"] >= t["normal_b_mean"])


def _laplacian_abs_mean(ch: List[float], w: int, h: int) -> float:
    tot = 0.0
    for y in range(1, h - 1):
        for x in range(1, w - 1):
            k = y * w + x
            tot += abs(4 * ch[k] - ch[k - 1] - ch[k + 1] - ch[k - w] - ch[k + w])
    return tot / max(1, (w - 2) * (h - 2))


def lightmap_score(img: FImage) -> Dict[str, float]:
    lum = img.luma()
    low = box_blur(lum, img.w, img.h, max(1, img.w // 16), wrap=False)
    lo_std = _stats(low)["std"]
    hf = _laplacian_abs_mean(lum, img.w, img.h)
    sat = sum(saturation(r, g, b) for r, g, b in zip(img.r, img.g, img.b)) / img.n
    return {"hf_ratio": hf / max(1e-4, lo_std), "saturation": sat, "range": max(low) - min(low)}


def is_lightmap(s: Dict[str, float]) -> bool:
    t = THRESHOLDS
    return s["hf_ratio"] <= t["lightmap_hf"] and s["saturation"] <= t["lightmap_sat"] and s["range"] >= t["lightmap_range"]


def ui_score(img: FImage) -> Dict[str, float]:
    w, h = img.w, img.h
    al = img.alpha
    q = [(int(r * 255 + 0.5), int(g * 255 + 0.5), int(b * 255 + 0.5)) for r, g, b in zip(img.r, img.g, img.b)]
    flat = opaque = 0
    soft = hard = 0
    colours = set()
    for y in range(h):
        for x in range(w):
            k = y * w + x
            if al[k] < 0.5:
                continue
            opaque += 1
            colours.add(q[k])
            same = 0
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and 0 <= yy < h and al[yy * w + xx] >= 0.5:
                    o = q[yy * w + xx]
                    if o == q[k]:
                        same += 1
                    elif dx > 0 or dy > 0:  # count each differing pair once
                        if max(abs(o[0] - q[k][0]), abs(o[1] - q[k][1]), abs(o[2] - q[k][2])) <= 3:
                            soft += 1
                        else:
                            hard += 1
            if same >= 3:
                flat += 1
    transparent = sum(1 for a in al if a < 0.5) / img.n
    return {"flat_frac": flat / max(1, opaque), "opaque_frac": opaque / img.n, "transparent_frac": transparent,
            "colours_per_opaque": len(colours) / max(1, opaque), "soft_edge_frac": soft / max(1, soft + hard)}


def is_ui(s: Dict[str, float]) -> bool:
    """Flat regions separated by hard edges; smooth 8-bit gradients (1-3 LSB steps) are not UI."""
    return (s["opaque_frac"] >= 0.03 and s["flat_frac"] >= THRESHOLDS["ui_flat_frac"]
            and s["soft_edge_frac"] < THRESHOLDS["ui_soft_edge_frac"])


def _seam_ratio(steps: List[float]) -> float:
    """``steps[i]`` = mean |line i+1 - line i| (cyclic; the last entry crosses the wrap seam). The seam step is
    compared with the steps at the same phase of the texture's dominant period (bricks, tiles, planks repeat;
    a tiling texture's seam may well sit on a mortar line, like every other period), or with the median step
    when there is no clear period."""
    n = len(steps)
    seam = n - 1
    inter = steps[:-1]
    mean = sum(inter) / len(inter)
    spread = sum(abs(v - mean) for v in inter) / len(inter) + 1e-6
    best = None
    for period in (2, 4, 8, 16, 32, 64, 128):
        if period > n // 2:
            break
        xs = range(n - 1 - period)
        d = sum(abs(steps[x] - steps[x + period]) for x in xs) / len(xs)
        if best is None or d < best[0]:
            best = (d, period)
    if best is not None and best[0] / spread < 0.6:
        ref = sorted(steps[seam - k * best[1]] for k in range(1, n // best[1]) if seam - k * best[1] >= 0)
    else:
        ref = sorted(inter)
    return steps[seam] / (ref[len(ref) // 2] + 1e-3)


def tiling_score(img: FImage) -> Dict[str, float]:
    w, h = img.w, img.h
    lum = img.luma()
    cols = [sum(abs(lum[y * w + (x + 1) % w] - lum[y * w + x]) for y in range(h)) / h for x in range(w)]
    rows = [sum(abs(lum[((y + 1) % h) * w + x] - lum[y * w + x]) for x in range(w)) / w for y in range(h)]
    return {"seam_ratio": max(_seam_ratio(cols), _seam_ratio(rows))}


def is_tiling(s: Dict[str, float]) -> bool:
    return s["seam_ratio"] <= THRESHOLDS["tiling_seam_ratio"]


def _pearson(xs: List[float], ys: List[float]) -> float:
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    return sxy / math.sqrt(sxx * syy) if sxx > 1e-12 and syy > 1e-12 else 0.0


def baked_light_score(img: FImage) -> Dict[str, float]:
    """Two signals: (1) the heavily blurred luma still varies (light pools, shadows); (2) light is
    multiplicative, so the local detail amplitude follows the local brightness (block mean vs block
    high-pass std correlate). Large uneven albedo features (two planks of different tone) give (1) but not (2)."""
    w, h = img.w, img.h
    lum = img.luma()
    low = box_blur(lum, w, h, max(2, w // 5), wrap=True)
    st = _stats(low)
    hp_base = box_blur(lum, w, h, 1, wrap=True)
    hp = [a - b for a, b in zip(lum, hp_base)]
    bs = max(4, w // 8)
    means, stds = [], []
    for by in range(0, h - bs + 1, bs):
        for bx in range(0, w - bs + 1, bs):
            idx = [(by + y) * w + bx + x for y in range(bs) for x in range(bs)]
            means.append(sum(lum[k] for k in idx) / len(idx))
            stds.append(_stats([hp[k] for k in idx])["std"])
    return {"low_cv": st["std"] / max(1e-4, _stats(lum)["mean"]), "detail_corr": _pearson(means, stds)}


def has_baked_light(s: Dict[str, float]) -> bool:
    t = THRESHOLDS
    return s["low_cv"] >= t["baked_cv_strong"] or (s["low_cv"] >= t["baked_cv"] and s["detail_corr"] >= t["baked_corr"])


def classify(img: FImage) -> Dict[str, Any]:
    nm, lm, ui, tl, bk = (normal_map_score(img), lightmap_score(img), ui_score(img), tiling_score(img),
                          baked_light_score(img))
    out = {"normal_map": is_normal_map(nm), "lightmap": is_lightmap(lm), "ui": is_ui(ui), "tiling": is_tiling(tl),
           "scores": {"normal_map": nm, "lightmap": lm, "ui": ui, "tiling": tl, "baked_light": bk}}
    if out["normal_map"]:
        role = "normal"
    elif out["ui"]:
        role = "ui"
    elif out["lightmap"]:
        role = "lightmap"
    else:
        role = "albedo"
    out["role"] = role
    out["baked_light"] = role == "albedo" and has_baked_light(bk)
    return out


# ---- the labelled synthetic set (W3.1 gate) ----------------------------------------------------------------------


def labelled_set(seed: int, per_class: int = 20, size: int = 64) -> Dict[str, List[Any]]:
    """Per detector: a balanced list of (image, label, description), generated from ``seed``.

    Negatives include hard cases: blue water/sky textures for normal maps, baked-light albedos for lightmaps,
    alpha-tested foliage for UI. The baked-light detector only runs on albedo-role images, so its set is
    albedo with and without baked light."""
    import random

    from . import fimage as F

    rng = random.Random(seed)
    sets: Dict[str, List[Any]] = {k: [] for k in ("normal_map", "lightmap", "ui", "tiling", "baked_light")}

    def albedo(periodic: bool = True) -> FImage:
        return F.gen_albedo(rng, size, periodic=periodic)[0]

    for i in range(per_class):
        sets["normal_map"].append((F.gen_normal_map(rng, size, periodic=rng.random() < 0.7), True, "normal"))
        neg = [albedo, lambda: F.gen_blue_albedo(rng, size), lambda: F.gen_lightmap(rng, size),
               lambda: F.gen_ui(rng, size)][i % 4]()
        sets["normal_map"].append((neg, False, "other"))

        sets["lightmap"].append((F.gen_lightmap(rng, size), True, "lightmap"))
        neg = [albedo, lambda: F.gen_baked(rng, size), lambda: F.gen_normal_map(rng, size),
               lambda: F.gen_ui(rng, size)][i % 4]()
        sets["lightmap"].append((neg, False, "other"))

        sets["ui"].append((F.gen_ui(rng, size), True, "ui"))
        neg = [albedo, lambda: F.gen_foliage(rng, size), lambda: F.gen_normal_map(rng, size),
               lambda: F.gen_lightmap(rng, size)][i % 4]()
        sets["ui"].append((neg, False, "other"))

        tiled = F.gen_normal_map(rng, size, True) if i % 3 == 2 else albedo(True)
        sets["tiling"].append((tiled, True, "tiling"))
        untiled = F.gen_normal_map(rng, size, False) if i % 3 == 2 else albedo(False)
        sets["tiling"].append((untiled, False, "non-tiling"))

        sets["baked_light"].append((F.gen_baked(rng, size), True, "baked"))
        sets["baked_light"].append((albedo(rng.random() < 0.5), False, "clean"))
    return sets


def evaluate(seed: int, per_class: int = 20, size: int = 64) -> Dict[str, Any]:
    """Accuracy per detector on :func:`labelled_set`; images are quantised to 8 bits first (as stored)."""
    sets = labelled_set(seed, per_class, size)
    fns = {"normal_map": lambda im: is_normal_map(normal_map_score(im)),
           "lightmap": lambda im: is_lightmap(lightmap_score(im)),
           "ui": lambda im: is_ui(ui_score(im)),
           "tiling": lambda im: is_tiling(tiling_score(im)),
           "baked_light": lambda im: has_baked_light(baked_light_score(im))}
    out: Dict[str, Any] = {}
    for name, items in sets.items():
        ok, misses = 0, []
        for n, (img, label, desc) in enumerate(items):
            got = fns[name](img.quantised())
            if got == label:
                ok += 1
            else:
                misses.append({"index": n, "label": label, "kind": desc})
        out[name] = {"accuracy": ok / len(items), "n": len(items), "misses": misses}
    return out
