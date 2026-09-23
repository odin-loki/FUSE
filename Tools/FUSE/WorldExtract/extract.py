#!/usr/bin/env python3
"""FUSE WorldExtract: playable world-model video (+ action log) -> 3D world as FUSE assets.

    extract.py all --video run.mp4 --actions run.actions.json --work build/worldextract/run
    extract.py frames|poses|depth|fuse|splat|collision|export ...   (one stage; resumable)
    extract.py verify --work DIR           re-hash every exported file against the manifest
    extract.py download-models --what moge3,mapanything   (the ONLY networked command)

Every stage caches under --work, is skipped when its parameters and upstream results are unchanged
(--force recomputes), logs to <work>/extract.log, and is deterministic for a given --seed.
See README.md for the pipeline, licences and limitations.
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import sys
import time
from pathlib import Path
from typing import Any, Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))

from worldextract import __version__  # noqa: E402
from worldextract.common import (  # noqa: E402
    EXIT_FAILED,
    EXIT_OK,
    EXIT_USAGE,
    LOG,
    Stage,
    WorldExtractError,
    canonical_json,
    cuda_available,
    missing_modules,
    read_json,
    seed_everything,
    setup_logging,
    sha256_bytes,
    sha256_file,
    upstream_hash,
    write_json,
)

CORE_MODULES = ["numpy", "cv2", "scipy", "open3d"]
STAGES = ["frames", "poses", "depth", "fuse", "splat", "collision", "export"]
MODEL_IDS = {
    "moge3": "Ruicheng/moge-3-vitl",
    "moge2": "Ruicheng/moge-2-vitl-normal",
    "da2-small": "depth-anything/Depth-Anything-V2-Small-hf",
    "mapanything": "facebook/map-anything-apache",
}


def build_parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    g = common.add_argument_group("inputs / run control")
    g.add_argument("--work", type=Path, required=True, help="work/cache directory (keep it outside the repo or under build/)")
    g.add_argument("--video", type=Path, help="input MP4 (required for the frames stage)")
    g.add_argument("--actions", type=Path, help="per-frame action log JSON written by generate.py (optional prior)")
    g.add_argument("--gen-manifest", type=Path, help="<name>.gen.json from generate.py (provenance)")
    g.add_argument("--name", default="", help="asset name (default: video stem)")
    g.add_argument("--seed", type=int, default=0)
    g.add_argument("--force", action="store_true", help="recompute stages even if cached")
    g.add_argument("--cpu", action="store_true", help="never use CUDA (CI / synthetic test mode)")
    g.add_argument("--allow-download", action="store_true",
                   help="allow Hugging Face downloads at runtime (default: offline; use download-models)")
    g.add_argument("--threads", type=int, default=0)
    g.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    fr = common.add_argument_group("frames")
    fr.add_argument("--stride", type=int, default=1)
    fr.add_argument("--max-frames", type=int, default=0)
    fr.add_argument("--min-parallax-px", type=float, default=0.0)
    fr.add_argument("--max-gap", type=int, default=12)
    fr.add_argument("--blur-percentile", type=float, default=15.0)
    fr.add_argument("--hud", default="auto", help="auto | none | 'x0,y0,x1,y1;...' pixel boxes to mask")
    po = common.add_argument_group("poses")
    po.add_argument("--pose-backend", default="auto", choices=["auto", "colmap", "mapanything"])
    po.add_argument("--mapper", default="incremental", choices=["incremental", "global"])
    po.add_argument("--camera-model", default="SIMPLE_PINHOLE", choices=["SIMPLE_PINHOLE", "PINHOLE", "SIMPLE_RADIAL"])
    po.add_argument("--hfov-deg", type=float, default=0.0, help="initial horizontal FOV (0 = action log / 80)")
    po.add_argument("--seq-overlap", type=int, default=8)
    po.add_argument("--no-loop-pairs", action="store_true")
    po.add_argument("--max-features", type=int, default=4096)
    po.add_argument("--scale-mode", default="eye-height", choices=["eye-height", "action-speed", "none"])
    po.add_argument("--eye-height", type=float, default=0.0, help="metres (0 = action log hint / 1.7)")
    po.add_argument("--loop-closure", default="auto", choices=["auto", "off", "force"])
    de = common.add_argument_group("depth")
    de.add_argument("--depth-backend", default="auto", choices=["auto", "moge3", "moge2", "da2-small", "mapanything", "sfm", "gt"])
    de.add_argument("--gt-depth", default="", help="synthetic ground-truth depth .npz (depth backend gt)")
    de.add_argument("--max-depth", type=float, default=0.0)
    fu = common.add_argument_group("fuse")
    fu.add_argument("--voxel", type=float, default=0.0, help="TSDF voxel in metres (0 = auto)")
    fu.add_argument("--target-tris", type=int, default=200_000)
    fu.add_argument("--texture", default="atlas", choices=["atlas", "vertex"])
    fu.add_argument("--atlas-size", type=int, default=2048)
    sp = common.add_argument_group("splat (optional, CUDA)")
    sp.add_argument("--splat", default="auto", choices=["auto", "on", "off"], help="`all`: auto = on when CUDA+gsplat")
    sp.add_argument("--splat-iters", type=int, default=7000)
    co = common.add_argument_group("collision / export")
    co.add_argument("--collision-tris", type=int, default=8000)
    co.add_argument("--max-slope-deg", type=float, default=40.0)
    co.add_argument("--seed-image-origin", default="", choices=["", "own", "library", "original"])
    co.add_argument("--reviewer", default="")

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"WorldExtract {__version__}")
    sub = ap.add_subparsers(dest="command", required=True)
    for name in STAGES + ["all", "verify"]:
        sub.add_parser(name, parents=[common], help=f"run {name}")
    dl = sub.add_parser("download-models", help="download FOSS reconstruction weights into the HF cache (network)")
    dl.add_argument("--what", default="moge3,mapanything", help=f"comma list of {', '.join(MODEL_IDS)}")
    dl.add_argument("--log-level", default="INFO")
    return ap


# --------------------------------------------------------------------------------------------- #


def _inputs(args: argparse.Namespace) -> dict[str, Any]:
    """Record/refresh <work>/inputs.json (video + action log + generation manifest hashes)."""
    path = args.work / "inputs.json"
    ctx: dict[str, Any] = read_json(path) if path.exists() else {}
    if args.video:
        if not args.video.exists():
            raise WorldExtractError(f"--video {args.video} does not exist")
        ctx["video"] = str(args.video.resolve())
        ctx["video_sha256"] = sha256_file(args.video)
    if args.actions:
        if not args.actions.exists():
            raise WorldExtractError(f"--actions {args.actions} does not exist")
        ctx["action_log"] = read_json(args.actions)
        ctx["action_log_sha256"] = sha256_file(args.actions)
    if args.gen_manifest:
        ctx["generation"] = read_json(args.gen_manifest)
    ctx.setdefault("name", args.name or (Path(ctx["video"]).stem if ctx.get("video") else "world"))
    if args.name:
        ctx["name"] = args.name
    write_json(path, ctx)
    return ctx


def _run_stage(args: argparse.Namespace, name: str, params: dict[str, Any], upstream: list[str],
               fn: Callable[[Path], dict[str, Any]]) -> dict[str, Any]:
    full = {"params": params, "upstream": {u: upstream_hash(args.work, u) for u in upstream}, "seed": args.seed}
    stage = Stage(args.work, name, full, force=args.force)
    if stage.is_done():
        LOG.info("[%s] cached (params %s) - skipping; use --force to recompute", name, stage.params_hash())
        return stage.summary()
    seed_everything(args.seed)
    LOG.info("[%s] running", name)
    t0 = time.time()
    out = stage.begin()
    summary = fn(out)
    summary = {**summary, "seconds": round(time.time() - t0, 2)}
    stage.finish(summary)
    LOG.info("[%s] done in %.1fs", name, summary["seconds"])
    return summary


def run_frames(args: argparse.Namespace, ctx: dict[str, Any]) -> dict[str, Any]:
    from worldextract.stages import frames

    if not ctx.get("video"):
        raise WorldExtractError("the frames stage needs --video")
    p = frames.FramesParams(max_frames=args.max_frames, stride=args.stride, min_parallax_px=args.min_parallax_px,
                            max_gap=args.max_gap, blur_percentile=args.blur_percentile, hud=args.hud)
    params = {**dataclasses.asdict(p), "video_sha256": ctx["video_sha256"]}
    return _run_stage(args, "frames", params, [], lambda out: frames.run(Path(ctx["video"]), out, p))


def run_poses(args: argparse.Namespace, ctx: dict[str, Any]) -> dict[str, Any]:
    from worldextract.stages import poses

    backend = args.pose_backend
    if args.cpu and backend == "auto":
        backend = "colmap"
    p = poses.PosesParams(backend=backend, mapper=args.mapper, camera_model=args.camera_model,
                          hfov_deg=args.hfov_deg, seq_overlap=args.seq_overlap, loop_pairs=not args.no_loop_pairs,
                          max_features=args.max_features, scale_mode=args.scale_mode, eye_height=args.eye_height,
                          loop_closure=args.loop_closure, seed=args.seed, threads=args.threads,
                          gpu=not args.cpu)
    params = {**dataclasses.asdict(p), "action_log_sha256": ctx.get("action_log_sha256")}
    return _run_stage(args, "poses", params, ["frames"],
                      lambda out: poses.run(args.work, out, p, ctx.get("action_log")))


def run_depth(args: argparse.Namespace, ctx: dict[str, Any]) -> dict[str, Any]:
    from worldextract.stages import depth

    backend = args.depth_backend
    if args.cpu and backend in ("auto", "moge3", "moge2", "da2-small", "mapanything"):
        if backend != "auto":
            raise WorldExtractError(f"--depth-backend {backend} needs CUDA; drop --cpu or use sfm/gt")
        backend = "sfm"
    p = depth.DepthParams(backend=backend, gt_depth=args.gt_depth, max_depth=args.max_depth)
    params = {**dataclasses.asdict(p), "gt_sha256": sha256_file(Path(args.gt_depth)) if args.gt_depth else None}
    return _run_stage(args, "depth", params, ["poses"], lambda out: depth.run(args.work, out, p))


def run_fuse(args: argparse.Namespace, ctx: dict[str, Any]) -> dict[str, Any]:
    from worldextract.stages import fuse

    p = fuse.FuseParams(voxel=args.voxel, target_tris=args.target_tris, texture=args.texture,
                        atlas_size=args.atlas_size)
    return _run_stage(args, "fuse", dataclasses.asdict(p), ["depth"], lambda out: fuse.run(args.work, out, p, args.seed))


def run_splat(args: argparse.Namespace, ctx: dict[str, Any]) -> dict[str, Any]:
    from worldextract.stages import splat

    p = splat.SplatParams(iterations=args.splat_iters)
    return _run_stage(args, "splat", dataclasses.asdict(p), ["fuse"], lambda out: splat.run(args.work, out, p, args.seed))


def run_collision(args: argparse.Namespace, ctx: dict[str, Any]) -> dict[str, Any]:
    from worldextract.stages import collision

    p = collision.CollisionParams(target_tris=args.collision_tris, max_slope_deg=args.max_slope_deg)
    return _run_stage(args, "collision", dataclasses.asdict(p), ["fuse"],
                      lambda out: collision.run(args.work, out, p, args.seed))


def run_export(args: argparse.Namespace, ctx: dict[str, Any]) -> dict[str, Any]:
    from worldextract.stages import export

    # The recipe hash covers every upstream stage's parameters: identical hash -> identical rebuild.
    ups = ["frames", "poses", "depth", "fuse", "collision"]
    ctx["recipe_hash"] = sha256_bytes(canonical_json({u: upstream_hash(args.work, u) for u in ups}))[:16]
    write_json(args.work / "inputs.json", ctx)
    p = export.ExportParams(name=ctx.get("name", "world"), seed_image_origin=args.seed_image_origin,
                            reviewer=args.reviewer)
    params = {**dataclasses.asdict(p), "recipe_hash": ctx["recipe_hash"]}
    return _run_stage(args, "export", params, ups, lambda out: export.run(args.work, out, p))


RUNNERS: dict[str, Callable[[argparse.Namespace, dict[str, Any]], dict[str, Any]]] = {
    "frames": run_frames, "poses": run_poses, "depth": run_depth, "fuse": run_fuse,
    "splat": run_splat, "collision": run_collision, "export": run_export,
}


def download_models(what: str) -> int:
    from worldextract.common import require

    require(["huggingface_hub"], "download-models")
    from huggingface_hub import snapshot_download

    for key in [w.strip() for w in what.split(",") if w.strip()]:
        if key not in MODEL_IDS:
            raise WorldExtractError(f"unknown model '{key}' (choose from {', '.join(MODEL_IDS)})")
        LOG.info("downloading %s (%s) into the Hugging Face cache (HF_HOME=%s)", key, MODEL_IDS[key],
                 os.environ.get("HF_HOME", "~/.cache/huggingface"))
        snapshot_download(MODEL_IDS[key])
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "download-models":
        setup_logging(args.log_level)
        try:
            return download_models(args.what)
        except WorldExtractError as exc:
            LOG.error("%s", exc)
            return EXIT_FAILED
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    setup_logging(args.log_level, args.work / "extract.log")
    if not args.allow_download:
        os.environ["HF_HUB_OFFLINE"] = "1"
        os.environ["TRANSFORMERS_OFFLINE"] = "1"
    if args.cpu:
        os.environ["CUDA_VISIBLE_DEVICES"] = ""
    missing = missing_modules(CORE_MODULES + (["pycolmap"] if args.command in ("poses", "all") else []))
    if missing:
        LOG.error("missing core python modules: %s. Install: pip install -r %s", ", ".join(missing),
                  Path(__file__).resolve().parent / "requirements.txt")
        return EXIT_USAGE
    try:
        if args.command == "verify":
            from worldextract.stages import export

            export.check_outputs(args.work / "export")
            LOG.info("verify: every exported file matches the manifest")
            return EXIT_OK
        ctx = _inputs(args)
        if args.command == "all":
            results = {}
            for name in STAGES:
                if name == "splat":
                    want = args.splat == "on" or (args.splat == "auto" and not args.cpu and cuda_available()
                                                   and not missing_modules(["gsplat"]))
                    if not want:
                        LOG.info("[splat] skipped (%s)", "--splat off" if args.splat == "off" else "no CUDA/gsplat")
                        continue
                results[name] = RUNNERS[name](args, ctx)
            write_json(args.work / "summary.json", results)
        else:
            RUNNERS[args.command](args, ctx)
    except WorldExtractError as exc:
        LOG.error("%s", exc)
        return EXIT_FAILED
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
