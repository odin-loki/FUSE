#!/usr/bin/env python3
"""ctest fuse_world_extract_synthetic: prove the extractor works without the world model (CPU only).

1. generate.py --dry-run writes a reconstruction-friendly `walkthrough` action log.
2. synthetic_scene.py ray-casts a known procedural scene along it (MP4 + fake HUD + ground truth).
3. extract.py all --cpu: COLMAP SfM + ground-truth depth (aligned to the SfM points like a monocular
   network's output would be), TSDF, collision, export.
4. A second `all` run must be fully cached (resumability).
5. The SfM-only depth path (no neural network) reuses the cached frames + poses stages.
6. Metrics vs ground truth, asserted against thresholds: pose ATE after Sim(3), Chamfer distance
   (accuracy / exact point-to-mesh completeness / F-score@10 cm), ground-plane normal error, scale
   error of the eye-height normalisation, focal error, action-gain calibration error. Plus format
   checks: glTF, .fuselevel round-trip, action-log loop-closure drift correction (injected drift), manifest hashes, POCO headers, HUD detection, and (when
   --fuse-cook is given) cooking world.glb / collision.glb to .fusemesh with the real FUSE cooker.

Exit 0 = pass, 1 = fail, 77 = SKIP (python deps missing).
"""

from __future__ import annotations

import argparse
import importlib
import json
import math
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent
SKIP = 77
REQUIRED = ["numpy", "cv2", "scipy", "open3d", "pycolmap"]
GAINS = {"deg": 30.0, "mpf": 0.08}

# Upper bounds. Pose/scale/gain metrics are shared by both depth paths (same poses stage).
THRESHOLDS_GT = {
    "ate_rmse_m": 0.10, "ate_rel_path": 0.01, "rot_err_mean_deg": 1.0, "scale_err": 0.05, "focal_err": 0.03,
    "gain_deg_err": 0.05, "gain_speed_err": 0.08, "ground_normal_err_deg": 2.0,
    "chamfer_mean_m": 0.10, "accuracy_m": 0.08, "completeness_m": 0.10, "one_minus_fscore": 0.15,
}
# The SfM-only path densifies sparse points by image-space interpolation: holes where texture is sparse.
THRESHOLDS_SFM = {"chamfer_mean_m": 0.25, "accuracy_m": 0.15, "completeness_m": 0.35, "one_minus_fscore": 0.45,
                  "ground_normal_err_deg": 3.0}


def run(cmd: list[str], log: Path, cwd: Path = TOOL) -> float:
    t0 = time.time()
    with open(log, "a", encoding="utf-8") as fh:
        fh.write("\n$ " + " ".join(cmd) + "\n")
        fh.flush()
        res = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, cwd=cwd)
    if res.returncode != 0:
        print(log.read_text(encoding="utf-8")[-6000:])
        raise SystemExit(f"FAIL: command exited {res.returncode}: {' '.join(cmd)}")
    return time.time() - t0


def evaluate(wx: Path, syn: Path) -> dict[str, Any]:
    import numpy as np
    import open3d as o3d
    import open3d.core as o3c
    from scipy.spatial import cKDTree

    from worldextract.geometry import angle_deg, backproject, umeyama

    gt_poses = np.load(syn / "gt" / "poses.npy")
    gt_depth = np.load(syn / "gt" / "depth.npz")
    gt_k = json.loads((syn / "gt" / "intrinsics.json").read_text())
    pj = json.loads((wx / "poses" / "poses.json").read_text())
    est = np.array([np.asarray(f["cam2world"]).reshape(4, 4) for f in pj["frames"]])
    src = [f["source_index"] for f in pj["frames"]]
    gt = gt_poses[src]
    s, r, t = umeyama(est[:, :3, 3], gt[:, :3, 3], with_scale=True)
    aligned = (s * (r @ est[:, :3, 3].T)).T + t
    ate = float(np.sqrt(((aligned - gt[:, :3, 3]) ** 2).sum(1).mean()))
    path_len = float(np.linalg.norm(np.diff(gt_poses[:, :3, 3], axis=0), axis=1).sum())
    rot_err = []
    for e, g in zip(est, gt):
        rr = (r @ e[:3, :3]).T @ g[:3, :3]
        rot_err.append(math.degrees(math.acos(max(-1.0, min(1.0, (np.trace(rr) - 1) / 2)))))

    mesh = np.load(wx / "fuse" / "mesh.npz")
    v, f = mesh["positions"].astype(np.float64), mesh["faces"].astype(np.int64)
    v_gt = (s * (r @ v.T)).T + t
    area = 0.5 * np.linalg.norm(np.cross(v_gt[f[:, 1]] - v_gt[f[:, 0]], v_gt[f[:, 2]] - v_gt[f[:, 0]]), axis=1)
    rng = np.random.default_rng(0)
    n_s = 60000
    tri = rng.choice(len(f), n_s, p=area / area.sum())
    bc = rng.random((n_s, 2))
    flip = bc.sum(1) > 1
    bc[flip] = 1 - bc[flip]
    e1, e2 = v_gt[f[tri, 1]] - v_gt[f[tri, 0]], v_gt[f[tri, 2]] - v_gt[f[tri, 0]]
    samples = v_gt[f[tri, 0]] + bc[:, :1] * e1 + bc[:, 1:] * e2
    # Ground-truth visible surface inside the reconstruction's working range (fuse depth truncation).
    max_depth_gt = json.loads((wx / "fuse" / "fuse.json").read_text())["depth_trunc_m"] * s
    k = np.array([[gt_k["fx"], 0, gt_k["cx"]], [0, gt_k["fy"], gt_k["cy"]], [0, 0, 1.0]])
    clouds = []
    for i in src:
        d = gt_depth[f"f{i:06d}"].astype(np.float32)
        d[d > max_depth_gt] = 0
        clouds.append(backproject(d, k, gt_poses[i], stride=4))
    gt_cloud = np.concatenate(clouds)
    gt_cloud = gt_cloud[np.unique(np.floor(gt_cloud / 0.03).astype(np.int64), axis=0, return_index=True)[1]]
    acc_d = cKDTree(gt_cloud).query(samples, k=1, workers=-1)[0]
    scene = o3d.t.geometry.RaycastingScene()
    scene.add_triangles(o3c.Tensor(v_gt.astype(np.float32)), o3c.Tensor(f.astype(np.uint32)))
    comp_d = scene.compute_distance(o3c.Tensor(gt_cloud.astype(np.float32))).numpy()  # exact point-to-mesh
    tau = 0.10
    precision, recall = float((acc_d < tau).mean()), float((comp_d < tau).mean())
    fscore = 2 * precision * recall / max(precision + recall, 1e-9)

    col = json.loads((wx / "collision" / "collision.json").read_text())
    ground_err = angle_deg(r @ np.asarray(col["ground_plane"]["normal"]), np.array([0.0, 0.0, 1.0]))
    cal = pj["calibration"]
    return {
        "keyframes": len(pj["frames"]),
        "registered": pj["stats"]["registered"],
        "ate_rmse_m": ate,
        "ate_rel_path": ate / path_len,
        "rot_err_mean_deg": float(np.mean(rot_err)),
        "sim3_scale_est_to_gt": s,
        "scale_err": abs(s - 1.0),
        "focal_err": abs(pj["intrinsics"]["fx"] - gt_k["fx"]) / gt_k["fx"],
        "gain_deg_err": abs(cal["deg_per_mouse_unit"] - GAINS["deg"]) / GAINS["deg"],
        "gain_speed_err": abs(cal["m_per_frame"] - GAINS["mpf"]) / GAINS["mpf"],
        "accuracy_m": float(acc_d.mean()),
        "completeness_m": float(comp_d.mean()),
        "chamfer_mean_m": 0.5 * float(acc_d.mean() + comp_d.mean()),
        "precision_10cm": precision,
        "recall_10cm": recall,
        "fscore_10cm": fscore,
        "one_minus_fscore": 1.0 - fscore,
        "ground_normal_err_deg": ground_err,
        "walkable_ratio": col["walkable"]["ratio"],
        "mesh_triangles": int(len(f)),
        "gt_path_length_m": path_len,
        "loop_closure": pj["loop_closure"],
    }


def loop_closure_selftest(actions: Path) -> dict[str, float]:
    """Inject 0.8 m + 5 deg of linear drift into exact prior poses; the action-log closure must remove it."""
    import numpy as np

    from worldextract import actions as act
    from worldextract.stages.poses import _calibrate, _loop_closure

    log = json.loads(actions.read_text())
    kb, mouse, labels, gains = act.load_action_log(log)
    truth_all = act.integrate(kb, mouse, labels, gains, 1.6)
    src = list(range(0, len(truth_all), 3))
    src[-1] = len(truth_all) - 1
    truth = truth_all[src]
    drifted = truth.copy()
    n = len(src)
    for i in range(n):
        a = i / (n - 1)
        yaw = math.radians(5.0) * a
        rz = np.array([[math.cos(yaw), -math.sin(yaw), 0], [math.sin(yaw), math.cos(yaw), 0], [0, 0, 1]])
        drifted[i, :3, :3] = rz @ truth[i, :3, :3]
        drifted[i, :3, 3] = rz @ (truth[i, :3, 3] - truth[0, :3, 3]) + truth[0, :3, 3] + a * np.array([0.8, 0.6, 0])
    valid = np.ones(n, bool)
    fixed, info = _loop_closure(drifted, src, valid, log, _calibrate(drifted, src, valid, log), "auto", 1.6)
    before = float(np.linalg.norm(drifted[:, :3, 3] - truth[:, :3, 3], axis=1).mean())
    after = float(np.linalg.norm(fixed[:, :3, 3] - truth[:, :3, 3], axis=1).mean())
    return {"applied": float(info["applied"]), "drift_err_before_m": before, "drift_err_after_m": after,
            "drift_residual_ratio": after / before}


def check_formats(wx: Path, n_vertices: int) -> list[str]:
    from worldextract.stages.export import check_outputs, read_fuselevel

    failures = []
    exp = wx / "export"
    check_outputs(exp)
    fl = read_fuselevel(exp / "world.fuselevel")
    wire = [e for e in fl["entities"] if e.startswith("__fuse.wire|material|WorldMesh|")]
    if "PlayerSpawn" not in fl["entities"] or not wire:
        failures.append("fuselevel entities missing")
    if (exp / "world.glb").read_bytes()[:4] != b"glTF":
        failures.append("world.glb bad magic")
    try:
        import pygltflib

        g = pygltflib.GLTF2().load(str(exp / "world.glb"))
        prim = g.meshes[0].primitives[0]
        if g.accessors[prim.attributes.POSITION].count != n_vertices:
            failures.append("glb vertex count mismatch")
        if prim.attributes.TEXCOORD_0 is None or prim.attributes.COLOR_0 is None:
            failures.append("glb lacks TEXCOORD_0/COLOR_0")
    except ImportError:
        pass
    man = json.loads((exp / "worldextract.manifest.json").read_text())
    if not man["provenance"]["ai"]["action_log_sha256"] or man["units"]["length"] != "m":
        failures.append("manifest provenance/units incomplete")
    if len(list((exp / "poco").rglob("*.poco.json"))) < 4:
        failures.append("POCO headers missing")
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", type=Path, help="scratch directory (default: a temp dir); only metrics.json + "
                                              "test.log are kept on success")
    ap.add_argument("--frames", type=int, default=241)
    ap.add_argument("--keep", action="store_true", help="keep the temp dir on success")
    ap.add_argument("--fuse-cook", type=Path, help="fuse_cook binary: also cook world.glb/collision.glb to .fusemesh")
    args = ap.parse_args()

    missing = []
    for mod in REQUIRED:
        try:
            importlib.import_module(mod)
        except ImportError:
            missing.append(mod)
    if missing:
        print(f"SKIP: python modules missing: {', '.join(missing)} "
              f"(pip install -r {TOOL / 'requirements.txt'}; configure with -DFUSE_WORLDEXTRACT_PYTHON=<venv python>)")
        return SKIP

    import cv2
    import numpy as np

    sys.path.insert(0, str(TOOL))
    tmp = None
    if args.work:
        work = args.work.resolve()
        if work.exists():
            shutil.rmtree(work)
    else:
        tmp = Path(tempfile.mkdtemp(prefix="fuse_wx_"))
        work = tmp
    work.mkdir(parents=True, exist_ok=True)
    log = work / "test.log"
    py = sys.executable
    t_start = time.time()

    seed_png = work / "seed.png"
    cv2.imwrite(str(seed_png), (np.random.default_rng(0).random((352, 640, 3)) * 255).astype(np.uint8))
    run([py, "generate.py", "--seed-image", str(seed_png), "--preset", "walkthrough", "--frames", str(args.frames),
         "--speed", "1.0", "--max-turn-deg", "3.0", "--m-per-frame", str(GAINS["mpf"]),
         "--deg-per-mouse-unit", str(GAINS["deg"]), "--out", str(work / "gen"), "--name", "base", "--dry-run"], log)
    syn = work / "syn"
    t_render = run([py, "tests/synthetic_scene.py", "--actions", str(work / "gen" / "base.actions.json"),
                    "--out", str(syn)], log)
    wx = work / "wx"
    common = ["--cpu", "--target-tris", "20000", "--atlas-size", "1024", "--seed", "3"]
    extract_cmd = [py, "extract.py", "all", "--work", str(wx), "--video", str(syn / "synthetic.mp4"),
                   "--actions", str(syn / "synthetic.actions.json"), "--gen-manifest", str(work / "gen" / "base.gen.json"),
                   "--name", "synthetic", "--depth-backend", "gt", "--gt-depth", str(syn / "gt" / "depth.npz"), *common]
    t_extract = run(extract_cmd, log)
    before = log.read_text(encoding="utf-8").count("cached (params")
    t_resume = run(extract_cmd, log)
    cached = log.read_text(encoding="utf-8").count("cached (params") - before

    # SfM-only depth path, reusing the cached frames + poses stages.
    wx_sfm = work / "wx_sfm"
    wx_sfm.mkdir()
    for item in ("frames", "poses"):
        shutil.copytree(wx / item, wx_sfm / item)
    shutil.copy2(wx / "inputs.json", wx_sfm / "inputs.json")
    t_sfm = run([py, "extract.py", "all", "--work", str(wx_sfm), "--depth-backend", "sfm", "--texture", "vertex",
                 *common], log)

    m_gt = evaluate(wx, syn)
    m_sfm = evaluate(wx_sfm, syn)
    m_loop = loop_closure_selftest(syn / "synthetic.actions.json")
    frames_info = json.loads((wx / "frames" / "frames.json").read_text())
    failures = [f"[gt depth] {k} = {m_gt[k]:.4f} > {v}" for k, v in THRESHOLDS_GT.items() if m_gt[k] > v]
    failures += [f"[sfm depth] {k} = {m_sfm[k]:.4f} > {v}" for k, v in THRESHOLDS_SFM.items() if m_sfm[k] > v]
    failures += check_formats(wx, int(np.load(wx / "fuse" / "mesh.npz")["positions"].shape[0]))
    if not frames_info["hud_boxes"]:
        failures.append("static HUD overlay was not detected")
    if not m_loop["applied"] or m_loop["drift_residual_ratio"] > 0.3:
        failures.append(f"loop-closure self-test: residual ratio {m_loop['drift_residual_ratio']:.3f} > 0.3")
    if args.fuse_cook:
        for asset in ("world", "collision"):
            run([str(args.fuse_cook), "--mesh", "--input", str(wx / "export" / f"{asset}.glb"),
                 "--output", str(work / f"{asset}.fusemesh")], log, cwd=work)  # its .fuse/ cache lands in work
            if not (work / f"{asset}.fusemesh").stat().st_size:
                failures.append(f"fuse_cook produced an empty {asset}.fusemesh")
    if cached != 6:
        failures.append(f"resume run recomputed stages ({cached}/6 cached)")

    report = {"gt_depth": m_gt, "sfm_depth": m_sfm, "loop_closure_selftest": m_loop, "hud_boxes": frames_info["hud_boxes"],
              "resume_cached_stages": cached,
              "seconds": {"render": round(t_render, 1), "extract_all": round(t_extract, 1),
                          "resume": round(t_resume, 1), "sfm_depth_branch": round(t_sfm, 1),
                          "total": round(time.time() - t_start, 1)}}
    (work / "metrics.json").write_text(json.dumps(report, indent=2, default=float), encoding="utf-8")
    print(json.dumps(report, indent=2, default=float))
    if failures:
        print("FAIL:\n  " + "\n  ".join(failures))
        print(f"work dir kept: {work}")
        return 1
    print("PASS: fuse_world_extract_synthetic")
    if not args.keep:  # keep only the report; the video, frames and meshes are ~30 MB
        if tmp is not None:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            for child in work.iterdir():
                if child.name not in ("metrics.json", "test.log"):
                    shutil.rmtree(child) if child.is_dir() else child.unlink()
    return 0


if __name__ == "__main__":
    sys.exit(main())
