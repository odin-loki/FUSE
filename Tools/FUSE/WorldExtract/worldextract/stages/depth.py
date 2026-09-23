"""Stage `depth`: per-keyframe dense depth, scale/shift-aligned to the SfM sparse points.

Backends (all FOSS; see README licence table):
    moge3        MoGe-3 (microsoft/MoGe, MIT code; Ruicheng/moge-3-vitl, MIT weights; released 2026-08-18).
                 Metric depth, SfM focal passed as fov_x -> per-frame scale alignment. Default on GPU.
    moge2        MoGe-2 (Ruicheng/moge-2-vitl-normal, MIT weights). Same interface, no FlexGEMM/Triton.
    da2-small    Depth Anything V2 *Small* (Apache-2.0; Base/Large are CC-BY-NC and are NOT allowed).
                 Relative inverse depth -> per-frame affine alignment in disparity space.
    mapanything  depth predicted jointly with the poses (poses stage backend mapanything); rescaled only.
    sfm          no neural network: sparse SfM depths interpolated over an image-space Delaunay mesh.
    gt           ground-truth depth from the synthetic test (`--gt-depth depth.npz`).
    auto         mapanything if the poses came from it, else moge3 / moge2 / da2-small with CUDA, else sfm.

Output: <work>/depth/<keyframe>.npy float32 metres (0 = invalid) + depth.json (alignment report).
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

import numpy as np

from ..common import WorldExtractError, cuda_available, missing_modules, write_json
from . import poses as poses_stage

LOG = logging.getLogger("worldextract.depth")


@dataclass
class DepthParams:
    backend: str = "auto"
    gt_depth: str = ""
    max_depth: float = 0.0  # 0 = auto (1.25 x 95th percentile of sparse depths)
    smooth_window: int = 5  # temporal median window for per-frame scales
    edge_px: float = 0.0  # sfm backend: drop Delaunay triangles with an edge longer than this (0 = auto)
    moge3_model: str = "Ruicheng/moge-3-vitl"
    moge2_model: str = "Ruicheng/moge-2-vitl-normal"
    da2_model: str = "depth-anything/Depth-Anything-V2-Small-hf"


def _fit_scale(pred: np.ndarray, ref: np.ndarray) -> float:
    ok = (pred > 0) & (ref > 0)
    if ok.sum() < 5:
        return float("nan")
    r = np.log(ref[ok]) - np.log(pred[ok])
    med = float(np.median(r))
    keep = np.abs(r - med) < 3 * (float(np.median(np.abs(r - med))) + 1e-6)
    return float(np.exp(np.mean(r[keep])))


def _fit_affine_disparity(pred_disp: np.ndarray, ref_depth: np.ndarray) -> tuple[float, float]:
    ok = (ref_depth > 0) & np.isfinite(pred_disp)
    if ok.sum() < 8:
        return float("nan"), float("nan")
    x, y = pred_disp[ok], 1.0 / ref_depth[ok]
    w = np.ones_like(x)
    a = b = 0.0
    for _ in range(10):  # IRLS with Huber weights
        A = np.stack([x * w, w], 1)
        a, b = np.linalg.lstsq(A, y * w, rcond=None)[0]
        res = np.abs(a * x + b - y)
        delta = 1.345 * (np.median(res) / 0.6745 + 1e-9)
        w = np.sqrt(np.where(res <= delta, 1.0, delta / np.maximum(res, 1e-12)))
    return float(a), float(b)


def _sample(depth: np.ndarray, uv: np.ndarray) -> np.ndarray:
    h, w = depth.shape
    u = np.clip((uv[:, 0] - 0.5).round().astype(int), 0, w - 1)
    v = np.clip((uv[:, 1] - 0.5).round().astype(int), 0, h - 1)
    return depth[v, u]


def _sfm_interp(uv: np.ndarray, z: np.ndarray, h: int, w: int, edge_px: float) -> np.ndarray:
    from scipy.spatial import Delaunay

    out = np.zeros((h, w), np.float32)
    if uv.shape[0] < 4:
        return out
    tri = Delaunay(uv)
    vs, us = np.mgrid[0:h, 0:w]
    q = np.stack([us.ravel() + 0.5, vs.ravel() + 0.5], 1)
    simplex = tri.find_simplex(q)
    inside = simplex >= 0
    corners = uv[tri.simplices]  # (T, 3, 2)
    edges = np.stack([np.linalg.norm(corners[:, i] - corners[:, (i + 1) % 3], axis=1) for i in range(3)], 1)
    good_tri = edges.max(1) <= edge_px
    inside &= np.where(inside, good_tri[np.maximum(simplex, 0)], False)
    t = tri.transform[simplex[inside]]
    bary2 = np.einsum("nij,nj->ni", t[:, :2], q[inside] - t[:, 2])
    bary = np.c_[bary2, 1 - bary2.sum(1)]
    inv = 1.0 / z[tri.simplices[simplex[inside]]]
    out.ravel()[np.nonzero(inside)[0]] = 1.0 / np.maximum((bary * inv).sum(1), 1e-9)
    return out


def _load_image(path: Path) -> np.ndarray:
    import cv2

    img = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if img is None:
        raise WorldExtractError(f"cannot read keyframe {path}")
    return cv2.cvtColor(img, cv2.COLOR_BGR2RGB)


def _moge_predictor(model_id: str, version: int, fov_x_deg: float) -> Callable[[np.ndarray], np.ndarray]:
    """MoGe API per microsoft/MoGe README (2026-09-23): MoGeModel.from_pretrained(...).infer(image[3,H,W])."""
    import importlib

    import torch

    MoGeModel = importlib.import_module(f"moge.model.v{version}").MoGeModel
    model = MoGeModel.from_pretrained(model_id).to("cuda").eval()

    def predict(rgb: np.ndarray) -> np.ndarray:
        x = torch.tensor(rgb / 255.0, dtype=torch.float32, device="cuda").permute(2, 0, 1)
        with torch.no_grad():
            try:
                out = model.infer(x, fov_x=fov_x_deg)  # known FOV from SfM sharpens the geometry
            except TypeError:
                out = model.infer(x)
        d = out["depth"].float().cpu().numpy()
        d[~out["mask"].cpu().numpy().astype(bool)] = 0.0
        d[~np.isfinite(d)] = 0.0
        return d

    return predict


def _da2_predictor(model_id: str) -> Callable[[np.ndarray], np.ndarray]:
    import torch
    from transformers import AutoImageProcessor, AutoModelForDepthEstimation  # type: ignore[import-not-found]

    proc = AutoImageProcessor.from_pretrained(model_id)
    model = AutoModelForDepthEstimation.from_pretrained(model_id).to("cuda").eval()

    def predict(rgb: np.ndarray) -> np.ndarray:
        inputs = proc(images=rgb, return_tensors="pt").to("cuda")
        with torch.no_grad():
            pred = model(**inputs).predicted_depth  # relative inverse depth
        pred = torch.nn.functional.interpolate(pred[:, None], size=rgb.shape[:2], mode="bicubic",
                                               align_corners=False)[0, 0]
        return pred.float().cpu().numpy()

    return predict


def run(work: Path, out: Path, p: DepthParams) -> dict[str, Any]:
    import cv2

    info, poses, k = poses_stage.load(work)
    pts = np.load(work / "poses" / "points.npz")
    frames = info["frames"]
    h, w = int(info["intrinsics"]["height"]), int(info["intrinsics"]["width"])
    image_dir = Path(info["image_dir"])
    hud_path = work / "frames" / "hud_mask.png"
    hud = cv2.imread(str(hud_path), cv2.IMREAD_GRAYSCALE) if hud_path.exists() else None
    if hud is not None and hud.shape != (h, w):
        hud = None  # different resolution (mapanything images): HUD was masked at feature level

    backend = p.backend
    if backend == "auto":
        if info["backend"] == "mapanything":
            backend = "mapanything"
        elif cuda_available() and not missing_modules(["moge.model.v3"]):
            backend = "moge3"
        elif cuda_available() and not missing_modules(["moge"]):
            backend = "moge2"
        elif cuda_available() and not missing_modules(["transformers"]):
            backend = "da2-small"
        else:
            backend = "sfm"
        LOG.info("depth backend auto -> %s", backend)

    gt: Any = None
    if backend == "gt":
        if not p.gt_depth:
            raise WorldExtractError("--depth-backend gt needs --gt-depth <depth.npz>")
        gt = np.load(p.gt_depth)
    predict: Callable[[np.ndarray], np.ndarray] | None = None
    fov_x = float(np.degrees(2 * np.arctan(0.5 * w / k[0, 0])))
    if backend == "moge3":
        predict = _moge_predictor(p.moge3_model, 3, fov_x)
    elif backend == "moge2":
        predict = _moge_predictor(p.moge2_model, 2, fov_x)
    elif backend == "da2-small":
        predict = _da2_predictor(p.da2_model)
    elif backend not in ("sfm", "gt", "mapanything"):
        raise WorldExtractError(f"unknown depth backend {backend}")

    obs_f, obs_uv, obs_z = pts["obs_frame"], pts["obs_uv"], pts["obs_depth"]
    all_z = obs_z[obs_z > 0]
    max_depth = p.max_depth or (1.25 * float(np.percentile(all_z, 95)) if all_z.size else 100.0)
    edge_px = p.edge_px or 0.12 * w
    scale_factor = float(info["scale"]["factor"])

    raw: dict[str, np.ndarray] = {}
    fits: list[dict[str, Any]] = []
    for fr in frames:
        name, idx = fr["name"], fr["index"]
        sel = obs_f == idx
        uv, z = obs_uv[sel].astype(np.float64), obs_z[sel].astype(np.float64)
        rec: dict[str, Any] = {"name": name, "sparse_points": int(sel.sum())}
        if backend == "sfm":
            d = _sfm_interp(uv, z, h, w, edge_px)
            rec["mode"] = "delaunay"
        elif backend == "mapanything":
            d = np.load(work / "poses" / "ma_depth" / (name + ".npy")) * scale_factor
            rec["mode"] = "scaled"
        else:
            if backend == "gt":
                key = name[:-4]
                if key not in gt:
                    raise WorldExtractError(f"ground-truth depth file has no entry '{key}'")
                d = gt[key].astype(np.float32)
                rec["mode"] = "scale"
            else:
                assert predict is not None
                d = predict(_load_image(image_dir / name)).astype(np.float32)
                rec["mode"] = "disparity-affine" if backend == "da2-small" else "scale"
            if rec["mode"] == "scale":
                rec["scale"] = _fit_scale(_sample(d, uv), z)
            else:
                rec["a"], rec["b"] = _fit_affine_disparity(_sample(d, uv), z)
        raw[name] = d
        fits.append(rec)

    # Temporal smoothing of the fitted alignment (few sparse points in a frame -> noisy fit).
    keys = [k_ for k_ in ("scale", "a", "b") if k_ in fits[0]]
    for key in keys:
        vals = np.array([f[key] for f in fits], dtype=np.float64)
        good = np.isfinite(vals)
        if not good.any():
            raise WorldExtractError(f"depth alignment failed on every frame ({backend}); too few sparse points?")
        if backend == "gt":
            vals[:] = float(np.median(vals[good]))  # one metric source: a single global scale
        else:
            filled = np.where(good, vals, np.median(vals[good]))
            r = max(1, p.smooth_window // 2)
            vals = np.array([np.median(filled[max(0, i - r):i + r + 1]) for i in range(len(filled))])
        for f, v in zip(fits, vals):
            f[key + "_smoothed"] = float(v)

    rel_errs = []
    for fr, rec in zip(frames, fits):
        d = raw[fr["name"]]
        if "scale_smoothed" in rec:
            d = d * rec["scale_smoothed"]
        elif "a_smoothed" in rec:
            disp = rec["a_smoothed"] * d + rec["b_smoothed"]
            d = np.where(disp > 1e-6, 1.0 / np.maximum(disp, 1e-6), 0.0)
        d = np.where((d > 0) & (d < max_depth) & np.isfinite(d), d, 0.0).astype(np.float32)
        if hud is not None:
            d[hud == 0] = 0.0
        sel = obs_f == fr["index"]
        if sel.any():
            s_d = _sample(d, obs_uv[sel].astype(np.float64))
            ok = s_d > 0
            if ok.any():
                rel = float(np.median(np.abs(s_d[ok] - obs_z[sel][ok]) / obs_z[sel][ok]))
                rec["median_abs_rel"] = round(rel, 5)
                rel_errs.append(rel)
        rec["valid_ratio"] = round(float((d > 0).mean()), 4)
        np.save(out / (fr["name"] + ".npy"), d)
    summary = {
        "backend": backend,
        "frames": len(frames),
        "max_depth": max_depth,
        "median_abs_rel_vs_sparse": round(float(np.median(rel_errs)), 5) if rel_errs else None,
    }
    write_json(out / "depth.json", {**summary, "per_frame": fits})
    LOG.info("depth (%s): %d frames, median |rel| vs sparse %.4f", backend, len(frames),
             summary["median_abs_rel_vs_sparse"] or float("nan"))
    return summary
