"""W5.6: emissive surface -> proxy light heuristic for renderer tier T0 (plan §4.4 table, "Emissive surfaces").

At T0 (no path tracing) an emissive surface lights nothing by itself, so the importer spawns a proxy light
for each emissive surface that is bright and large enough to matter. Input: triangles grouped by material
(Wavefront OBJ ``usemtl`` groups, or in-memory lists) and POCO ``material`` payloads (§2.3: ``model:
Emissive`` and/or ``emissiveNits``, ``baseColor`` linear RGBA).

Per emissive group:

* area A, area-weighted centroid and normal; *planarity* = |Σ n_i A_i| / Σ A_i;
* luminous flux of a one-sided Lambertian emitter: Φ = π · L · A (lumens, L = ``emissiveNits``);
* skipped when L < ``min_nits`` or Φ < ``min_lumens`` (dim signs, tiny LEDs);
* planar groups (planarity ≥ 0.95) become a ``Rect`` light with ``intensity`` = L (nits, the POCO unit for
  area lights) and ``size`` = the extents along the surface's principal axes; others become a ``Point`` light
  of Φ lumens offset 2 cm along the normal;
* ``range`` = the distance where the illuminance of Φ/(4π) cd drops to ``cutoff_lux`` (default 0.1 lux);
* colour = ``baseColor.rgb`` normalised to a maximum of 1.

The output is POCO records (``fuse.poco/1``): a ``light`` per proxy and an ``entity`` placing it (transform
column-major 4x4, translation in elements 12-14, local +Z = emission direction **[VERIFY the engine's rect-light
axis convention]**), with ``provenance.origin: generated`` (the heuristic is ours; the derivation is recorded
so a recipe-only source mesh keeps the light recipe-only too).
"""

from __future__ import annotations

import math
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .errors import RecipeError

V3 = Tuple[float, float, float]


def load_obj_groups(path: str | Path) -> Tuple[List[V3], Dict[str, List[Tuple[int, int, int]]]]:
    verts: List[V3] = []
    groups: Dict[str, List[Tuple[int, int, int]]] = {}
    current = "default"
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError as e:
        raise RecipeError(f"cannot read mesh {path}: {e}") from None
    for line in lines:
        p = line.split()
        if not p or p[0].startswith("#"):
            continue
        if p[0] == "v":
            verts.append((float(p[1]), float(p[2]), float(p[3])))
        elif p[0] == "usemtl":
            current = p[1]
        elif p[0] == "f":
            idx = [int(t.split("/")[0]) - 1 for t in p[1:]]
            for k in range(1, len(idx) - 1):
                groups.setdefault(current, []).append((idx[0], idx[k], idx[k + 1]))
    return verts, groups


def _sub(a: V3, b: V3) -> V3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a: V3, b: V3) -> V3:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _norm(a: V3) -> float:
    return math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])


def surface_stats(verts: Sequence[V3], tris: Sequence[Tuple[int, int, int]]) -> Dict[str, Any]:
    area = 0.0
    c = [0.0, 0.0, 0.0]
    nsum = [0.0, 0.0, 0.0]
    for i, j, k in tris:
        a, b, d = verts[i], verts[j], verts[k]
        cr = _cross(_sub(b, a), _sub(d, a))
        ta = 0.5 * _norm(cr)
        if ta <= 0:
            continue
        area += ta
        for m in range(3):
            c[m] += ta * (a[m] + b[m] + d[m]) / 3.0
            nsum[m] += 0.5 * cr[m]
    if area <= 0:
        return {"area": 0.0}
    centroid = (c[0] / area, c[1] / area, c[2] / area)
    nlen = _norm(tuple(nsum))  # type: ignore[arg-type]
    normal = tuple(v / nlen for v in nsum) if nlen > 0 else (0.0, 0.0, 1.0)
    # principal in-plane axes: pick a tangent, project the vertices, take the extents
    up = (0.0, 1.0, 0.0) if abs(normal[1]) < 0.9 else (1.0, 0.0, 0.0)
    t = _cross(up, normal)  # type: ignore[arg-type]
    tl = _norm(t)
    t = (t[0] / tl, t[1] / tl, t[2] / tl)
    bt = _cross(normal, t)  # type: ignore[arg-type]
    used = sorted({v for tri in tris for v in tri})
    us = [sum((verts[v][m] - centroid[m]) * t[m] for m in range(3)) for v in used]
    vs = [sum((verts[v][m] - centroid[m]) * bt[m] for m in range(3)) for v in used]
    return {"area": area, "centroid": centroid, "normal": normal, "planarity": nlen / area, "tangent": t,
            "bitangent": bt, "extent": (max(us) - min(us), max(vs) - min(vs))}


def _transform(pos: V3, t: V3, bt: V3, n: V3) -> List[float]:
    return [t[0], t[1], t[2], 0.0, bt[0], bt[1], bt[2], 0.0, n[0], n[1], n[2], 0.0, pos[0], pos[1], pos[2], 1.0]


def proxy_lights(verts: Sequence[V3], groups: Dict[str, Sequence[Tuple[int, int, int]]],
                 materials: Dict[str, Dict[str, Any]], *, min_nits: float = 20.0, min_lumens: float = 50.0,
                 cutoff_lux: float = 0.1, source_id: str = "mesh", source_distribution: str = "ship"
                 ) -> List[Dict[str, Any]]:
    """POCO light + entity records for every qualifying emissive group (see the module docstring)."""
    out: List[Dict[str, Any]] = []
    for name in sorted(groups):
        mat = materials.get(name)
        if not mat:
            continue
        nits = float(mat.get("emissiveNits", 0.0))
        if mat.get("model") != "Emissive" and nits <= 0:
            continue
        if nits < min_nits:
            continue
        st = surface_stats(verts, groups[name])
        if st["area"] <= 0:
            continue
        flux = math.pi * nits * st["area"]
        if flux < min_lumens:
            continue
        base = list(mat.get("baseColor", [1.0, 1.0, 1.0, 1.0]))[:3]
        mx = max(base) if max(base) > 0 else 1.0
        colour = [round(v / mx, 6) for v in base]
        rng = math.sqrt(flux / (4 * math.pi) / cutoff_lux)
        n = st["normal"]
        if st["planarity"] >= 0.95:
            light = {"type": "Rect", "intensity": nits, "size": [round(st["extent"][0], 6), round(st["extent"][1], 6)],
                     "colorLinear": colour, "range": round(rng, 4), "castsShadows": False}
            pos = st["centroid"]
        else:
            light = {"type": "Point", "intensity": round(flux, 4), "colorLinear": colour, "range": round(rng, 4),
                     "castsShadows": False, "size": [0.0, 0.0]}
            pos = tuple(st["centroid"][m] + 0.02 * n[m] for m in range(3))
        lid = f"light/proxy/{source_id}/{name}"
        prov = {"origin": "generated", "derived_from": [f"poco:{source_id}"],
                "tool": "Tools/FUSE/Remaster/fuse_ai emissive (W5.6)", "human_authorship": "none", "recipe": None}
        dist = "recipe-only" if source_distribution in ("recipe-only", "never") else "ship"
        out.append({"schema": "fuse.poco/1", "kind": "light", "id": lid, "name": f"Proxy light for {name}",
                    "payload": light, "provenance": prov, "distribution": dist, "tags": ["proxy-light", "t0"],
                    "replaces": []})
        out.append({"schema": "fuse.poco/1", "kind": "entity", "id": f"entity/proxy/{source_id}/{name}",
                    "name": f"Proxy light entity for {name}",
                    "payload": {"archetype": "light", "assetRefs": [lid], "components": [],
                                "legacyClass": "", "legacyName": name,
                                "transform": [round(v, 6) for v in _transform(pos, st["tangent"], st["bitangent"], n)]},
                    "provenance": prov, "distribution": dist, "tags": ["proxy-light", "t0"], "replaces": []})
    return out
