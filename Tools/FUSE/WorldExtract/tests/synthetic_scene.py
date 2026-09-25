#!/usr/bin/env python3
"""CPU stand-in for a world model: ray-cast a known procedural scene along a scripted trajectory.

Produces exactly what `generate.py` produces on a GPU (MP4 + action log), plus ground truth:
    <out>/synthetic.mp4             rendered "playable video" with a static fake HUD (tests masking)
    <out>/synthetic.actions.json    action log (from generate.py --dry-run, camera hints added)
    <out>/gt/poses.npy              (N, 4, 4) cam2world, +Z up, metres, OpenCV camera axes
    <out>/gt/depth.npz              z-depth per frame, key 'f%06d' (float16)
    <out>/gt/intrinsics.json        fx, fy, cx, cy (COLMAP +0.5 convention), width, height
    <out>/gt/scene.json             primitives (for reference)

Scene: textured ground (z = 0), boxes and pillars with multi-octave colour-noise texture maps so
SIFT finds plenty of stable features. numpy + OpenCV only (no GPU).
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from worldextract import actions as act  # noqa: E402


@dataclass
class Box:
    lo: np.ndarray
    hi: np.ndarray
    tint: np.ndarray


@dataclass
class Pillar:
    cx: float
    cy: float
    r: float
    h: float
    tint: np.ndarray


def make_scene(seed: int, centre: np.ndarray, half: np.ndarray) -> tuple[list[Box], list[Pillar]]:
    """Objects scattered in a ring around the trajectory's bounding box (never on the path)."""
    rng = np.random.default_rng(seed)
    boxes: list[Box] = []
    pillars: list[Pillar] = []
    for i in range(14):
        ang = 2 * math.pi * i / 14 + rng.uniform(-0.15, 0.15)
        rad_x, rad_y = half[0] + rng.uniform(2.0, 5.0), half[1] + rng.uniform(2.0, 5.0)
        cx, cy = centre[0] + rad_x * math.cos(ang), centre[1] + rad_y * math.sin(ang)
        tint = rng.uniform(0.35, 1.0, 3)
        if i % 3 == 2:
            pillars.append(Pillar(cx, cy, rng.uniform(0.25, 0.5), rng.uniform(2.5, 4.0), tint))
        else:
            size = rng.uniform([0.6, 0.6, 0.6], [1.8, 1.8, 2.6])
            lo = np.array([cx - size[0] / 2, cy - size[1] / 2, 0.0])
            boxes.append(Box(lo, lo + size, tint))
    return boxes, pillars


def noise_texture(size: int, seed: int, cells: tuple[int, ...] = (8, 32, 128, 384)) -> np.ndarray:
    """Tileable-enough multi-octave colour noise (float32 RGB in [0, 1]) built by upsampling random grids."""
    import cv2

    rng = np.random.default_rng(seed)
    lum = np.zeros((size, size), np.float32)
    weights = (0.4, 0.3, 0.2, 0.1)
    for c, wgt in zip(cells, weights):
        grid = rng.random((c, c)).astype(np.float32)
        lum += wgt * cv2.resize(grid, (size, size), interpolation=cv2.INTER_CUBIC)
    hue = cv2.resize(rng.random((6, 6, 3)).astype(np.float32), (size, size), interpolation=cv2.INTER_CUBIC)
    speck = cv2.resize(rng.random((256, 256)).astype(np.float32), (size, size), interpolation=cv2.INTER_NEAREST)
    tex = (0.25 + 0.9 * lum)[..., None] * (0.55 + 0.7 * hue)
    tex[speck > 0.9] *= 0.35
    return np.clip(tex, 0, 1)


class Textures:
    GROUND_PPM = 64.0  # texels per metre (32 m unique)
    OBJECT_PPM = 96.0

    def __init__(self, seed: int) -> None:
        self.ground = noise_texture(2048, seed)
        self.obj = noise_texture(1024, seed + 1, cells=(6, 24, 96, 256))

    @staticmethod
    def sample(tex: np.ndarray, u: np.ndarray, v: np.ndarray) -> np.ndarray:
        import cv2

        n = tex.shape[0]
        if u.size == 0:
            return np.zeros((0, 3))
        mu = np.mod(u, n - 1).astype(np.float32).reshape(-1, 1)
        mv = np.mod(v, n - 1).astype(np.float32).reshape(-1, 1)
        return cv2.remap(tex, mu, mv, cv2.INTER_LINEAR, borderMode=cv2.BORDER_WRAP).reshape(-1, 3)


def render(cam2world: np.ndarray, k: np.ndarray, w: int, h: int, boxes: list[Box],
           pillars: list[Pillar], tex: Textures) -> tuple[np.ndarray, np.ndarray]:
    vs, us = np.mgrid[0:h, 0:w]
    d_cam = np.stack([(us + 0.5 - k[0, 2]) / k[0, 0], (vs + 0.5 - k[1, 2]) / k[1, 1], np.ones_like(us, float)], -1)
    d_cam = d_cam.reshape(-1, 3)
    r, o = cam2world[:3, :3], cam2world[:3, 3]
    d = d_cam @ r.T  # world ray directions with camera-z component 1 -> t is z-depth
    n_rays = d.shape[0]
    t_best = np.full(n_rays, np.inf)
    obj = np.full(n_rays, -1)
    normal = np.zeros((n_rays, 3))
    # Ground z = 0.
    with np.errstate(divide="ignore", invalid="ignore"):
        tg = -o[2] / d[:, 2]
    hit = (tg > 0) & (d[:, 2] < 0)
    t_best[hit], obj[hit], normal[hit] = tg[hit], 0, [0, 0, 1]
    dd = (d * d).sum(1)

    def candidates(centre: np.ndarray, radius: float) -> np.ndarray:
        """Rays passing within `radius` of `centre` (bounding-sphere cull)."""
        oc = centre - o
        tc = d @ oc / dd
        dist2 = oc @ oc - tc * tc * dd
        return np.nonzero((dist2 <= radius * radius) & (tc * np.sqrt(dd) > -radius))[0]

    # Boxes (slab test on culled rays).
    for bi, b in enumerate(boxes):
        idx = candidates((b.lo + b.hi) / 2, 0.5 * float(np.linalg.norm(b.hi - b.lo)))
        if idx.size == 0:
            continue
        dsub = d[idx]
        inv = 1.0 / np.where(np.abs(dsub) < 1e-12, 1e-12, dsub)
        t1 = (b.lo - o) * inv
        t2 = (b.hi - o) * inv
        tmin = np.minimum(t1, t2)
        tn = tmin.max(1)
        tf = np.maximum(t1, t2).min(1)
        ok = (tn <= tf) & (tn > 1e-4) & (tn < t_best[idx])
        hit = idx[ok]
        axis = tmin[ok].argmax(1)
        t_best[hit], obj[hit] = tn[ok], 1 + bi
        nrm = np.zeros((hit.size, 3))
        nrm[np.arange(hit.size), axis] = -np.sign(dsub[ok][np.arange(hit.size), axis])
        normal[hit] = nrm
    # Pillars (vertical cylinders with a cap, culled rays).
    for pi, c in enumerate(pillars):
        idx = candidates(np.array([c.cx, c.cy, c.h / 2]), math.hypot(c.r, c.h / 2))
        if idx.size == 0:
            continue
        dsub = d[idx]
        ox, oy = o[0] - c.cx, o[1] - c.cy
        a = dsub[:, 0] ** 2 + dsub[:, 1] ** 2
        bq = 2 * (ox * dsub[:, 0] + oy * dsub[:, 1])
        cq = ox * ox + oy * oy - c.r * c.r
        disc = bq * bq - 4 * a * cq
        ok = (disc > 0) & (a > 1e-12)
        with np.errstate(invalid="ignore", divide="ignore"):
            tc = (-bq - np.sqrt(np.where(ok, disc, 0))) / (2 * a)
        zc = o[2] + tc * dsub[:, 2]
        side = ok & (tc > 1e-4) & (zc >= 0) & (zc <= c.h) & (tc < t_best[idx])
        hit = idx[side]
        t_best[hit], obj[hit] = tc[side], 1 + len(boxes) + pi
        pts = o + tc[side, None] * dsub[side]
        normal[hit] = np.stack([pts[:, 0] - c.cx, pts[:, 1] - c.cy, np.zeros(hit.size)], 1) / c.r
        with np.errstate(divide="ignore", invalid="ignore"):
            tt = (c.h - o[2]) / dsub[:, 2]
        pt = o + tt[:, None] * dsub
        capm = (tt > 1e-4) & (((pt[:, 0] - c.cx) ** 2 + (pt[:, 1] - c.cy) ** 2) <= c.r * c.r) & (tt < t_best[idx])
        hit = idx[capm]
        t_best[hit], obj[hit], normal[hit] = tt[capm], 1 + len(boxes) + pi, [0, 0, 1]
    img = np.zeros((n_rays, 3))
    sky = obj < 0
    elev = np.clip(d[sky, 2] / np.linalg.norm(d[sky], axis=1), 0, 1)
    img[sky] = np.stack([0.55 + 0.2 * elev, 0.7 + 0.15 * elev, 0.95 + 0 * elev], 1)
    sun = np.array([0.4, 0.3, 0.86])
    sun /= np.linalg.norm(sun)
    hit = ~sky
    p = o + t_best[hit, None] * d[hit]
    oid = obj[hit]
    col = np.zeros((len(p), 3))
    ground = oid == 0
    gp = tex.GROUND_PPM
    col[ground] = tex.sample(tex.ground, p[ground, 0] * gp + 1000, p[ground, 1] * gp + 1000) * [0.95, 0.85, 0.7]
    nh = normal[hit]
    op = tex.OBJECT_PPM
    for i, b in enumerate(boxes):
        m = oid == 1 + i
        ax = np.abs(nh[m]).argmax(1)
        q = p[m]
        u = np.where(ax == 0, q[:, 1], q[:, 0]) + 0.37 * ax
        v = np.where(ax == 2, q[:, 1], q[:, 2])
        off = 173.0 * (i + 1)
        col[m] = tex.sample(tex.obj, u * op + off, v * op + 3 * off) * b.tint
    for i, c in enumerate(pillars):
        m = oid == 1 + len(boxes) + i
        q = p[m]
        u = np.arctan2(q[:, 1] - c.cy, q[:, 0] - c.cx) * c.r
        off = 311.0 * (i + 1)
        col[m] = tex.sample(tex.obj, u * op + off, q[:, 2] * op + 2 * off) * c.tint
    shade = 0.55 + 0.45 * np.clip(normal[hit] @ sun, 0, 1)
    img[hit] = col * shade[:, None]
    depth = np.where(sky, 0.0, t_best).reshape(h, w)
    return (np.clip(img, 0, 1).reshape(h, w, 3) * 255).astype(np.uint8), depth


def draw_hud(bgr: np.ndarray) -> None:
    import cv2

    h, w = bgr.shape[:2]
    x0, y0 = int(0.03 * w), int(0.78 * h)
    cv2.rectangle(bgr, (x0, y0), (x0 + int(0.2 * w), h - int(0.04 * h)), (40, 40, 40), -1)
    cv2.putText(bgr, "W A S D", (x0 + 6, h - int(0.08 * h)), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (230, 230, 230), 1,
                cv2.LINE_AA)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--actions", type=Path, required=True, help="action log from generate.py --dry-run")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--width", type=int, default=384)
    ap.add_argument("--height", type=int, default=212)
    ap.add_argument("--hfov-deg", type=float, default=75.0)
    ap.add_argument("--eye-height", type=float, default=1.6)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--fps", type=float, default=25.0)
    ap.add_argument("--no-hud", action="store_true")
    args = ap.parse_args(argv)
    import cv2

    log = json.loads(args.actions.read_text(encoding="utf-8"))
    kb, mouse, labels, gains = act.load_action_log(log)
    poses = act.integrate(kb, mouse, labels, gains, args.eye_height)
    c = poses[:, :3, 3]
    centre = (c.min(0) + c.max(0)) / 2
    half = (c.max(0) - c.min(0)) / 2
    boxes, pillars = make_scene(args.seed, centre, half)
    tex = Textures(args.seed)
    w, h = args.width, args.height
    f = 0.5 * w / math.tan(math.radians(args.hfov_deg) / 2)
    k = np.array([[f, 0, w / 2], [0, f, h / 2], [0, 0, 1.0]])
    out = args.out
    (out / "gt").mkdir(parents=True, exist_ok=True)
    writer = cv2.VideoWriter(str(out / "synthetic.mp4"), cv2.VideoWriter_fourcc(*"mp4v"), args.fps, (w, h))
    if not writer.isOpened():
        print("error: OpenCV cannot write MP4 (mp4v) on this system", file=sys.stderr)
        return 1
    depths = {}
    for i, pose in enumerate(poses):
        rgb, depth = render(pose, k, w, h, boxes, pillars, tex)
        bgr = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
        if not args.no_hud:
            draw_hud(bgr)
        writer.write(bgr)
        depths[f"f{i:06d}"] = depth.astype(np.float16)
    writer.release()
    np.save(out / "gt" / "poses.npy", poses)
    np.savez_compressed(out / "gt" / "depth.npz", **depths)
    (out / "gt" / "intrinsics.json").write_text(json.dumps(
        {"fx": f, "fy": f, "cx": w / 2, "cy": h / 2, "width": w, "height": h}, indent=2), encoding="utf-8")
    (out / "gt" / "scene.json").write_text(json.dumps({
        "ground": "z = 0",
        "boxes": [{"lo": b.lo.tolist(), "hi": b.hi.tolist()} for b in boxes],
        "pillars": [{"c": [p.cx, p.cy], "r": p.r, "h": p.h} for p in pillars]}, indent=2), encoding="utf-8")
    log["model"] = {"name": "synthetic-raycaster", "hf_repo": "none (CPU test renderer)", "revision": "n/a",
                    "licence": "MIT (FUSE test code)", "checkpoint_sha256": None, "licence_review": "none"}
    log["video"] = {"width": w, "height": h, "fps": args.fps, "num_frames": int(len(poses))}
    # A deliberately imperfect FOV hint: COLMAP must refine the focal length.
    log["camera"] = {"hfov_deg": args.hfov_deg - 5.0, "eye_height_m": args.eye_height}
    (out / "synthetic.actions.json").write_text(json.dumps(log, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
