"""Stage `fuse`: TSDF fusion (Open3D, MIT) -> mesh clean-up -> simplification -> UVs + colour bake.

Outputs (in <work>/fuse/):
    mesh_raw.ply     marching-cubes TSDF surface with vertex colours
    mesh.npz         cleaned + simplified mesh: positions, normals, colours, uvs, faces (+Z up, metres)
    atlas.png        baked albedo atlas (when --texture atlas and xatlas is installed)
    fuse.json        report (voxel size, component filtering, triangle counts)
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from ..common import WorldExtractError, missing_modules, write_json
from ..geometry import invert_pose
from . import poses as poses_stage

LOG = logging.getLogger("worldextract.fuse")


@dataclass
class FuseParams:
    voxel: float = 0.0  # metres; 0 = auto (scene extent / 400, clamped to [0.02, 0.25])
    trunc_voxels: float = 4.0
    depth_trunc: float = 0.0  # 0 = depth.json max_depth
    min_component_ratio: float = 0.002  # drop connected components smaller than this x largest
    min_component_tris: int = 50
    target_tris: int = 200_000
    texture: str = "atlas"  # atlas | vertex
    atlas_size: int = 2048
    block_count: int = 60_000  # voxel blocks (8^3) pre-allocated for the TSDF hash map


def _o3d_intrinsic(k: np.ndarray):  # type: ignore[no-untyped-def]
    import open3d.core as o3c

    # Open3D projects to pixel *indices*; WorldExtract K uses COLMAP's +0.5 pixel-centre convention.
    kk = k.copy()
    kk[0, 2] -= 0.5
    kk[1, 2] -= 0.5
    return o3c.Tensor(kk, o3c.float64)


def _clean(mesh, p: FuseParams) -> tuple[Any, dict[str, Any]]:  # type: ignore[no-untyped-def]
    mesh.remove_degenerate_triangles()
    mesh.remove_duplicated_triangles()
    mesh.remove_duplicated_vertices()
    mesh.remove_non_manifold_edges()
    clusters, counts, _ = mesh.cluster_connected_triangles()
    clusters = np.asarray(clusters)
    counts = np.asarray(counts)
    if counts.size == 0:
        raise WorldExtractError("TSDF produced an empty mesh (check depth stage output / --voxel)")
    keep_min = max(p.min_component_tris, p.min_component_ratio * counts.max())
    remove = counts[clusters] < keep_min
    mesh.remove_triangles_by_mask(remove)
    mesh.remove_unreferenced_vertices()
    return mesh, {"components": int(counts.size), "components_kept": int((counts >= keep_min).sum()),
                  "floater_triangles_removed": int(remove.sum())}


def _bake_atlas(verts: np.ndarray, faces: np.ndarray, colors: np.ndarray, uvs: np.ndarray, size: int,
                frames: list[tuple[np.ndarray, np.ndarray, np.ndarray]], k: np.ndarray) -> np.ndarray:
    """Project every texel into the keyframe that sees its triangle most frontally (depth-tested)."""
    import cv2

    tri_id = np.full((size, size), -1, np.int32)
    # xatlas and glTF share the top-left UV origin, so texel row = v * size (no flip).
    px = uvs * np.array([size, size]) - 0.5
    for t, f in enumerate(faces):
        cv2.fillConvexPoly(tri_id, np.round(px[f] * 4).astype(np.int32), int(t), lineType=cv2.LINE_8, shift=2)
    ys, xs = np.nonzero(tri_id >= 0)
    tids = tri_id[ys, xs]
    # Barycentric coordinates of each texel inside its triangle (in UV pixel space).
    a, b, c = px[faces[tids, 0]], px[faces[tids, 1]], px[faces[tids, 2]]
    q = np.stack([xs, ys], 1).astype(np.float64)
    v0, v1, v2 = b - a, c - a, q - a
    d00, d01, d11 = (v0 * v0).sum(1), (v0 * v1).sum(1), (v1 * v1).sum(1)
    d20, d21 = (v2 * v0).sum(1), (v2 * v1).sum(1)
    den = d00 * d11 - d01 * d01
    den = np.where(np.abs(den) < 1e-12, 1e-12, den)
    wv = (d11 * d20 - d01 * d21) / den
    ww = (d00 * d21 - d01 * d20) / den
    bary = np.clip(np.stack([1 - wv - ww, wv, ww], 1), 0, 1)
    bary /= bary.sum(1, keepdims=True)
    pos = (bary[:, :, None] * verts[faces[tids]]).sum(1)
    col = (bary[:, :, None] * colors[faces[tids]]).sum(1)  # fallback: vertex colour

    # Per-triangle best view.
    fn = np.cross(verts[faces[:, 1]] - verts[faces[:, 0]], verts[faces[:, 2]] - verts[faces[:, 0]])
    fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-12)
    cen = verts[faces].mean(1)
    best_score = np.zeros(len(faces))
    best_view = np.full(len(faces), -1)
    for vi, (c2w, _img, depth) in enumerate(frames):
        w2c = invert_pose(c2w)
        pc = (w2c[:3, :3] @ cen.T).T + w2c[:3, 3]
        z = pc[:, 2]
        with np.errstate(divide="ignore", invalid="ignore"):
            u = k[0, 0] * pc[:, 0] / z + k[0, 2]
            v = k[1, 1] * pc[:, 1] / z + k[1, 2]
        h, w = depth.shape
        inb = (z > 1e-3) & (u >= 0) & (u < w) & (v >= 0) & (v < h)
        ui = np.clip(u.astype(int), 0, w - 1)
        vi_ = np.clip(v.astype(int), 0, h - 1)
        dz = depth[vi_, ui]
        vis = inb & (dz > 0) & (np.abs(dz - z) < 0.03 * z + 0.05)
        view_dir = c2w[:3, 3] - cen
        dist = np.linalg.norm(view_dir, axis=1)
        cos = np.abs((fn * view_dir).sum(1)) / np.maximum(dist, 1e-9)
        score = np.where(vis, cos / np.maximum(dist, 1e-3), 0.0)
        better = score > best_score
        best_score[better] = score[better]
        best_view[better] = vi
    tex_view = best_view[tids]
    for vi, (c2w, img, _depth) in enumerate(frames):
        sel = tex_view == vi
        if not sel.any():
            continue
        w2c = invert_pose(c2w)
        pc = (w2c[:3, :3] @ pos[sel].T).T + w2c[:3, 3]
        u = (k[0, 0] * pc[:, 0] / pc[:, 2] + k[0, 2] - 0.5).astype(np.float32)
        v = (k[1, 1] * pc[:, 1] / pc[:, 2] + k[1, 2] - 0.5).astype(np.float32)
        sampled = cv2.remap(img, u.reshape(-1, 1), v.reshape(-1, 1), cv2.INTER_LINEAR,
                            borderMode=cv2.BORDER_REPLICATE).reshape(-1, 3)
        col[sel] = sampled[:, ::-1] / 255.0  # BGR -> RGB
    atlas = np.zeros((size, size, 3), np.float32)
    atlas[ys, xs] = col
    filled = (tri_id >= 0).astype(np.uint8)
    # Dilate into gutters so bilinear sampling / mips do not bleed black.
    img8 = (np.clip(atlas, 0, 1) * 255).astype(np.uint8)
    for _ in range(8):
        dil = cv2.dilate(img8, np.ones((3, 3), np.uint8))
        grow = cv2.dilate(filled, np.ones((3, 3), np.uint8))
        new = (grow > 0) & (filled == 0)
        img8[new] = dil[new]
        filled = grow
    return img8  # RGB


def run(work: Path, out: Path, p: FuseParams, seed: int) -> dict[str, Any]:
    import cv2
    import open3d as o3d

    from ..common import read_json

    info, poses, k = poses_stage.load(work)
    dinfo = read_json(work / "depth" / "depth.json")
    image_dir = Path(info["image_dir"])
    centres = poses[:, :3, 3]
    pts = np.load(work / "poses" / "points.npz")["xyz"]
    ref = pts if len(pts) else centres
    extent = float(np.linalg.norm(np.percentile(ref, 95, 0) - np.percentile(ref, 5, 0)))
    voxel = p.voxel or float(np.clip(extent / 400.0, 0.02, 0.25))
    depth_trunc = p.depth_trunc or float(dinfo["max_depth"])
    import open3d.core as o3c

    # Tensor VoxelBlockGrid (the legacy ScalableTSDFVolume returns empty meshes in open3d 0.20 wheels).
    intr = _o3d_intrinsic(k)
    vol = o3d.t.geometry.VoxelBlockGrid(
        attr_names=("tsdf", "weight", "color"), attr_dtypes=(o3c.float32, o3c.float32, o3c.float32),
        attr_channels=((1), (1), (3)), voxel_size=voxel, block_resolution=8, block_count=p.block_count,
        device=o3c.Device("CPU:0"))
    frames_cache: list[tuple[np.ndarray, np.ndarray, np.ndarray]] = []
    for fr, c2w in zip(info["frames"], poses):
        bgr = cv2.imread(str(image_dir / fr["name"]), cv2.IMREAD_COLOR)
        if bgr is None:
            raise WorldExtractError(f"missing keyframe image {image_dir / fr['name']}")
        depth = np.load(work / "depth" / (fr["name"] + ".npy")).astype(np.float32)
        rgb = np.ascontiguousarray(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0)
        ext = o3c.Tensor(invert_pose(c2w), o3c.float64)
        d_t = o3d.t.geometry.Image(o3c.Tensor(np.ascontiguousarray(depth)))
        c_t = o3d.t.geometry.Image(o3c.Tensor(rgb))
        blocks = vol.compute_unique_block_coordinates(d_t, intr, ext, 1.0, depth_trunc, p.trunc_voxels)
        vol.integrate(blocks, d_t, c_t, intr, intr, ext, 1.0, depth_trunc, p.trunc_voxels)
        frames_cache.append((c2w, bgr, depth))
    mesh = vol.extract_triangle_mesh().to_legacy()
    raw_tris = len(mesh.triangles)
    if raw_tris == 0:
        raise WorldExtractError("TSDF fusion produced no surface; depth maps may be empty or misaligned")
    o3d.io.write_triangle_mesh(str(out / "mesh_raw.ply"), mesh)  # type: ignore[arg-type]
    mesh, clean_info = _clean(mesh, p)
    if len(mesh.triangles) > p.target_tris:
        mesh = mesh.simplify_quadric_decimation(p.target_tris)
        mesh, _ = _clean(mesh, FuseParams(min_component_ratio=0.0, min_component_tris=0))
    mesh.compute_vertex_normals()
    verts = np.asarray(mesh.vertices, dtype=np.float64)
    faces = np.asarray(mesh.triangles, dtype=np.int64)
    colors = np.asarray(mesh.vertex_colors, dtype=np.float64)
    normals = np.asarray(mesh.vertex_normals, dtype=np.float64)
    LOG.info("TSDF voxel %.3f m: %d raw -> %d final triangles (%d floater tris removed)", voxel, raw_tris,
             len(faces), clean_info["floater_triangles_removed"])

    texture = p.texture
    uvs = None
    if texture == "atlas" and missing_modules(["xatlas"]):
        LOG.warning("xatlas not installed: falling back to --texture vertex with planar UVs")
        texture = "vertex"
    if texture == "atlas":
        import xatlas

        atlas = xatlas.Atlas()
        atlas.add_mesh(verts.astype(np.float32), faces.astype(np.uint32), normals.astype(np.float32))
        popt = xatlas.PackOptions()
        popt.resolution = p.atlas_size
        popt.padding = 2
        atlas.generate(pack_options=popt)
        vmap, new_faces, uvs = atlas[0]
        verts, colors, normals = verts[vmap], colors[vmap], normals[vmap]
        faces = new_faces.astype(np.int64)
        uvs = uvs.astype(np.float64)
        img = _bake_atlas(verts, faces, colors, uvs, p.atlas_size, frames_cache, k)
        cv2.imwrite(str(out / "atlas.png"), cv2.cvtColor(img, cv2.COLOR_RGB2BGR))
    else:
        # Planar top-down UVs (metres / tile) so the mesh still carries TEXCOORD_0 for material layering.
        lo = verts[:, :2].min(0)
        uvs = (verts[:, :2] - lo) / max(float((verts[:, :2].max(0) - lo).max()), 1e-9)
        stale = out / "atlas.png"
        if stale.exists():
            stale.unlink()
    np.savez_compressed(out / "mesh.npz", positions=verts.astype(np.float32), normals=normals.astype(np.float32),
                        colors=colors.astype(np.float32), uvs=uvs.astype(np.float32), faces=faces.astype(np.uint32))
    report = {"voxel_m": voxel, "sdf_trunc_m": p.trunc_voxels * voxel, "depth_trunc_m": depth_trunc,
              "raw_triangles": raw_tris, "triangles": int(len(faces)), "vertices": int(len(verts)),
              "texture": texture, "atlas_size": p.atlas_size if texture == "atlas" else 0, **clean_info}
    write_json(out / "fuse.json", report)
    return report
