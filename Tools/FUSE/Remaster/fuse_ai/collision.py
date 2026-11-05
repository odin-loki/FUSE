"""W5.2: collision-vs-render check (plan §4.3 step 4).

Track B keeps the original collision hulls for gameplay parity and checks them against the new render
mesh: the symmetric Hausdorff distance between the two surfaces must stay within a tolerance. Both directions
are reported because they fail differently:

* ``render_to_hull`` large with the worst render sample *outside* the hull volume (ray-parity test) - the
  render mesh pokes out of the collision (players walk *into* visible geometry);
* otherwise - invisible collision ("invisible walls") where nothing is drawn.

The distance is measured from dense surface samples (vertices, edge midpoints, centroids and a deterministic
barycentric grid on each triangle) to the exact closest point on the other mesh's triangles (Ericson,
*Real-Time Collision Detection* §5.1.5). Meshes are Wavefront OBJ (``v`` / ``f``, polygons fan-triangulated)
or in-memory vertex/triangle lists. Pure Python: the gate meshes are small.
"""

from __future__ import annotations

import math
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

from .errors import GateFailure, RecipeError

V3 = Tuple[float, float, float]
Tri = Tuple[int, int, int]


def load_obj(path: str | Path) -> Tuple[List[V3], List[Tri]]:
    verts: List[V3] = []
    tris: List[Tri] = []
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError as e:
        raise RecipeError(f"cannot read mesh {path}: {e}") from None
    for n, line in enumerate(lines, 1):
        parts = line.split()
        if not parts or parts[0].startswith("#"):
            continue
        if parts[0] == "v":
            verts.append((float(parts[1]), float(parts[2]), float(parts[3])))
        elif parts[0] == "f":
            idx = []
            for p in parts[1:]:
                i = int(p.split("/")[0])
                idx.append(i - 1 if i > 0 else len(verts) + i)
            for k in range(1, len(idx) - 1):
                tris.append((idx[0], idx[k], idx[k + 1]))
    if not verts or not tris:
        raise RecipeError(f"mesh {path} has no triangles")
    for t in tris:
        if max(t) >= len(verts) or min(t) < 0:
            raise RecipeError(f"mesh {path}: face index out of range")
    return verts, tris


def _sub(a: V3, b: V3) -> V3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a: V3, b: V3) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def closest_point_on_triangle(p: V3, a: V3, b: V3, c: V3) -> V3:
    ab, ac, ap = _sub(b, a), _sub(c, a), _sub(p, a)
    d1, d2 = _dot(ab, ap), _dot(ac, ap)
    if d1 <= 0 and d2 <= 0:
        return a
    bp = _sub(p, b)
    d3, d4 = _dot(ab, bp), _dot(ac, bp)
    if d3 >= 0 and d4 <= d3:
        return b
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        v = d1 / (d1 - d3)
        return (a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2])
    cp = _sub(p, c)
    d5, d6 = _dot(ab, cp), _dot(ac, cp)
    if d6 >= 0 and d5 <= d6:
        return c
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        w = d2 / (d2 - d6)
        return (a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2])
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        return (b[0] + w * (c[0] - b[0]), b[1] + w * (c[1] - b[1]), b[2] + w * (c[2] - b[2]))
    denom = 1.0 / (va + vb + vc)
    v, w = vb * denom, vc * denom
    return (a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w)


def surface_samples(verts: Sequence[V3], tris: Sequence[Tri], grid: int = 4) -> List[V3]:
    pts = list(verts)
    for i, j, k in tris:
        a, b, c = verts[i], verts[j], verts[k]
        for s in range(grid + 1):
            for t in range(grid + 1 - s):
                u, v = s / grid, t / grid
                w = 1 - u - v
                pts.append((a[0] * w + b[0] * u + c[0] * v, a[1] * w + b[1] * u + c[1] * v,
                            a[2] * w + b[2] * u + c[2] * v))
    return pts


def one_sided(src: Tuple[List[V3], List[Tri]], dst: Tuple[List[V3], List[Tri]], grid: int = 4) -> Tuple[float, V3]:
    dv, dt = dst
    tris = [(dv[i], dv[j], dv[k]) for i, j, k in dt]
    worst, at = 0.0, (0.0, 0.0, 0.0)
    for p in surface_samples(src[0], src[1], grid):
        best = math.inf
        for a, b, c in tris:
            q = closest_point_on_triangle(p, a, b, c)
            d = (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2
            if d < best:
                best = d
                if best == 0.0:
                    break
        if best > worst:
            worst, at = best, p
    return math.sqrt(worst), at


def inside(p: V3, mesh: Tuple[List[V3], List[Tri]]) -> bool:
    """Ray-parity point-in-mesh test (closed meshes; a skewed ray avoids edge hits)."""
    d = (1.0, 0.0123, 0.0071)
    hits = 0
    verts, tris = mesh
    for i, j, k in tris:
        a, b, c = verts[i], verts[j], verts[k]
        e1, e2 = _sub(b, a), _sub(c, a)
        h = (d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0])
        det = _dot(e1, h)
        if abs(det) < 1e-12:
            continue
        f = 1.0 / det
        s = _sub(p, a)
        u = f * _dot(s, h)
        if u < 0.0 or u > 1.0:
            continue
        q = (s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0])
        v = f * _dot(d, q)
        if v < 0.0 or u + v > 1.0:
            continue
        if f * _dot(e2, q) > 1e-9:
            hits += 1
    return hits % 2 == 1


def bounds(verts: Sequence[V3]) -> Tuple[V3, V3]:
    return (tuple(min(v[i] for v in verts) for i in range(3)), tuple(max(v[i] for v in verts) for i in range(3)))  # type: ignore[return-value]


def check(render: Tuple[List[V3], List[Tri]], hull: Tuple[List[V3], List[Tri]], *, tolerance: float,
          grid: int = 4) -> Dict[str, Any]:
    r2h, r_at = one_sided(render, hull, grid)
    h2r, h_at = one_sided(hull, render, grid)
    rb, hb = bounds(render[0]), bounds(hull[0])
    # Surface distances are symmetric for nested shapes; whether the worst render sample lies outside the hull
    # volume tells "geometry you can walk into" apart from "collision where nothing is drawn".
    render_pokes_out = r2h > tolerance and not inside(r_at, hull)
    return {"tolerance": tolerance, "render_to_hull": r2h, "hull_to_render": h2r, "hausdorff": max(r2h, h2r),
            "worst_render_point": r_at, "worst_hull_point": h_at, "render_bounds": rb, "hull_bounds": hb,
            "render_outside_hull": render_pokes_out, "ok": max(r2h, h2r) <= tolerance}


def check_files(render_obj: str | Path, hull_obj: str | Path, *, tolerance: float) -> Dict[str, Any]:
    res = check(load_obj(render_obj), load_obj(hull_obj), tolerance=tolerance)
    res["render"], res["hull"] = str(render_obj), str(hull_obj)
    return res


def gate(render_obj: str | Path, hull_obj: str | Path, *, tolerance: float) -> Dict[str, Any]:
    res = check_files(render_obj, hull_obj, tolerance=tolerance)
    if not res["ok"]:
        side = ("render mesh outside the collision hull" if res["render_outside_hull"]
                else "collision hull where nothing is drawn")
        raise GateFailure(f"collision vs render: Hausdorff {res['hausdorff']:.4f} > {tolerance} ({side}; "
                          f"render->hull {res['render_to_hull']:.4f}, hull->render {res['hull_to_render']:.4f})")
    return res
