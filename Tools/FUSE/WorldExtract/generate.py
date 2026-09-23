#!/usr/bin/env python3
"""Drive Matrix-Game 2.0 (Skywork, MIT) with reconstruction-friendly scripted actions.

    generate.py --seed-image start.png --preset walkthrough --out build/worldextract/gen \\
                --mg-repo ~/src/Matrix-Game/Matrix-Game-2 --weights ~/models/Matrix-Game-2.0
    generate.py --seed-image start.png --preset orbit --out /tmp/x --dry-run   # no GPU needed

Writes <out>/<name>.mp4 (raw frames, no key/mouse HUD), <name>.actions.json (per-frame keyboard/mouse
exactly as fed to the model, plus the preset and gains) and <name>.gen.json (provenance: model,
revision, weights sha256, seed, hashes). `--dry-run` validates everything that does not need the GPU
and writes the action log.

This wraps the upstream code; it does not reimplement the model. The model is imported from
`--mg-repo` (a checkout of github.com/SkyworkAI/Matrix-Game, directory Matrix-Game-2, set up per its
README) and weights come from `--weights` (huggingface.co/Skywork/Matrix-Game-2.0).
"""

from __future__ import annotations

import argparse
import dataclasses
import math
import os
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import numpy as np
except ImportError:
    sys.exit("error: generate.py needs numpy (activate the Matrix-Game-2 environment or "
             "pip install -r requirements.txt)")

from worldextract import __version__  # noqa: E402
from worldextract import actions as act  # noqa: E402
from worldextract.geometry import yaw_of  # noqa: E402
from worldextract.common import (  # noqa: E402
    EXIT_FAILED,
    EXIT_OK,
    LOG,
    WorldExtractError,
    read_json,
    repo_revision,
    setup_logging,
    sha256_file,
    sha256_file_cached,
    write_json,
)

GEN_SCHEMA = "fuse.worldextract.generation/1"
MIN_TOTAL_VRAM_GB = 22.0  # upstream README: "at least 24 GB" (A100/H100 tested); 3090/4090 report ~23.6
MODEL_LICENCE_NOTE = (
    "Matrix-Game 2.0 code (github.com/SkyworkAI/Matrix-Game) and weights (Skywork/Matrix-Game-2.0) are MIT. "
    "The weights card names Skywork/SkyReels-V2-I2V-1.3B-540P (Skywork community licence) as base_model; "
    "Skywork, the rights holder of both, released the derived weights under MIT."
)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"WorldExtract {__version__}")
    ap.add_argument("--seed-image", type=Path, required=True, help="start frame (PNG/JPEG); resize-cropped to 640x352")
    ap.add_argument("--seed-image-origin", default="own", choices=["own", "library", "original"],
                    help="licence origin of the start image (original game assets make outputs recipe-only)")
    ap.add_argument("--out", type=Path, required=True, help="output directory (not inside the repo tree)")
    ap.add_argument("--name", default="", help="output basename (default: <preset>_s<seed>)")
    ap.add_argument("--preset", default="walkthrough", choices=list(act.PRESETS))
    ap.add_argument("--mode", default="universal", choices=list(act.MODES))
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--frames", type=int, default=597, help="target video frames (rounded up to a valid latent count)")
    ap.add_argument("--speed", type=float, default=0.5, help="movement duty cycle 0.05..1 (slow dolly = low)")
    ap.add_argument("--max-turn-deg", type=float, default=1.0, help="max heading change per frame (assumed gains)")
    ap.add_argument("--rows", type=int, default=4, help="strafe_scan rows (even)")
    ap.add_argument("--deg-per-mouse-unit", type=float, default=act.Gains.deg_per_mouse_unit)
    ap.add_argument("--m-per-frame", type=float, default=act.Gains.m_per_frame)
    ap.add_argument("--strafe-ratio", type=float, default=act.Gains.strafe_ratio)
    ap.add_argument("--calibration", type=Path, help="poses.json from `extract.py poses` on a calibrate run")
    ap.add_argument("--fps", type=float, default=25.0, help="MP4 container fps (the model's real-time rate)")
    ap.add_argument("--mg-repo", type=Path, help="Matrix-Game-2 directory of a SkyworkAI/Matrix-Game checkout")
    ap.add_argument("--weights", type=Path, help="local snapshot of huggingface.co/Skywork/Matrix-Game-2.0")
    ap.add_argument("--min-vram-gb", type=float, default=MIN_TOTAL_VRAM_GB)
    ap.add_argument("--keep-encoder-on-gpu", action="store_true",
                    help="do not move the VAE encoder + CLIP to CPU after encoding the seed image")
    ap.add_argument("--skip-weights-hash", action="store_true", help="do not sha256 the 6 GB checkpoint")
    ap.add_argument("--dry-run", action="store_true", help="validate + write the action log only (no GPU)")
    ap.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    return ap


def _check_image(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise WorldExtractError(f"--seed-image {path} does not exist")
    head = path.read_bytes()[:12]
    if not (head.startswith(b"\x89PNG") or head.startswith(b"\xff\xd8") or head[8:12] == b"WEBP"):
        raise WorldExtractError(f"--seed-image {path} is not a PNG/JPEG/WebP file")
    info: dict[str, Any] = {"path": str(path.resolve()), "sha256": sha256_file(path)}
    try:
        import cv2

        img = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if img is None:
            raise WorldExtractError(f"--seed-image {path} could not be decoded")
        h, w = img.shape[:2]
        info["size"] = [w, h]
        if w < 320 or h < 176:
            LOG.warning("seed image %dx%d is smaller than half the model resolution 640x352", w, h)
    except ImportError:
        pass
    return info


def _gains(args: argparse.Namespace) -> act.Gains:
    g = act.Gains(args.deg_per_mouse_unit, args.m_per_frame, args.strafe_ratio,
                  "command-line" if (args.deg_per_mouse_unit != act.Gains.deg_per_mouse_unit
                                     or args.m_per_frame != act.Gains.m_per_frame) else "assumed-default")
    if args.calibration:
        cal = read_json(args.calibration).get("calibration") or {}
        if not cal.get("deg_per_mouse_unit"):
            raise WorldExtractError(f"{args.calibration} has no calibration block (run extract.py poses with --actions)")
        g.deg_per_mouse_unit = float(cal["deg_per_mouse_unit"])
        if cal.get("m_per_frame"):
            g.m_per_frame = float(cal["m_per_frame"])
        if cal.get("strafe_ratio"):
            g.strafe_ratio = float(cal["strafe_ratio"])
        g.source = f"calibrated:{args.calibration}"
    if not (0.1 <= g.deg_per_mouse_unit <= 200.0) or g.m_per_frame <= 0:
        raise WorldExtractError("implausible gains (deg_per_mouse_unit must be 0.1..200, m_per_frame > 0)")
    return g


def _check_gpu(min_total_gb: float) -> dict[str, Any]:
    try:
        import torch
    except ImportError as exc:
        raise WorldExtractError("PyTorch is not installed; set up the Matrix-Game-2 environment (see README)") from exc
    if not torch.cuda.is_available():
        raise WorldExtractError("no CUDA device visible. Matrix-Game 2.0 needs an NVIDIA GPU with >= 24 GB "
                                "(RTX 3090/4090, A100, H100). Use --dry-run to validate without a GPU.")
    props = torch.cuda.get_device_properties(0)
    total = props.total_memory / 2**30
    free, _ = torch.cuda.mem_get_info(0)
    free_gb = free / 2**30
    info = {"device": props.name, "total_gb": round(total, 2), "free_gb": round(free_gb, 2),
            "capability": f"{props.major}.{props.minor}", "torch": torch.__version__, "cuda": torch.version.cuda}
    if (props.major, props.minor) < (8, 0):
        raise WorldExtractError(f"{props.name} (sm_{props.major}{props.minor}) lacks bf16 tensor cores; "
                                "Matrix-Game 2.0 runs in bf16 and needs Ampere (sm_80/86) or newer")
    if total < min_total_gb:
        raise WorldExtractError(f"{props.name} has {total:.1f} GB; Matrix-Game 2.0 needs >= {min_total_gb:.0f} GB "
                                "(upstream tested 24 GB+). Close other GPU apps or use a larger card.")
    if free_gb < min_total_gb - 2.0:
        raise WorldExtractError(f"only {free_gb:.1f} GB of {total:.1f} GB VRAM is free; close other GPU "
                                "processes (desktop compositor, browsers, other notebooks) and retry")
    return info


def _check_model_files(mg_repo: Path | None, weights: Path | None, mode: act.ModeSpec) -> tuple[Path, Path]:
    if mg_repo is None or weights is None:
        raise WorldExtractError("--mg-repo and --weights are required unless --dry-run")
    mg_repo, weights = mg_repo.resolve(), weights.resolve()
    for rel in ("inference.py", "pipeline/causal_inference.py", mode.config_yaml):
        if not (mg_repo / rel).exists():
            raise WorldExtractError(f"{mg_repo} is not a Matrix-Game-2 checkout (missing {rel})")
    for rel in [mode.checkpoint, *act.MG2_SHARED_FILES]:
        if not (weights / rel).exists():
            raise WorldExtractError(f"weights file missing: {weights / rel}. Download with: "
                                    f"huggingface-cli download {act.MG2_HF_REPO} --local-dir {weights}")
    return mg_repo, weights


def _write_video(path: Path, frames: np.ndarray, fps: float) -> None:
    """frames: (T, H, W, 3) uint8 RGB. Prefers imageio-ffmpeg (libx264, in the MG2 env), else OpenCV mp4v."""
    try:
        import imageio.v2 as imageio

        with imageio.get_writer(str(path), fps=fps, codec="libx264", quality=9, macro_block_size=8,
                                ffmpeg_params=["-pix_fmt", "yuv420p"]) as wr:
            for f in frames:
                wr.append_data(f)
        return
    except (ImportError, OSError, RuntimeError) as exc:
        LOG.warning("imageio-ffmpeg unavailable (%s); writing MPEG-4 Part 2 with OpenCV", exc)
    import cv2

    h, w = frames.shape[1:3]
    wr = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*"mp4v"), fps, (w, h))
    for f in frames:
        wr.write(cv2.cvtColor(f, cv2.COLOR_RGB2BGR))
    wr.release()


def _run_matrix_game(mg_repo: Path, weights: Path, mode: act.ModeSpec, seed_image: Path, latent: int, seed: int,
                     keyboard: np.ndarray, mouse: np.ndarray, offload_encoder: bool) -> np.ndarray:
    """Load the upstream InteractiveGameInference and run its pipeline with *our* action tensors."""
    import torch

    cwd = os.getcwd()
    os.chdir(mg_repo)  # upstream configs use repo-relative paths (configs/distilled_model/...)
    sys.path.insert(0, str(mg_repo))
    try:
        import inference as mg_inference  # upstream Matrix-Game-2/inference.py
        from diffusers.utils import load_image
        from einops import rearrange
        from utils.misc import set_seed

        set_seed(seed)
        ns = SimpleNamespace(config_path=mode.config_yaml, checkpoint_path=str(weights / mode.checkpoint),
                             img_path=str(seed_image), output_folder=str(mg_repo / "outputs"),
                             num_output_frames=latent, seed=seed, pretrained_model_path=str(weights))
        t0 = time.time()
        game = mg_inference.InteractiveGameInference(ns)
        LOG.info("Matrix-Game 2.0 loaded in %.1fs; VRAM allocated %.1f GB", time.time() - t0,
                 torch.cuda.memory_allocated() / 2**30)
        cfg_mode = game.config.pop("mode")
        if cfg_mode != mode.name:
            raise WorldExtractError(f"config {mode.config_yaml} is mode '{cfg_mode}', expected '{mode.name}'")
        dev, dt = game.device, game.weight_dtype
        image = game._resizecrop(load_image(str(seed_image)), act.MG2_HEIGHT, act.MG2_WIDTH)
        image = game.frame_process(image)[None, :, None, :, :].to(dtype=dt, device=dev)
        padding = torch.zeros_like(image).repeat(1, 1, 4 * (latent - 1), 1, 1)
        img_cond = torch.concat([image, padding], dim=2)
        tiler = {"tiled": True, "tile_size": [44, 80], "tile_stride": [23, 38]}  # upstream values
        img_cond = game.vae.encode(img_cond, device=dev, **tiler).to(dev)
        mask_cond = torch.ones_like(img_cond)
        mask_cond[:, :, 1:] = 0
        cond = {"cond_concat": torch.cat([mask_cond[:, :4], img_cond], dim=1).to(dev, dt),
                "visual_context": game.vae.clip.encode_video(image).to(dev, dt),
                "keyboard_cond": torch.tensor(keyboard, dtype=dt, device=dev)[None]}
        if mode.has_mouse:
            cond["mouse_cond"] = torch.tensor(mouse, dtype=dt, device=dev)[None]
        if offload_encoder:
            game.vae.to("cpu")  # encoder + CLIP are not used again; frees VRAM for the KV caches
            torch.cuda.empty_cache()
        noise = torch.randn([1, 16, latent, 44, 80], device=dev, dtype=dt)
        torch.cuda.reset_peak_memory_stats()
        t0 = time.time()
        with torch.no_grad():
            videos = game.pipeline.inference(noise=noise, conditional_dict=cond, return_latents=False,
                                             mode=mode.name, profile=False)
        video = rearrange(torch.cat(videos, dim=1), "B T C H W -> B T H W C")
        video = ((video.float() + 1) * 127.5).clip(0, 255).cpu().numpy().astype(np.uint8)[0]
        LOG.info("generated %d frames in %.1fs (%.1f fps); peak VRAM %.1f GB", video.shape[0], time.time() - t0,
                 video.shape[0] / max(time.time() - t0, 1e-6), torch.cuda.max_memory_allocated() / 2**30)
        return video
    finally:
        os.chdir(cwd)


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    setup_logging(args.log_level)
    try:
        mode = act.MODES[args.mode]
        if mode.licence_review.startswith("REVIEW"):
            LOG.warning("mode %s: %s", mode.name, mode.licence_review)
        gains = _gains(args)
        latent = act.mg2_latent_frames_for(args.frames)
        n_frames = act.mg2_video_frames(latent)
        max_turn = min(args.max_turn_deg, act.MG2_CAM_VALUE * gains.deg_per_mouse_unit)
        if max_turn < args.max_turn_deg:
            LOG.info("max turn limited to %.3f deg/frame (mouse |%.2f| x %.2f deg/unit)", max_turn,
                     act.MG2_CAM_VALUE, gains.deg_per_mouse_unit)
        params = act.TrajectoryParams(preset=args.preset, num_frames=n_frames, speed=args.speed,
                                      max_turn_deg=max_turn, rows=args.rows)
        try:
            intent = act.build_intent(params)
            keyboard, mouse, warnings = act.quantise(intent, mode, gains, params.key_hold)
        except ValueError as exc:
            raise WorldExtractError(str(exc)) from exc
        for w in warnings:
            LOG.warning("%s", w)
        prior = act.integrate(keyboard, mouse, mode.keyboard_labels, gains)
        c = prior[:, :3, 3]
        path_len = float(np.linalg.norm(np.diff(c, axis=0), axis=1).sum())
        gap = float(np.linalg.norm(c[-1] - c[0]))
        yaw_gap = math.degrees((yaw_of(prior[-1]) - yaw_of(prior[0]) + math.pi) % (2 * math.pi) - math.pi)
        LOG.info("preset %s: %d frames (%d latents), path %.2f m, closure gap %.3f m / %.2f deg (assumed gains)",
                 args.preset, n_frames, latent, path_len, gap, yaw_gap)

        seed_info = _check_image(args.seed_image)
        seed_info["origin"] = args.seed_image_origin
        name = args.name or f"{args.preset}_s{args.seed}"
        out = args.out.resolve()
        out.mkdir(parents=True, exist_ok=True)
        model: dict[str, Any] = {
            "name": "Matrix-Game-2.0", "hf_repo": act.MG2_HF_REPO, "revision": act.MG2_HF_REVISION,
            "code": "https://github.com/SkyworkAI/Matrix-Game/tree/main/Matrix-Game-2", "licence": "MIT",
            "licence_note": MODEL_LICENCE_NOTE, "checkpoint": mode.checkpoint,
            "checkpoint_sha256_expected": mode.checkpoint_sha256, "checkpoint_sha256": None,
            "licence_review": mode.licence_review, "resolution": [act.MG2_WIDTH, act.MG2_HEIGHT],
            "latent_frames": latent,
        }
        gpu: dict[str, Any] = {}
        mg_repo = weights = None
        if not args.dry_run:
            mg_repo, weights = _check_model_files(args.mg_repo, args.weights, mode)
            gpu = _check_gpu(args.min_vram_gb)
            LOG.info("GPU: %s", gpu)
            if not args.skip_weights_hash:
                LOG.info("hashing %s (cached after the first run)", weights / mode.checkpoint)
                digest = sha256_file_cached(weights / mode.checkpoint)
                model["checkpoint_sha256"] = digest
                if digest != mode.checkpoint_sha256:
                    LOG.warning("checkpoint sha256 %s differs from the pinned %s (revision %s)", digest,
                                mode.checkpoint_sha256, act.MG2_HF_REVISION)
            rev_file = mg_repo.parent / ".git" / "HEAD"
            model["code_revision"] = rev_file.read_text(encoding="utf-8").strip() if rev_file.exists() else "unknown"

        log = act.make_action_log(mode=mode, params=params, gains=gains, keyboard=keyboard, mouse=mouse,
                                  intent=intent, seed=args.seed, fps=args.fps, width=act.MG2_WIDTH,
                                  height=act.MG2_HEIGHT, model=model, seed_image=seed_info,
                                  extra={"prior_summary": {"path_length_m": round(path_len, 4),
                                                           "closure_gap_m": round(gap, 4),
                                                           "closure_gap_yaw_deg": round(yaw_gap, 3)}})
        actions_path = out / f"{name}.actions.json"
        write_json(actions_path, log)
        LOG.info("wrote action log %s", actions_path)

        gen: dict[str, Any] = {"schema": GEN_SCHEMA, "dry_run": args.dry_run, "tool_revision": repo_revision(),
                               "tool_version": __version__, "argv": sys.argv[1:], "model": model, "gpu": gpu,
                               "actions": {"path": actions_path.name, "sha256": sha256_file(actions_path)},
                               "video": None, "trajectory": dataclasses.asdict(params)}
        if not args.dry_run:
            assert mg_repo is not None and weights is not None
            t0 = time.time()
            video = _run_matrix_game(mg_repo, weights, mode, args.seed_image.resolve(), latent, args.seed,
                                     keyboard, mouse, offload_encoder=not args.keep_encoder_on_gpu)
            if video.shape[0] != n_frames:
                LOG.warning("model returned %d frames, action log has %d", video.shape[0], n_frames)
            vpath = out / f"{name}.mp4"
            _write_video(vpath, video, args.fps)
            gen["video"] = {"path": vpath.name, "sha256": sha256_file(vpath), "frames": int(video.shape[0]),
                            "seconds": round(time.time() - t0, 2)}
            LOG.info("wrote %s", vpath)
        write_json(out / f"{name}.gen.json", gen)
        if args.dry_run:
            LOG.info("dry run OK: arguments valid; action log written (no video generated)")
    except WorldExtractError as exc:
        LOG.error("%s", exc)
        return EXIT_FAILED
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
