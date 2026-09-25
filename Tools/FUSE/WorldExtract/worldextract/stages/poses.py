"""Stage `poses`: camera intrinsics + extrinsics, normalised to a +Z-up metric-ish world.

Backends:
    colmap       COLMAP 4.x via pycolmap (BSD-3): SIFT, sequential matching, action-prior loop-closure
                 pairs, incremental mapping or GLOMAP-style global mapping (`--mapper global`).
    mapanything  MapAnything (Apache-2.0 code, `facebook/map-anything-apache` Apache-2.0 weights):
                 feed-forward multi-view poses + intrinsics + dense depth. GPU only.
    auto         mapanything when importable with CUDA, else colmap.

The action log (optional) is used to
    1. propose loop-closure image pairs (frames whose prior poses are close but far apart in time),
    2. fill keyframes SfM could not register (prior motion anchored to registered neighbours),
    3. estimate gravity (the model's camera never rolls; prior pitch gives the up vector per frame),
    4. correct residual loop-closure drift (observed start/end gap minus the gap the actions predict),
    5. calibrate the action gains (degrees per mouse unit, metres per key frame) for `generate.py`.

Scale is arbitrary in monocular video. Default: the median camera height above the RANSAC ground
plane is set to `--eye-height` metres (1.7 m, a standing first-person camera).

Outputs (in <work>/poses/): poses.json, points.npz, sfm/ (COLMAP database + model).
"""

from __future__ import annotations

import logging
import math
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from .. import actions as act
from ..common import WorldExtractError, read_json, write_json
from ..geometry import (
    apply_sim3_to_poses,
    fit_plane_ransac,
    invert_pose,
    pose_anchor_points,
    rotation_between,
    umeyama,
    yaw_of,
)

LOG = logging.getLogger("worldextract.poses")


@dataclass
class PosesParams:
    backend: str = "auto"
    mapper: str = "incremental"  # incremental | global
    camera_model: str = "SIMPLE_PINHOLE"
    hfov_deg: float = 0.0  # 0 = action log camera.hfov_deg, else 80 (COLMAP refines it)
    refine_focal: bool = True
    seq_overlap: int = 8
    loop_pairs: bool = True
    max_features: int = 4096
    scale_mode: str = "eye-height"  # eye-height | action-speed | none
    eye_height: float = 0.0  # 0 = action log camera.eye_height_m, else 1.7
    loop_closure: str = "auto"  # auto | off | force
    mapanything_model: str = "facebook/map-anything-apache"
    seed: int = 0
    threads: int = 0  # 0 = all cores
    gpu: bool = False


@dataclass
class RawReconstruction:
    names: list[str]
    cam2world: dict[str, np.ndarray]  # registered keyframes only, arbitrary frame
    k: np.ndarray
    width: int
    height: int
    points: np.ndarray  # (M, 3)
    colors: np.ndarray  # (M, 3) uint8
    obs: dict[str, tuple[np.ndarray, np.ndarray]]  # name -> (uv (n,2), point index (n,))
    source: str
    image_dir: Path
    stats: dict[str, Any]


# --------------------------------------------------------------------------------------------- #
# COLMAP backend
# --------------------------------------------------------------------------------------------- #


def _loop_pairs(names: list[str], prior: np.ndarray, overlap: int) -> list[tuple[str, str]]:
    """Image pairs the action prior says revisit the same place (candidate loop closures)."""
    c = prior[:, :3, 3]
    steps = np.linalg.norm(np.diff(c, axis=0), axis=1)
    if steps.size == 0 or float(np.median(steps)) <= 0:
        return []
    extent = float(np.linalg.norm(c.max(0) - c.min(0)))
    radius = max(3.0 * float(np.median(steps)), 0.12 * extent)
    heading = np.array([yaw_of(p) for p in prior])
    pairs = []
    for i in range(len(names)):
        cands = []
        for j in range(i + overlap + 1, len(names)):
            d = float(np.linalg.norm(c[i] - c[j]))
            dh = abs((heading[i] - heading[j] + math.pi) % (2 * math.pi) - math.pi)
            if d < radius and dh < math.radians(50):
                cands.append((d, j))
        for _, j in sorted(cands)[:3]:
            pairs.append((names[i], names[j]))
    return pairs


def run_colmap(frames_dir: Path, names: list[str], has_masks: bool, out: Path, p: PosesParams,
               width: int, height: int, hfov_deg: float, prior: np.ndarray | None) -> RawReconstruction:
    import pycolmap

    sfm = out / "sfm"
    if sfm.exists():
        shutil.rmtree(sfm)
    sfm.mkdir(parents=True)
    db = sfm / "database.db"
    image_dir = frames_dir / "key"
    pycolmap.set_random_seed(p.seed)
    if not LOG.isEnabledFor(logging.DEBUG):
        pycolmap.logging.minloglevel = 1  # COLMAP INFO spam only with --log-level DEBUG
    device = pycolmap.Device.cuda if (p.gpu and pycolmap.has_cuda) else pycolmap.Device.cpu

    f = 0.5 * width / math.tan(math.radians(hfov_deg) / 2)
    reader = pycolmap.ImageReaderOptions()
    reader.camera_model = p.camera_model
    if p.camera_model in ("SIMPLE_PINHOLE", "SIMPLE_RADIAL"):
        params = [f, width / 2, height / 2] + ([0.0] if p.camera_model == "SIMPLE_RADIAL" else [])
    elif p.camera_model == "PINHOLE":
        params = [f, f, width / 2, height / 2]
    else:
        raise WorldExtractError(f"unsupported --camera-model {p.camera_model}")
    reader.camera_params = ",".join(f"{v:.6f}" for v in params)
    if has_masks:
        reader.mask_path = str(frames_dir / "masks")
    ext = pycolmap.FeatureExtractionOptions()
    ext.sift.max_num_features = p.max_features
    ext.num_threads = p.threads or -1
    ext.use_gpu = device == pycolmap.Device.cuda
    LOG.info("SIFT extraction on %d keyframes (%s)", len(names), "cuda" if ext.use_gpu else "cpu")
    pycolmap.extract_features(db, image_dir, image_names=names, camera_mode=pycolmap.CameraMode.SINGLE,
                              reader_options=reader, extraction_options=ext, device=device)

    match = pycolmap.FeatureMatchingOptions()
    match.num_threads = p.threads or -1
    match.use_gpu = device == pycolmap.Device.cuda
    verify = pycolmap.TwoViewGeometryOptions()
    verify.filter_stationary_matches = True  # drops matches on any residual static overlay
    seq = pycolmap.SequentialPairingOptions()
    seq.overlap = p.seq_overlap
    seq.quadratic_overlap = True
    seq.loop_detection = False  # vocab-tree loop detection needs a downloaded tree; we use the prior
    pycolmap.match_sequential(db, matching_options=match, pairing_options=seq, verification_options=verify,
                              device=device)
    n_loop = 0
    if p.loop_pairs and prior is not None:
        pairs = _loop_pairs(names, prior, p.seq_overlap * 2)
        if pairs:
            pair_file = sfm / "loop_pairs.txt"
            pair_file.write_text("".join(f"{a} {b}\n" for a, b in pairs), encoding="utf-8")
            imp = pycolmap.ImportedPairingOptions()
            imp.match_list_path = str(pair_file)
            pycolmap.match_image_pairs(db, matching_options=match, pairing_options=imp,
                                       verification_options=verify, device=device)
            n_loop = len(pairs)
    LOG.info("matched sequentially (overlap %d) + %d action-prior loop pairs", p.seq_overlap, n_loop)

    model_dir = sfm / "model"
    model_dir.mkdir()
    if p.mapper == "global":
        gopt = pycolmap.GlobalPipelineOptions()
        gopt.random_seed = p.seed
        gopt.num_threads = p.threads or -1
        recs = pycolmap.global_mapping(db, image_dir, model_dir, options=gopt)
    elif p.mapper == "incremental":
        iopt = pycolmap.IncrementalPipelineOptions()
        iopt.random_seed = p.seed
        iopt.num_threads = p.threads or -1
        iopt.ba_refine_focal_length = p.refine_focal
        iopt.ba_refine_principal_point = False
        iopt.min_model_size = 3
        recs = pycolmap.incremental_mapping(db, image_dir, model_dir, options=iopt)
    else:
        raise WorldExtractError(f"unknown --mapper {p.mapper}")
    if not recs:
        raise WorldExtractError(
            "COLMAP could not reconstruct any model. Typical causes: too little parallax (pure rotation), "
            "textureless frames, or heavy generation artefacts. Try the walkthrough/strafe_scan presets, "
            "`--hud` masking, or `--pose-backend mapanything` on a GPU."
        )
    rec = max(recs.values(), key=lambda r: r.num_reg_images())
    rec.write(model_dir)
    LOG.info("SfM: %d models, largest registers %d/%d images, %d points, reproj %.3f px",
             len(recs), rec.num_reg_images(), len(names), rec.num_points3D(),
             rec.compute_mean_reprojection_error())

    cams = list(rec.cameras.values())
    k = np.asarray(cams[0].calibration_matrix(), dtype=np.float64)
    pid_index: dict[int, int] = {}
    pts: list[np.ndarray] = []
    cols: list[np.ndarray] = []
    for pid, pt in rec.points3D.items():
        pid_index[pid] = len(pts)
        pts.append(np.asarray(pt.xyz))
        cols.append(np.asarray(pt.color))
    cam2world: dict[str, np.ndarray] = {}
    obs: dict[str, tuple[np.ndarray, np.ndarray]] = {}
    for img in rec.images.values():
        if not img.has_pose:
            continue
        w2c = np.eye(4)
        w2c[:3, :] = np.asarray(img.cam_from_world().matrix())
        cam2world[img.name] = invert_pose(w2c)
        uv, idx = [], []
        for p2d in img.points2D:
            if p2d.has_point3D():
                uv.append(np.asarray(p2d.xy))
                idx.append(pid_index[p2d.point3D_id])
        obs[img.name] = (np.asarray(uv, dtype=np.float64).reshape(-1, 2), np.asarray(idx, dtype=np.int64))
    return RawReconstruction(
        names=names, cam2world=cam2world, k=k, width=width, height=height,
        points=np.asarray(pts).reshape(-1, 3), colors=np.asarray(cols, dtype=np.uint8).reshape(-1, 3),
        obs=obs, source="sfm", image_dir=image_dir,
        stats={"models": len(recs), "registered": rec.num_reg_images(), "points3D": rec.num_points3D(),
               "mean_reproj_px": round(float(rec.compute_mean_reprojection_error()), 4),
               "loop_pairs": n_loop, "mapper": p.mapper, "focal_px": round(float(k[0, 0]), 3)},
    )


# --------------------------------------------------------------------------------------------- #
# MapAnything backend (GPU; unverified in this repository's CI)
# --------------------------------------------------------------------------------------------- #


def run_mapanything(frames_dir: Path, names: list[str], out: Path, p: PosesParams) -> RawReconstruction:
    """Feed-forward multi-view reconstruction. API per facebookresearch/map-anything README (2026-09-23)."""
    import cv2
    import torch
    from mapanything.models import MapAnything  # type: ignore[import-not-found]
    from mapanything.utils.image import load_images  # type: ignore[import-not-found]

    if not torch.cuda.is_available():
        raise WorldExtractError("mapanything backend needs a CUDA GPU; use --pose-backend colmap")
    model = MapAnything.from_pretrained(p.mapanything_model).to("cuda")
    model.eval()
    views = load_images([str(frames_dir / "key" / n) for n in names])
    with torch.no_grad():
        preds = model.infer(views, memory_efficient_inference=True, use_amp=True, amp_dtype="bf16",
                            apply_mask=True, mask_edges=True, apply_confidence_mask=False)
    img_dir = out / "ma_images"
    depth_dir = out / "ma_depth"
    for d in (img_dir, depth_dir):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
    cam2world: dict[str, np.ndarray] = {}
    obs: dict[str, tuple[np.ndarray, np.ndarray]] = {}
    pts_all: list[np.ndarray] = []
    col_all: list[np.ndarray] = []
    rng = np.random.default_rng(p.seed)
    k_list: list[np.ndarray] = []
    h = w = 0
    for name, pred in zip(names, preds):
        img = (pred["img_no_norm"][0].float().cpu().numpy() * (255.0 if pred["img_no_norm"].max() <= 1.0 else 1.0))
        img = np.clip(img, 0, 255).astype(np.uint8)
        h, w = img.shape[:2]
        cv2.imwrite(str(img_dir / name), cv2.cvtColor(img, cv2.COLOR_RGB2BGR))
        depth = pred["depth_z"][0, ..., 0].float().cpu().numpy()
        mask = pred["mask"][0, ..., 0].cpu().numpy().astype(bool)
        depth[~mask] = 0.0
        np.save(depth_dir / (name + ".npy"), depth.astype(np.float32))
        cam2world[name] = pred["camera_poses"][0].float().cpu().numpy().astype(np.float64)
        k_list.append(pred["intrinsics"][0].float().cpu().numpy())
        pts = pred["pts3d"][0].float().cpu().numpy()[mask]
        cols = img[mask]
        sel = rng.choice(len(pts), size=min(len(pts), 3000), replace=False) if len(pts) else np.zeros(0, int)
        base = int(sum(int(x.shape[0]) for x in pts_all))
        pts_all.append(pts[sel])
        col_all.append(cols[sel])
        vs, us = np.nonzero(mask)
        obs[name] = (np.stack([us[sel] + 0.5, vs[sel] + 0.5], 1).astype(np.float64), base + np.arange(len(sel)))
    k = np.median(np.stack(k_list), axis=0).astype(np.float64)
    # MapAnything intrinsics use pixel-index centres; WorldExtract uses COLMAP's (+0.5) convention.
    k[0, 2] += 0.5
    k[1, 2] += 0.5
    return RawReconstruction(
        names=names, cam2world=cam2world, k=k, width=w, height=h,
        points=np.concatenate(pts_all), colors=np.concatenate(col_all).astype(np.uint8), obs=obs,
        source="mapanything", image_dir=img_dir,
        stats={"model": p.mapanything_model, "registered": len(cam2world), "points3D": int(sum(len(x) for x in pts_all))},
    )


# --------------------------------------------------------------------------------------------- #
# Normalisation, prior fusion, calibration
# --------------------------------------------------------------------------------------------- #


def _fill_from_prior(raw: RawReconstruction, prior: np.ndarray | None) -> tuple[np.ndarray, list[str]]:
    names = raw.names
    reg = [i for i, n in enumerate(names) if n in raw.cam2world]
    if len(reg) < 2:
        raise WorldExtractError(f"only {len(reg)} keyframes registered; cannot build a trajectory")
    poses = np.zeros((len(names), 4, 4))
    source = ["missing"] * len(names)
    for i in reg:
        poses[i] = raw.cam2world[names[i]]
        source[i] = raw.source
    missing = [i for i in range(len(names)) if i not in reg]
    if not missing:
        return poses, source
    if prior is None:
        LOG.warning("%d keyframes unregistered and no action log: dropping them", len(missing))
        return poses, source
    reach = float(np.median(np.linalg.norm(np.diff(prior[:, :3, 3], axis=0), axis=1))) or 1.0
    s, r, t = umeyama(pose_anchor_points(prior[reg], reach), pose_anchor_points(poses[reg], 1.0), True)
    for i in missing:
        j = min(reg, key=lambda x: abs(x - i))  # nearest registered neighbour in time
        rel = invert_pose(prior[j]) @ prior[i]
        rel[:3, 3] *= s
        poses[i] = poses[j] @ rel
        source[i] = "action-prior"
    LOG.info("filled %d unregistered keyframes from the action prior (Sim3 scale %.4f)", len(missing), s)
    return poses, source


def _estimate_up(poses: np.ndarray, valid: np.ndarray, prior: np.ndarray | None) -> np.ndarray:
    if prior is not None:
        # World-model cameras do not roll; the prior knows each frame's pitch.
        ups = [poses[i, :3, :3] @ prior[i, :3, :3].T @ np.array([0.0, 0.0, 1.0]) for i in np.nonzero(valid)[0]]
        up = np.mean(ups, axis=0)
    else:
        # Without roll the camera x axes are all horizontal: up is their common normal.
        _, _, vt = np.linalg.svd(poses[valid, :3, 0])
        up = vt[2]
        if up @ (-poses[valid, :3, 1].mean(0)) < 0:
            up = -up
    return up / np.linalg.norm(up)


def _ground_plane(points: np.ndarray, poses: np.ndarray, valid: np.ndarray, up: np.ndarray,
                  seed: int) -> tuple[np.ndarray, float, int] | None:
    if points.shape[0] < 30:
        return None
    cam_h = poses[valid, :3, 3] @ up
    below = points[(points @ up) < float(np.min(cam_h))]
    if below.shape[0] < 30:
        return None
    lo, hi = np.percentile(points, 5, axis=0), np.percentile(points, 95, axis=0)
    thr = 0.006 * float(np.linalg.norm(hi - lo))
    try:
        n, d, inl = fit_plane_ransac(below, thr, np.random.default_rng(seed), 600, up, 25.0)
    except ValueError:
        return None
    if inl.sum() < max(30, 0.05 * below.shape[0]):
        return None
    return n, d, int(inl.sum())


def _calibrate(poses: np.ndarray, src_idx: list[int], valid: np.ndarray, log: dict[str, Any]) -> dict[str, Any]:
    kb, mouse, labels, _ = act.load_action_log(log)
    lab = {n: i for i, n in enumerate(labels)}
    yaw_x, yaw_y, fwd_x, fwd_y, st_x, st_y = [], [], [], [], [], []
    for a, b in zip(range(len(src_idx) - 1), range(1, len(src_idx))):
        if not (valid[a] and valid[b]):
            continue
        fa, fb = src_idx[a], src_idx[b]
        if fb > kb.shape[0]:
            break
        units = float(mouse[fa + 1:fb + 1, 1].sum())
        dyaw = (yaw_of(poses[b]) - yaw_of(poses[a]) + math.pi) % (2 * math.pi) - math.pi
        yaw_x.append(-units)
        yaw_y.append(math.degrees(dyaw))
        disp = poses[b, :3, 3] - poses[a, :3, 3]
        ya = yaw_of(poses[a])
        heading = np.array([math.cos(ya), math.sin(ya), 0.0])
        right = np.array([math.sin(ya), -math.cos(ya), 0.0])
        nf = float(kb[fa + 1:fb + 1, lab["forward"]].sum() - kb[fa + 1:fb + 1, lab["back"]].sum()) if "forward" in lab else 0.0
        ns = float(kb[fa + 1:fb + 1, lab["right"]].sum() - kb[fa + 1:fb + 1, lab["left"]].sum()) if "right" in lab else 0.0
        fwd_x.append(nf)
        fwd_y.append(float(disp @ heading))
        st_x.append(ns)
        st_y.append(float(disp @ right))

    def fit(x: list[float], y: list[float]) -> float | None:
        xa, ya_ = np.asarray(x), np.asarray(y)
        den = float(xa @ xa)
        return float(xa @ ya_ / den) if den > 1e-9 else None

    deg = fit(yaw_x, yaw_y)
    mpf = fit(fwd_x, fwd_y)
    msf = fit(st_x, st_y)
    return {
        "deg_per_mouse_unit": None if deg is None else round(deg, 4),
        "m_per_frame": None if mpf is None else round(mpf, 5),
        "strafe_ratio": None if (msf is None or not mpf) else round(msf / mpf, 4),
        "samples": len(yaw_x),
        "note": "fitted from normalised poses; m_per_frame is in the output units (see scale)",
    }


def _loop_closure(poses: np.ndarray, src_idx: list[int], valid: np.ndarray, log: dict[str, Any],
                  calib: dict[str, Any], mode: str, eye: float) -> tuple[np.ndarray, dict[str, Any]]:
    info: dict[str, Any] = {"mode": mode, "applied": False}
    if mode == "off" or not log.get("closes_loop", False):
        info["reason"] = "disabled" if mode == "off" else "trajectory does not close"
        return poses, info
    ends = [i for i in range(len(src_idx)) if valid[i]]
    first, last = ends[0], ends[-1]
    kb, mouse, labels, gains = act.load_action_log(log)
    if calib.get("deg_per_mouse_unit"):
        gains.deg_per_mouse_unit = float(calib["deg_per_mouse_unit"])
    if calib.get("m_per_frame"):
        gains.m_per_frame = float(calib["m_per_frame"])
    if calib.get("strafe_ratio"):
        gains.strafe_ratio = float(calib["strafe_ratio"])
    prior = act.integrate(kb, mouse, labels, gains, eye)
    exp_rel = invert_pose(prior[src_idx[first]]) @ prior[src_idx[last]]
    obs_rel = invert_pose(poses[first]) @ poses[last]
    gap_local = obs_rel[:3, 3] - exp_rel[:3, 3]
    gap = poses[first, :3, :3] @ gap_local
    gap[2] = 0.0  # never "correct" height: the ground plane already fixes it
    dyaw = (yaw_of(poses[last]) - yaw_of(poses[first])) - (yaw_of(prior[src_idx[last]]) - yaw_of(prior[src_idx[first]]))
    dyaw = (dyaw + math.pi) % (2 * math.pi) - math.pi
    c = poses[:, :3, 3]
    seg = np.r_[0.0, np.cumsum(np.linalg.norm(np.diff(c, axis=0), axis=1))]
    length = float(seg[last] - seg[first]) or 1.0
    info.update({"gap_m": round(float(np.linalg.norm(gap)), 4), "gap_yaw_deg": round(math.degrees(dyaw), 3),
                 "path_length": round(length, 4), "gap_ratio": round(float(np.linalg.norm(gap)) / length, 5)})
    ratio = float(np.linalg.norm(gap)) / length
    if mode == "auto" and ratio < 0.01 and abs(math.degrees(dyaw)) < 2.0:
        info["reason"] = "gap below tolerance (SfM closed the loop)"
        return poses, info
    if ratio > 0.25 and mode != "force":
        info["reason"] = "gap > 25% of path: action prior too unreliable to correct with"
        LOG.warning("loop closure gap %.1f%% of path; not correcting (use --loop-closure force)", 100 * ratio)
        return poses, info
    out = poses.copy()
    for i in range(len(poses)):
        a = min(1.0, max(0.0, (seg[i] - seg[first]) / length))
        yaw = -a * dyaw
        rz = np.array([[math.cos(yaw), -math.sin(yaw), 0], [math.sin(yaw), math.cos(yaw), 0], [0, 0, 1]])
        pivot = poses[first, :3, 3]
        out[i, :3, :3] = rz @ poses[i, :3, :3]
        out[i, :3, 3] = rz @ (poses[i, :3, 3] - pivot) + pivot - a * gap
    info["applied"] = True
    LOG.info("loop closure: distributed %.3f m / %.2f deg drift along the path", float(np.linalg.norm(gap)), math.degrees(dyaw))
    return out, info


def run(work: Path, out: Path, p: PosesParams, action_log: dict[str, Any] | None) -> dict[str, Any]:
    frames = read_json(work / "frames" / "frames.json")
    names = [k["name"] for k in frames["keyframes"]]
    src_idx = [int(k["source_index"]) for k in frames["keyframes"]]
    w, h = int(frames["width"]), int(frames["height"])
    cam_hint = (action_log or {}).get("camera") or {}
    hfov = p.hfov_deg or float(cam_hint.get("hfov_deg") or 80.0)
    eye = p.eye_height or float(cam_hint.get("eye_height_m") or 1.7)

    prior_kf = None
    if action_log is not None:
        kb, mouse, labels, gains = act.load_action_log(action_log)
        if kb.shape[0] < max(src_idx) + 1:
            raise WorldExtractError(
                f"action log has {kb.shape[0]} frames but the video needs {max(src_idx) + 1}; wrong log?")
        expected = int(action_log.get("video", {}).get("num_frames", kb.shape[0]))
        if kb.shape[0] != expected:
            LOG.warning("action log length %d != its declared video length %d", kb.shape[0], expected)
        prior_full = act.integrate(kb, mouse, labels, gains, eye)
        prior_kf = prior_full[src_idx]

    backend = p.backend
    if backend == "auto":
        from ..common import cuda_available, missing_modules

        backend = "mapanything" if (cuda_available() and not missing_modules(["mapanything"])) else "colmap"
        LOG.info("pose backend auto -> %s", backend)
    if backend == "colmap":
        raw = run_colmap(work / "frames", names, bool(frames.get("has_masks")), out, p, w, h, hfov, prior_kf)
    elif backend == "mapanything":
        raw = run_mapanything(work / "frames", names, out, p)
    else:
        raise WorldExtractError(f"unknown pose backend {backend}")

    poses, source = _fill_from_prior(raw, prior_kf)
    valid = np.array([s != "missing" for s in source])

    # 1. Gravity: action-prior / camera-roll estimate, refined by the ground plane.
    up = _estimate_up(poses, valid, prior_kf)
    gp = _ground_plane(raw.points, poses, valid, up, p.seed)
    ground_info: dict[str, Any] = {"found": gp is not None}
    if gp is not None:
        n, d, count = gp
        ground_info.update({"inliers": count, "tilt_vs_camera_up_deg": round(math.degrees(math.acos(min(1.0, float(n @ up)))), 3)})
        up = n
    else:
        LOG.warning("no ground plane found in sparse points; using camera-derived up vector")

    # 2. Rotate up -> +Z, first camera heading -> +X.
    r1 = rotation_between(up, np.array([0.0, 0.0, 1.0]))
    first = int(np.nonzero(valid)[0][0])
    fwd = r1 @ poses[first, :3, 2]
    yaw0 = math.atan2(fwd[1], fwd[0])
    rz = np.array([[math.cos(-yaw0), -math.sin(-yaw0), 0], [math.sin(-yaw0), math.cos(-yaw0), 0], [0, 0, 1]])
    r_align = rz @ r1

    # 3. Scale.
    cam_c = poses[valid, :3, 3]
    if gp is not None:
        heights = cam_c @ gp[0] + gp[1]
        ground_off = -gp[1]  # plane: n.x = -d
    else:
        heights = None
        ground_off = float(np.min(cam_c @ up)) - 1.0
    scale_info: dict[str, Any] = {"mode": p.scale_mode}
    if p.scale_mode == "eye-height" and heights is not None and float(np.median(heights)) > 0:
        s = eye / float(np.median(heights))
        scale_info.update({"eye_height_m": eye, "units": "metres (assumed eye height)"})
    elif p.scale_mode == "action-speed" and prior_kf is not None:
        sfm_len = float(np.linalg.norm(np.diff(cam_c, axis=0), axis=1).sum())
        pri_len = float(np.linalg.norm(np.diff(prior_kf[valid, :3, 3], axis=0), axis=1).sum())
        s = pri_len / sfm_len if sfm_len > 0 else 1.0
        scale_info.update({"units": "metres (assumed action gains)"})
    else:
        if p.scale_mode != "none":
            LOG.warning("scale mode %s unavailable (no ground plane / no action log); using unit scale", p.scale_mode)
        ext = float(np.linalg.norm(np.percentile(raw.points, 95, 0) - np.percentile(raw.points, 5, 0))) if len(raw.points) else 1.0
        s = 10.0 / ext if ext > 0 else 1.0
        scale_info.update({"mode": "none", "units": "arbitrary (scene 5-95% extent = 10 units)"})
    scale_info["factor"] = s

    # 4. Translate: ground -> z = 0, first camera above the origin.
    c0 = s * (r_align @ poses[first, :3, 3])
    t = np.array([-c0[0], -c0[1], -s * ground_off])
    poses_n = apply_sim3_to_poses(poses, s, r_align, t)
    points_n = (s * (r_align @ raw.points.T)).T + t if len(raw.points) else raw.points

    # Camera-frame depth of each observation (for depth alignment) before any drift correction.
    obs_frame, obs_uv, obs_depth, obs_pid = [], [], [], []
    for i, n in enumerate(names):
        if n not in raw.obs:
            continue
        uv, idx = raw.obs[n]
        if len(idx) == 0:
            continue
        w2c = invert_pose(poses_n[i])
        pc = (w2c[:3, :3] @ points_n[idx].T).T + w2c[:3, 3]
        ok = pc[:, 2] > 0
        obs_frame.append(np.full(int(ok.sum()), i))
        obs_uv.append(uv[ok])
        obs_depth.append(pc[ok, 2])
        obs_pid.append(idx[ok])

    calib: dict[str, Any] = {}
    loop_info: dict[str, Any] = {"applied": False, "reason": "no action log"}
    if action_log is not None:
        calib = _calibrate(poses_n, src_idx, valid, action_log)
        poses_n, loop_info = _loop_closure(poses_n, src_idx, valid, action_log, calib, p.loop_closure, eye)

    k = raw.k
    frames_out = []
    for i, n in enumerate(names):
        if not valid[i]:
            continue
        frames_out.append({"name": n, "index": i, "source_index": src_idx[i], "source": source[i],
                           "cam2world": [round(float(v), 8) for v in poses_n[i].reshape(-1)]})
    result = {
        "backend": backend,
        "image_dir": str(raw.image_dir),
        "intrinsics": {"width": raw.width, "height": raw.height, "fx": float(k[0, 0]), "fy": float(k[1, 1]),
                       "cx": float(k[0, 2]), "cy": float(k[1, 2]), "convention": "COLMAP (pixel centre +0.5)"},
        "world": {"up": "+Z", "handedness": "right", "camera": "OpenCV (+X right, +Y down, +Z forward)",
                  "origin": "ground below the first keyframe", "x_axis": "first keyframe heading"},
        "scale": scale_info,
        "ground": ground_info,
        "calibration": calib,
        "loop_closure": loop_info,
        "stats": raw.stats,
        "frames": frames_out,
    }
    write_json(out / "poses.json", result)
    np.savez_compressed(
        out / "points.npz", xyz=points_n.astype(np.float32), rgb=raw.colors,
        obs_frame=np.concatenate(obs_frame) if obs_frame else np.zeros(0, int),
        obs_uv=np.concatenate(obs_uv).astype(np.float32) if obs_uv else np.zeros((0, 2), np.float32),
        obs_depth=np.concatenate(obs_depth).astype(np.float32) if obs_depth else np.zeros(0, np.float32),
        obs_pid=np.concatenate(obs_pid) if obs_pid else np.zeros(0, int),
    )
    LOG.info("poses: %d/%d keyframes (%s), scale %.4f (%s)", len(frames_out), len(names), backend, s, scale_info["mode"])
    return {"backend": backend, "registered": int(valid.sum()), "keyframes": len(names),
            "scale": s, "ground_found": gp is not None, "loop_closure_applied": loop_info.get("applied", False)}


def load(work: Path) -> tuple[dict[str, Any], np.ndarray, np.ndarray]:
    """(poses.json, cam2world (N,4,4) in poses.json frame order, K 3x3 COLMAP convention)."""
    info = read_json(work / "poses" / "poses.json")
    poses = np.array([np.asarray(f["cam2world"]).reshape(4, 4) for f in info["frames"]])
    intr = info["intrinsics"]
    k = np.array([[intr["fx"], 0, intr["cx"]], [0, intr["fy"], intr["cy"]], [0, 0, 1.0]])
    return info, poses, k
