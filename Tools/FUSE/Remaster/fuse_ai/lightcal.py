"""W5.3: light calibration solver (plan §4.4 "Calibration step").

Legacy light colours and intensities are unitless 0-1 values. Per level, from a golden capture:

1. For each capture region r the FUSE (or Remix) render provides the linear luminance each *light type*
   contributes at unit scale, ``contrib[r][t]`` (types such as point, spot, sun, ambient).
2. The original capture is display-referred; its region means are converted back to linear with the
   capture's tone curve (``srgb`` for fixed-function era output, ``gamma:<g>`` or ``linear``). Regions that are
   clipped (≥ ``clip``) or nearly black (≤ ``floor``) carry no information and are skipped.
3. Solve ``target[r] ≈ Σ_t k_t · contrib[r][t]`` by non-negative weighted least squares (relative error:
   weights 1/target), on CPU, then split ``k_t = scale · m_t`` with the per-type multipliers normalised so their
   contribution-weighted mean is 1. ``scale`` is the one global exposure scale of §4.4; ``m_t`` are the
   per-light-type multipliers.

:func:`synthetic_scene` builds the W5.3 gate: a floor of regions lit by legacy point/spot lights, a sun
with a shadow mask and an ambient term, rendered at unit scale, then "captured" with a known scale and
multipliers, sRGB-encoded, with capture noise, 8-bit quantisation and some clipped highlights.
"""

from __future__ import annotations

import math
import random
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .errors import RecipeError
from .fimage import linear_to_srgb, srgb_to_linear

CAL_SCHEMA = "fuse.remaster-light-calibration/1"


def decode(v: float, curve: str) -> float:
    if curve == "srgb":
        return srgb_to_linear(v)
    if curve == "linear":
        return v
    if curve.startswith("gamma:"):
        return max(0.0, v) ** float(curve.split(":", 1)[1])
    raise RecipeError(f"unknown tone curve {curve!r} (srgb, linear, gamma:<g>)")


def _solve(a: List[List[float]], b: List[float]) -> List[float]:
    """Gaussian elimination with partial pivoting (small dense systems)."""
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        if abs(m[p][c]) < 1e-15:
            raise RecipeError("light calibration is singular: a light type contributes to no usable region")
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r != c:
                f = m[r][c] / m[c][c]
                for k in range(c, n + 1):
                    m[r][k] -= f * m[c][k]
    return [m[i][n] / m[i][i] for i in range(n)]


def nnls_weighted(rows: Sequence[Sequence[float]], target: Sequence[float], weights: Sequence[float]) -> List[float]:
    """Non-negative weighted least squares by active-set elimination (types with a negative fit are dropped)."""
    n = len(rows[0])
    active = list(range(n))
    while True:
        ata = [[sum(w * r[i] * r[j] for r, w in zip(rows, weights)) for j in active] for i in active]
        atb = [sum(w * r[i] * t for r, t, w in zip(rows, target, weights)) for i in active]
        sol = _solve(ata, atb)
        if min(sol) >= 0 or len(active) == 1:
            out = [0.0] * n
            for i, v in zip(active, sol):
                out[i] = max(0.0, v)
            return out
        active.pop(sol.index(min(sol)))


def calibrate(contrib: Sequence[Dict[str, float]], capture: Sequence[float], *, curve: str = "srgb",
              clip: float = 0.98, floor: float = 0.02) -> Dict[str, Any]:
    if len(contrib) != len(capture):
        raise RecipeError("contrib and capture need one entry per region")
    types = sorted({t for c in contrib for t in c})
    rows, target = [], []
    for c, v in zip(contrib, capture):
        if v >= clip or v <= floor:
            continue
        rows.append([c.get(t, 0.0) for t in types])
        target.append(decode(v, curve))
    if len(rows) < len(types):
        raise RecipeError(f"only {len(rows)} usable regions for {len(types)} light types")
    weights = [1.0 / (t * t) for t in target]  # relative error
    k = nnls_weighted(rows, target, weights)
    totals = [sum(r[i] for r in rows) for i in range(len(types))]
    scale = sum(ki * wi for ki, wi in zip(k, totals)) / max(1e-12, sum(totals))
    mult = {t: (ki / scale if scale > 0 else 0.0) for t, ki in zip(types, k)}
    pred = [sum(ki * ri for ki, ri in zip(k, r)) for r in rows]
    rel = math.sqrt(sum(((p - t) / t) ** 2 for p, t in zip(pred, target)) / len(target))
    return {"schema": CAL_SCHEMA, "global_scale": scale, "multipliers": mult, "tone_curve": curve,
            "regions_used": len(rows), "regions_skipped": len(capture) - len(rows), "rms_relative_error": rel}


# ---- the synthetic scene (W5.3 gate) ------------------------------------------------------------------------------


def synthetic_scene(seed: int, *, scale: float, multipliers: Dict[str, float], grid: int = 8,
                    noise: float = 0.02) -> Tuple[List[Dict[str, float]], List[float], Dict[str, Any]]:
    """Regions on a 16 m floor lit by legacy lights. Returns (contrib per region, captured sRGB means, truth)."""
    rng = random.Random(seed)
    lights = []
    for _ in range(5):
        lights.append({"type": "point", "pos": (rng.uniform(0, 16), rng.uniform(2, 4), rng.uniform(0, 16)),
                       "colour": rng.uniform(0.4, 1.0)})
    for _ in range(3):
        lights.append({"type": "spot", "pos": (rng.uniform(0, 16), rng.uniform(3, 5), rng.uniform(0, 16)),
                       "colour": rng.uniform(0.5, 1.0), "cos_outer": math.cos(math.radians(rng.uniform(25, 45)))})
    sun_dir_y = math.cos(math.radians(rng.uniform(20, 50)))
    shadow = [[rng.random() < 0.35 for _ in range(grid)] for _ in range(grid)]
    contrib: List[Dict[str, float]] = []
    for gz in range(grid):
        for gx in range(grid):
            p = ((gx + 0.5) * 16 / grid, 0.0, (gz + 0.5) * 16 / grid)
            c = {"point": 0.0, "spot": 0.0, "sun": 0.0 if shadow[gz][gx] else 0.6 * sun_dir_y, "ambient": 0.08}
            for L in lights:
                d = [L["pos"][i] - p[i] for i in range(3)]
                d2 = sum(x * x for x in d)
                cos_n = d[1] / math.sqrt(d2)  # floor normal +Y
                e = L["colour"] * max(0.0, cos_n) / d2 * 6.0
                if L["type"] == "spot":
                    cos_axis = d[1] / math.sqrt(d2)  # spots point straight down
                    e = e if cos_axis >= L["cos_outer"] else 0.0
                c[L["type"]] += e
            contrib.append(c)
    capture = []
    for c in contrib:
        lin = scale * sum(multipliers[t] * v for t, v in c.items())
        lin *= 1.0 + rng.gauss(0.0, noise)
        enc = linear_to_srgb(min(1.0, max(0.0, lin)))
        capture.append(round(enc * 255) / 255)
    totals = {t: sum(c[t] for c in contrib) for t in multipliers}
    norm = sum(multipliers[t] * totals[t] for t in multipliers) / sum(totals.values())
    truth = {"scale": scale * norm, "multipliers": {t: multipliers[t] / norm for t in multipliers}}
    return contrib, capture, truth
