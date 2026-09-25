"""Stage `splat` (optional, CUDA): Gaussian-splat training with gsplat (Apache-2.0).

The original INRIA 3DGS code is non-commercial and is NOT used (REMASTER_PLAN §3.4). This is a
compact trainer over gsplat's public `rasterization` API and `DefaultStrategy` densification,
initialised from the fused mesh (vertex positions + colours) and the stage `poses` cameras.

Output: <work>/splat/splat.ply in the de-facto 3DGS PLY layout (x y z, f_dc_*, opacity, scale_*,
rot_*), +Z up, metres - loadable by common splat viewers; FUSE has no splat renderer yet.

UNVERIFIED here (no GPU in CI): written against gsplat >= 1.4 documentation.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from ..common import WorldExtractError, cuda_available, missing_modules, write_json
from ..geometry import invert_pose
from . import poses as poses_stage

LOG = logging.getLogger("worldextract.splat")
SH_C0 = 0.28209479177387814


@dataclass
class SplatParams:
    iterations: int = 7000
    max_init_points: int = 300_000
    lr_means: float = 1.6e-4


def _write_ply(path: Path, means: np.ndarray, rgb: np.ndarray, opac_logit: np.ndarray, log_scales: np.ndarray,
               quats: np.ndarray) -> None:
    n = means.shape[0]
    f_dc = (rgb - 0.5) / SH_C0
    fields = ["x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1", "scale_2",
              "rot_0", "rot_1", "rot_2", "rot_3"]
    data = np.concatenate([means, f_dc, opac_logit[:, None], log_scales, quats], 1).astype("<f4")
    header = "ply\nformat binary_little_endian 1.0\nelement vertex %d\n" % n
    header += "".join(f"property float {f}\n" for f in fields) + "end_header\n"
    with open(path, "wb") as fh:
        fh.write(header.encode("ascii"))
        fh.write(data.tobytes())


def run(work: Path, out: Path, p: SplatParams, seed: int) -> dict[str, Any]:
    if not cuda_available() or missing_modules(["gsplat"]):
        raise WorldExtractError("splat stage needs CUDA + gsplat (pip install -r requirements-gpu.txt); "
                                "it is optional - skip it on CPU-only machines")
    import cv2
    import torch
    from gsplat import rasterization  # type: ignore[import-not-found]
    from gsplat.strategy import DefaultStrategy  # type: ignore[import-not-found]

    torch.manual_seed(seed)
    dev = torch.device("cuda")
    info, poses, k = poses_stage.load(work)
    w, h = int(info["intrinsics"]["width"]), int(info["intrinsics"]["height"])
    image_dir = Path(info["image_dir"])
    imgs = []
    for fr in info["frames"]:
        bgr = cv2.imread(str(image_dir / fr["name"]), cv2.IMREAD_COLOR)
        if bgr is None:
            raise WorldExtractError(f"missing keyframe image {image_dir / fr['name']}")
        imgs.append(torch.tensor(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB) / 255.0, dtype=torch.float32))
    gt = torch.stack(imgs).to(dev)
    viewmats = torch.tensor(np.stack([invert_pose(c) for c in poses]), dtype=torch.float32, device=dev)
    k_idx = k.copy()
    k_idx[0, 2] -= 0.5  # gsplat uses pixel-index centres
    k_idx[1, 2] -= 0.5
    ks = torch.tensor(k_idx, dtype=torch.float32, device=dev)

    mesh = np.load(work / "fuse" / "mesh.npz")
    pts, cols = mesh["positions"], mesh["colors"]
    rng = np.random.default_rng(seed)
    if len(pts) > p.max_init_points:
        sel = rng.choice(len(pts), p.max_init_points, replace=False)
        pts, cols = pts[sel], cols[sel]
    from scipy.spatial import cKDTree

    dist, _ = cKDTree(pts).query(pts, k=4)
    init_scale = np.log(np.clip(dist[:, 1:].mean(1), 1e-4, None))
    n = len(pts)
    params = torch.nn.ParameterDict({
        "means": torch.nn.Parameter(torch.tensor(pts, dtype=torch.float32, device=dev)),
        "scales": torch.nn.Parameter(torch.tensor(np.repeat(init_scale[:, None], 3, 1), dtype=torch.float32, device=dev)),
        "quats": torch.nn.Parameter(torch.tensor(np.tile([1.0, 0, 0, 0], (n, 1)), dtype=torch.float32, device=dev)),
        "opacities": torch.nn.Parameter(torch.full((n,), 2.0, device=dev)),
        "colors": torch.nn.Parameter(torch.logit(torch.tensor(np.clip(cols, 0.02, 0.98), dtype=torch.float32, device=dev))),
    })
    lrs = {"means": p.lr_means, "scales": 5e-3, "quats": 1e-3, "opacities": 5e-2, "colors": 2.5e-3}
    optimizers = {k_: torch.optim.Adam([{"params": params[k_], "lr": lr, "name": k_}], eps=1e-15)
                  for k_, lr in lrs.items()}
    strategy = DefaultStrategy(refine_stop_iter=int(p.iterations * 0.6))
    strategy.check_sanity(params, optimizers)
    state = strategy.initialize_state()
    g = torch.Generator(device="cpu").manual_seed(seed)
    loss_v = float("nan")
    for step in range(p.iterations):
        i = int(torch.randint(len(gt), (1,), generator=g))
        renders, _alphas, meta = rasterization(
            params["means"], torch.nn.functional.normalize(params["quats"], dim=-1), torch.exp(params["scales"]),
            torch.sigmoid(params["opacities"]), torch.sigmoid(params["colors"]), viewmats[i:i + 1], ks[None],
            w, h, packed=False)
        strategy.step_pre_backward(params, optimizers, state, step, meta)
        loss = torch.abs(renders[0] - gt[i]).mean()
        loss.backward()
        strategy.step_post_backward(params, optimizers, state, step, meta, packed=False)
        for opt in optimizers.values():
            opt.step()
            opt.zero_grad(set_to_none=True)
        loss_v = float(loss)
        if step % 1000 == 0:
            LOG.info("splat step %d/%d  L1 %.4f  gaussians %d", step, p.iterations, loss_v, len(params["means"]))
    with torch.no_grad():
        _write_ply(out / "splat.ply", params["means"].cpu().numpy(), torch.sigmoid(params["colors"]).cpu().numpy(),
                   params["opacities"].cpu().numpy(), params["scales"].cpu().numpy(),
                   torch.nn.functional.normalize(params["quats"], dim=-1).cpu().numpy())
    report = {"gaussians": int(len(params["means"])), "iterations": p.iterations, "final_l1": loss_v}
    write_json(out / "splat.json", report)
    return report
