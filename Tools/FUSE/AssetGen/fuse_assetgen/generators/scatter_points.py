"""scatter_points: Poisson-disc-like dart throwing in a square tile (JSON), e.g. a vegetation scatter.

Grid-accelerated rejection sampling with a fixed attempt budget; every coordinate is quantised to
millimetres and written with sorted keys, so the JSON bytes are platform independent.
"""

from __future__ import annotations

import json
import math

from ..registry import generator

PARAMS = {
    "extent_m": {"type": "number", "minimum": 1.0, "maximum": 4096.0, "default": 64.0},
    "min_distance_m": {"type": "number", "minimum": 0.05, "maximum": 512.0, "default": 2.0},
    "attempts": {"type": "int", "minimum": 1, "maximum": 200000, "default": 4000},
    "scale_range": {"type": "array", "items": {"type": "number", "minimum": 0.01, "maximum": 100.0},
                    "min_items": 2, "max_items": 2, "default": [0.8, 1.2]},
}


@generator("scatter_points", version=1, params=PARAMS,
           description="Minimum-distance dart-throwing scatter in a square tile (positions, yaw, scale) as JSON")
def generate(ctx) -> None:
    p = ctx.params
    extent = float(p["extent_m"])
    r = float(p["min_distance_m"])
    lo, hi = sorted(p["scale_range"])
    cell = r / math.sqrt(2.0)
    n = max(1, int(extent / cell) + 1)
    grid = {}
    pos_rng = ctx.stream.derive("position").rng()
    attr_rng = ctx.stream.derive("attributes").rng()
    points = []
    r2 = r * r
    for _ in range(p["attempts"]):
        x = pos_rng.uniform() * extent
        y = pos_rng.uniform() * extent
        gx, gy = int(x / cell), int(y / cell)
        ok = True
        for yy in range(gy - 2, gy + 3):
            for xx in range(gx - 2, gx + 3):
                q = grid.get((xx, yy))
                if q is not None and (q[0] - x) * (q[0] - x) + (q[1] - y) * (q[1] - y) < r2:
                    ok = False
                    break
            if not ok:
                break
        if not ok or gx >= n or gy >= n:
            continue
        grid[(gx, gy)] = (x, y)
        points.append({
            "x_mm": int(round(x * 1000.0)),
            "y_mm": int(round(y * 1000.0)),
            "yaw_mdeg": attr_rng.below(360000),
            "scale_milli": int(round((lo + (hi - lo) * attr_rng.uniform()) * 1000.0)),
        })
    doc = {"schema": 1, "extent_mm": int(round(extent * 1000.0)), "min_distance_mm": int(round(r * 1000.0)),
           "count": len(points), "points": points}
    ctx.emit("scatter.json", (json.dumps(doc, sort_keys=True, indent=1, ensure_ascii=True) + "\n").encode("ascii"))
