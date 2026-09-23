"""Stage `collision`: simplified collision mesh + walkable ground estimate.

Outputs (in <work>/collision/):
    collision.npz    positions, faces, walkable (per-face bool)
    collision.json   ground plane (RANSAC), walkable ratio, spawn point, bounds, kill height
"""

from __future__ import annotations

import logging
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from ..common import write_json
from ..geometry import angle_deg, fit_plane_ransac
from . import poses as poses_stage

LOG = logging.getLogger("worldextract.collision")


@dataclass
class CollisionParams:
    target_tris: int = 8000
    max_slope_deg: float = 40.0
    plane_threshold: float = 0.05  # metres
    spawn_height: float = 0.05  # metres above the ground under the first camera


def run(work: Path, out: Path, p: CollisionParams, seed: int) -> dict[str, Any]:
    import open3d as o3d

    m = np.load(work / "fuse" / "mesh.npz")
    info, poses, _ = poses_stage.load(work)
    mesh = o3d.geometry.TriangleMesh(o3d.utility.Vector3dVector(m["positions"].astype(np.float64)),
                                     o3d.utility.Vector3iVector(m["faces"].astype(np.int32)))
    mesh.remove_duplicated_vertices()  # atlas seams split vertices; weld before decimating
    if len(mesh.triangles) > p.target_tris:
        mesh = mesh.simplify_quadric_decimation(p.target_tris)
    mesh.remove_degenerate_triangles()
    mesh.remove_unreferenced_vertices()
    mesh.compute_triangle_normals()
    v = np.asarray(mesh.vertices)
    f = np.asarray(mesh.triangles)
    fn = np.asarray(mesh.triangle_normals)
    up = np.array([0.0, 0.0, 1.0])

    # Ground plane: RANSAC over the fused surface (area-weighted sample of face centroids).
    cen = v[f].mean(1)
    area = 0.5 * np.linalg.norm(np.cross(v[f[:, 1]] - v[f[:, 0]], v[f[:, 2]] - v[f[:, 0]]), axis=1)
    rng = np.random.default_rng(seed)
    n_s = min(20000, len(f))
    sample = cen[rng.choice(len(f), n_s, replace=True, p=area / area.sum())]
    ground: dict[str, Any]
    try:
        n, d, inl = fit_plane_ransac(sample, p.plane_threshold, rng, 800, up, 30.0)
        ground = {"normal": [round(float(x), 6) for x in n], "d": round(float(d), 6),
                  "inlier_ratio": round(float(inl.mean()), 4), "tilt_deg": round(angle_deg(n, up), 4),
                  "height_at_origin": round(-float(d) / float(n[2]), 4)}
    except ValueError:
        n, d = up, 0.0
        ground = {"normal": [0.0, 0.0, 1.0], "d": 0.0, "inlier_ratio": 0.0, "tilt_deg": None,
                  "height_at_origin": 0.0, "note": "RANSAC failed; assumed z = 0 from the poses stage"}

    cos_max = math.cos(math.radians(p.max_slope_deg))
    fn_up = fn @ up
    walkable = fn_up >= cos_max
    walk_area = float(area[walkable].sum())
    c0 = poses[0, :3, 3]
    ground_z = -(float(n[0]) * c0[0] + float(n[1]) * c0[1] + d) / float(n[2])
    spawn = [float(c0[0]), float(c0[1]), ground_z + p.spawn_height]
    fwd = poses[0, :3, 2]
    spawn_yaw = math.degrees(math.atan2(fwd[1], fwd[0]))
    lo, hi = v.min(0), v.max(0)
    report: dict[str, Any] = {
        "triangles": int(len(f)),
        "ground_plane": ground,
        "walkable": {"max_slope_deg": p.max_slope_deg, "faces": int(walkable.sum()),
                     "ratio": round(float(walkable.mean()), 4), "area_m2": round(walk_area, 3)},
        "spawn": {"position": [round(x, 5) for x in spawn], "yaw_deg": round(spawn_yaw, 4),
                  "eye_height_m": round(float(c0[2] - ground_z), 4)},
        "bounds": {"min": [round(float(x), 4) for x in lo], "max": [round(float(x), 4) for x in hi]},
        "kill_z": round(float(lo[2]) - 10.0, 3),
    }
    np.savez_compressed(out / "collision.npz", positions=v.astype(np.float32), faces=f.astype(np.uint32),
                        walkable=walkable)
    write_json(out / "collision.json", report)
    LOG.info("collision: %d tris, ground tilt %s deg, walkable %.1f%%", len(f), ground["tilt_deg"],
             100 * report["walkable"]["ratio"])
    return {"triangles": len(f), "ground_tilt_deg": ground["tilt_deg"], "walkable_ratio": report["walkable"]["ratio"]}
