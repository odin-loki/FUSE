"""W3.4: library matcher - rank library materials for a legacy texture by colour statistics and tags (§4.2).

Features (:func:`features`, computed on a 32² area resample of the albedo):

* CIELAB mean and standard deviation of the pixels (colour and contrast),
* a 4×4 histogram over CIELAB a*/b* plus a 6-bin L* histogram (palette shape),
* texture statistics on luma: gradient energy, horizontal/vertical anisotropy and the share of
  high-frequency energy (separates bricks, planks, weave and noise of the same colour).

:func:`rank` scores ``colour_distance + w_tags * (1 - jaccard(tags))`` (weights in :data:`WEIGHTS`) and returns the library sorted best
first; a human picks among the top candidates (§4.2 step 2). Embeddings (SigLIP 2) are an optional extra
signal later; this matcher is the CPU baseline the gate measures.

:func:`synthetic_benchmark` builds the W3.4 gate: a library of materials (several share a colour family or
a pattern on purpose) and, for each query, a "legacy" texture of the same material - different noise seed,
colour drift, a 32² → 64² low-resolution round trip, 5-bit quantisation - with partial, noisy tags.
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Sequence, Tuple

from . import fimage as F
from .fimage import FImage, srgb_to_lab

_AB_EDGES = (-128.0, -20.0, 0.0, 20.0, 128.0)
_L_BINS = 6
FEATURE_SIZE = 32


@dataclass
class Entry:
    id: str
    tags: List[str]
    feat: Dict[str, Any]
    meta: Dict[str, Any] = field(default_factory=dict)


def _bin(v: float, edges: Sequence[float]) -> int:
    for i in range(len(edges) - 1):
        if v < edges[i + 1]:
            return i
    return len(edges) - 2


def features(img: FImage) -> Dict[str, Any]:
    if (img.w, img.h) != (FEATURE_SIZE, FEATURE_SIZE):
        img = F.resize(img, FEATURE_SIZE, FEATURE_SIZE, "area" if img.w > FEATURE_SIZE else "bilinear")
    labs = [srgb_to_lab((r, g, b)) for r, g, b in zip(img.r, img.g, img.b)]
    n = len(labs)
    mean = [sum(l[i] for l in labs) / n for i in range(3)]
    std = [math.sqrt(sum((l[i] - mean[i]) ** 2 for l in labs) / n) for i in range(3)]
    hab = [0.0] * 16
    hl = [0.0] * _L_BINS
    for L, a, b in labs:
        hab[_bin(a, _AB_EDGES) * 4 + _bin(b, _AB_EDGES)] += 1.0 / n
        hl[min(_L_BINS - 1, max(0, int(L / (100.0 / _L_BINS))))] += 1.0 / n
    lum = img.luma()
    w = h = FEATURE_SIZE
    gx = gy = 0.0
    for y in range(h):
        for x in range(w):
            k = y * w + x
            gx += abs(lum[y * w + (x + 1) % w] - lum[k])
            gy += abs(lum[((y + 1) % h) * w + x] - lum[k])
    gx /= n
    gy /= n
    low = F.box_blur(lum, w, h, 1)
    hf = sum(abs(a - b) for a, b in zip(lum, low)) / n
    tot = sum(abs(a - sum(lum) / n) for a in lum) / n + 1e-6
    return {"lab_mean": mean, "lab_std": std, "hist_ab": hab, "hist_l": hl, "grad": gx + gy,
            "aniso": (gx - gy) / (gx + gy + 1e-6), "hf_share": hf / tot}


# Weights chosen on benchmark seeds 1-3 (the gate runs another seed): the CIELAB mean dominates, the palette
# histogram and the texture statistics break ties between same-colour materials. The contrast (std) term
# did not help on legacy (low-resolution, quantised) queries and is kept at 0.
WEIGHTS = {"mean": 2.0, "std": 0.0, "hist": 0.5, "texture": 0.25, "tags": 6.0}


def colour_distance(f: Dict[str, Any], g: Dict[str, Any], weights: Optional[Dict[str, float]] = None) -> float:
    w = weights or WEIGHTS
    dmean = math.sqrt(sum((a - b) ** 2 for a, b in zip(f["lab_mean"], g["lab_mean"])))
    dstd = sum(abs(a - b) for a, b in zip(f["lab_std"], g["lab_std"]))
    dhist = sum(abs(a - b) for a, b in zip(f["hist_ab"], g["hist_ab"])) +         sum(abs(a - b) for a, b in zip(f["hist_l"], g["hist_l"]))
    dtex = abs(f["grad"] - g["grad"]) * 40.0 + abs(f["aniso"] - g["aniso"]) * 20.0 + abs(f["hf_share"] - g["hf_share"]) * 20.0
    return w["mean"] * dmean / 10.0 + w["std"] * dstd / 10.0 + w["hist"] * dhist * 2.0 + w["texture"] * dtex


def jaccard(a: Sequence[str], b: Sequence[str]) -> float:
    sa, sb = set(a), set(b)
    return len(sa & sb) / len(sa | sb) if sa | sb else 0.0


def rank(query: FImage, query_tags: Sequence[str], library: Sequence[Entry], *, w_tags: Optional[float] = None,
         top: Optional[int] = None) -> List[Tuple[str, float]]:
    qf = features(query)
    wt = WEIGHTS["tags"] if w_tags is None else w_tags
    scored = [(e.id, colour_distance(qf, e.feat) + wt * (1.0 - jaccard(query_tags, e.tags))) for e in library]
    scored.sort(key=lambda t: (t[1], t[0]))
    return scored[:top] if top else scored


# ---- the W3.4 synthetic benchmark -------------------------------------------------------------------------------

_FAMILIES = [  # (name, hue, sat, tags) - several share a hue on purpose
    ("granite", 0.08, 0.10, ["stone", "rock", "grey"]), ("sandstone", 0.10, 0.35, ["stone", "rock", "warm"]),
    ("red_brick", 0.02, 0.55, ["brick", "masonry", "red"]), ("yellow_brick", 0.12, 0.45, ["brick", "masonry", "warm"]),
    ("oak", 0.08, 0.55, ["wood", "planks", "warm"]), ("pine", 0.11, 0.40, ["wood", "planks", "light"]),
    ("walnut", 0.06, 0.60, ["wood", "planks", "dark"]), ("moss", 0.28, 0.50, ["foliage", "ground", "green"]),
    ("grass", 0.30, 0.65, ["ground", "grass", "green"]), ("slate_tile", 0.60, 0.15, ["tile", "stone", "grey"]),
    ("terracotta", 0.04, 0.60, ["tile", "clay", "red"]), ("denim", 0.62, 0.55, ["fabric", "cloth", "blue"]),
    ("burlap", 0.11, 0.35, ["fabric", "cloth", "warm"]), ("rust", 0.05, 0.70, ["metal", "rust", "red"]),
    ("steel", 0.58, 0.08, ["metal", "bare", "grey"]), ("sand", 0.12, 0.30, ["ground", "sand", "light"]),
    ("dirt", 0.07, 0.40, ["ground", "dirt", "dark"]), ("ice", 0.55, 0.25, ["ice", "cold", "light"]),
]
_KIND_FOR_TAG = {"brick": "bricks", "planks": "planks", "tile": "tiles", "fabric": "weave", "ground": "speckle"}


def _family_kind(tags: Sequence[str], rng: random.Random) -> str:
    for t in tags:
        if t in _KIND_FOR_TAG:
            return _KIND_FOR_TAG[t]
    return "noise"


def _material_image(rng: random.Random, hue: float, sat: float, kind: str, value_shift: float = 0.0) -> FImage:
    import colorsys

    c1 = colorsys.hsv_to_rgb(hue, sat, min(1.0, 0.35 + value_shift))
    c2 = colorsys.hsv_to_rgb(hue, sat * 0.8, min(1.0, 0.8 + value_shift))
    return F.gen_albedo(rng, 64, kind=kind, palette=(c1, c2))[0]


def build_library(seed: int) -> Tuple[List[Entry], Dict[str, Dict[str, Any]]]:
    rng = random.Random(seed)
    lib, specs = [], {}
    for name, hue, sat, tags in _FAMILIES:
        for variant, vshift in (("", 0.0), ("_dark", -0.18)):
            kind = _family_kind(tags, rng)
            mid = f"mat/{name}{variant}"
            specs[mid] = {"hue": hue, "sat": sat, "kind": kind, "vshift": vshift, "tags": tags}
            img = _material_image(rng, hue, sat, kind, vshift)
            lib.append(Entry(mid, list(tags) + (["dark"] if variant else []), features(img)))
    return lib, specs


def legacy_query(rng: random.Random, spec: Dict[str, Any]) -> Tuple[FImage, List[str]]:
    """The 'legacy' texture of a library material: new noise, colour drift, low-res round trip, 5-bit."""
    hue = (spec["hue"] + rng.uniform(-0.015, 0.015)) % 1.0
    sat = max(0.0, spec["sat"] * rng.uniform(0.85, 1.15))
    img = _material_image(rng, hue, sat, spec["kind"], spec["vshift"] + rng.uniform(-0.05, 0.05))
    img = F.resize(F.resize(img, 32, 32, "area"), 64, 64, "bilinear")
    img = img.map(lambda r, g, b: tuple(round(v * 31) / 31 for v in (r, g, b)))
    tags = [t for t in spec["tags"] if rng.random() < 0.6]
    if rng.random() < 0.5:
        tags.append(rng.choice(["old", "worn", "dirty", "tiling", "wall", "floor"]))
    return img, tags


def synthetic_benchmark(seed: int, queries: int = 40, top_k: int = 3) -> Dict[str, Any]:
    lib, specs = build_library(seed)
    rng = random.Random(seed + 1)
    ids = sorted(specs)
    hits = colour_hits = 0
    rows = []
    for q in range(queries):
        truth = ids[q % len(ids)] if q < len(ids) else rng.choice(ids)
        img, tags = legacy_query(rng, specs[truth])
        ranked = rank(img, tags, lib, top=top_k)
        colour_only = rank(img, [], lib, w_tags=0.0, top=top_k)
        hit = truth in [r[0] for r in ranked]
        hits += hit
        colour_hits += truth in [r[0] for r in colour_only]
        rows.append({"truth": truth, "top": [r[0] for r in ranked], "hit": hit, "tags": tags})
    return {"library": len(lib), "queries": queries, "top_k": top_k, "top_k_rate": hits / queries,
            "colour_only_top_k_rate": colour_hits / queries, "rows": rows}
