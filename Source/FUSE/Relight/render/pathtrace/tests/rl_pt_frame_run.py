#!/usr/bin/env python3
"""FUSE Relight RL-5.1: Wine test driver of relight.frame.mode = pathtrace (d3d9.dll, PathTraceFrameRenderer).

Runs an RL-0.4 test app through the Relight d3d9.dll under cmake/toolchains/fuse-wine-xvfb-run.sh (the RL-4.2 raster
driver's run machinery, imported from --raster-tools; environment handling: RL-1.1's rl_tap_run.py) with the capture
tap and the frame orchestration in path-tracing mode (FUSE_RELIGHT_FRAME_MODE=pathtrace, FUSE_RELIGHT_PT_SPP samples
per frame, FUSE_RELIGHT_PT_REFERENCE_SPP: the in-process CPU reference of each scene):

  frame        two runs, 4 and 64 samples per frame: every frame is injected and rendered by the path tracer ("pass":
               "pathtrace", rendered, meshes, lights, triangles, spp); ff_lit rotates its spheres every frame, so every
               frame restarts the accumulation (reset, sample_base 0) while only transforms change (one scene compile,
               one GPU scene / BLAS build in the run); convergence toward the CPU reference of the same captured
               scene: at 64 spp every frame's 8x8 block means are within 4 sigma of it (0 failing blocks), and the RMSE to it
               at 64 spp is <= 0.5x the one at 4 spp (expected ~0.28x); FUSE's output dump is lit (non-black pixels)
               and the background at (1, 1) is black (ff_lit's black clear).
  determinism  the same app rendered twice: bit-identical output dumps and frame records.

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb).
"""
import argparse
import json
import os
import shutil
import sys

SKIP = 77
SPP_LOW = 4
SPP_HIGH = 64
REF_SPP = "256"


def load(path, name):
    sys.path.insert(0, path)
    return __import__(name)


def env_for(spp):
    return {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "capture", "FUSE_RELIGHT_FRAME_MODE": "pathtrace",
            "FUSE_RELIGHT_FRAME_STATS": "relight_frame.jsonl", "FUSE_RELIGHT_FRAME_DUMP": "relight_output.rgba",
            "FUSE_RELIGHT_PT_SPP": str(spp), "FUSE_RELIGHT_PT_REFERENCE_SPP": REF_SPP}


def pt_run(raster, tools, args, run_dir, spp):
    env = dict(env_for(spp), DXVK_RTX_CONFIG_FILE=raster.rtx_conf(args.out, args.app))
    rc, text = raster.run(tools, args, args.exe[0], run_dir, env)
    if rc != 0:
        return rc, None, None, text
    recs = raster.records(run_dir) or []
    path = os.path.join(run_dir, "relight_output.rgba")
    if not os.path.isfile(path):
        return 1, None, recs, text + "\nno relight_output.rgba"
    with open(path, "rb") as f:
        rgba = f.read()
    return rc, raster.rgb_of(rgba), recs, text


def check_frames(recs, spp, fail):
    """Per-frame checks of one run; returns the frames' RMSE to the CPU reference."""
    frames = [r for r in recs if r.get("ev") == "frame"]
    if not frames:
        fail(f"spp {spp}: no frame records")
    rmse = []
    for r in frames:
        p = r.get("raster", {})
        print(f"  spp {spp} frame {r['frame']}: pass {r.get('pass')} rendered {p.get('rendered')} meshes "
              f"{p.get('meshes')} triangles {p.get('triangles')} lights {p.get('lights')} reset {p.get('reset')} "
              f"recompiles {p.get('recompiles')} builds {p.get('scene_builds')} ref_rmse {p.get('ref_rmse')} "
              f"ref_failing {p.get('ref_failing')}/{p.get('ref_blocks')} worst z {p.get('ref_worst_z')}")
        if r.get("pass") != "pathtrace" or not p.get("rendered"):
            fail(f"spp {spp} frame {r['frame']}: not path traced ({r.get('pass')}, {r.get('error')}, "
                 f"{p.get('error')})")
            continue
        if not p.get("meshes") or not p.get("triangles") or not p.get("lights") or p.get("spp") != spp:
            fail(f"spp {spp} frame {r['frame']}: empty scene / wrong spp {p}")
        # ff_lit rotates its spheres every frame: every frame restarts the accumulation, but only the transforms
        # change (no recompile, no GPU scene / BLAS rebuild).
        if not p.get("reset") or p.get("sample_base") != 0:
            fail(f"spp {spp} frame {r['frame']}: moved instances did not restart the accumulation")
        if p.get("recompiles") != 1 or p.get("scene_builds") != 1:
            fail(f"spp {spp} frame {r['frame']}: a transform-only change recompiled / rebuilt the scene")
        # The block test needs a usable per-pixel variance estimate: gated at the high sample count only (4 samples
        # of a heavy-tailed estimator underestimate it; that run is the RMSE baseline).
        if spp == SPP_HIGH and (p.get("ref_failing") != 0 or not p.get("ref_blocks")):
            fail(f"spp {spp} frame {r['frame']}: block means not within 4 sigma of the CPU reference")
        rmse.append(p.get("ref_rmse", -1.0))
    return rmse


def cmd_frame(raster, tools, args):
    results = {}
    failures = []
    fail = failures.append
    for spp in (SPP_LOW, SPP_HIGH):
        rc, rgb, recs, text = pt_run(raster, tools, args, os.path.join(args.out, f"spp{spp}"), spp)
        if rc == SKIP:
            print(text.strip())
            return SKIP
        if rc != 0:
            print(f"FAIL: {args.app}: spp {spp}: exited with {rc}")
            return 1
        header = next((r for r in recs if r.get("ev") == "header"), {})
        print(f"spp {spp} header: {json.dumps(header.get('raster', {}))}")
        rmse = check_frames(recs, spp, fail)
        results[spp] = rmse
        if rgb is not None:
            lit = sum(1 for i in range(0, len(rgb), 3) if max(rgb[i:i + 3]) > 8)
            print(f"  spp {spp} output: {lit} lit pixels of {len(rgb) // 3}, (1, 1) = {raster.px(rgb, 1, 1)}")
            if lit < 200:
                fail(f"spp {spp}: output nearly black ({lit} lit pixels)")
            if max(raster.px(rgb, 1, 1)) > 2:
                fail(f"spp {spp}: background (1, 1) not black: {raster.px(rgb, 1, 1)}")
    lo, hi = results.get(SPP_LOW, []), results.get(SPP_HIGH, [])
    # RMSE^2 ~ var (1 / spp + 1 / REF_SPP): 16x the samples -> ~0.28x the RMSE; gate at 0.5x.
    if not lo or not hi or min(lo + hi) < 0 or hi[-1] > 0.5 * lo[-1]:
        fail(f"no convergence toward the CPU reference: rmse at {SPP_LOW} spp {lo}, at {SPP_HIGH} spp {hi}")
    for f in failures:
        print("FAIL: " + f)
    if failures:
        return 1
    print(f"PASS: {args.app}: path-traced frames within 4 sigma of the CPU reference; rmse {lo[-1]:.4f} at "
          f"{SPP_LOW} spp -> {hi[-1]:.4f} at {SPP_HIGH} spp")
    return 0


def cmd_determinism(raster, tools, args):
    outs = []
    for k in range(2):
        rc, rgb, recs, text = pt_run(raster, tools, args, os.path.join(args.out, f"run{k}"), SPP_LOW)
        if rc == SKIP:
            print(text.strip())
            return SKIP
        if rc != 0:
            print(f"FAIL: {args.app}: run {k} exited with {rc}")
            return 1
        frames = [r for r in recs if r.get("ev") == "frame"]
        for r in frames:
            for key in ("acquire", "release"):
                r.pop(key, None)
        outs.append((rgb, json.dumps(frames, sort_keys=True)))
    if outs[0][0] != outs[1][0]:
        same, worst, frac = raster.compare(outs[0][0], outs[1][0])
        print(f"FAIL: {args.app}: two runs differ (max {worst}, {frac * 100:.2f}% pixels)")
        return 1
    if outs[0][1] != outs[1][1]:
        print(f"FAIL: {args.app}: two runs' frame records differ")
        return 1
    print(f"PASS: {args.app}: two path-traced runs bit-identical (dumps and frame records)")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--tap-tools", required=True)
    p.add_argument("--raster-tools", required=True)
    p.add_argument("--exe", action="append", required=True)
    p.add_argument("--app", required=True)
    p.add_argument("--d3d9", required=True)
    p.add_argument("--d3d8", required=True)
    p.add_argument("--runner", required=True)
    p.add_argument("--prefix-root", required=True)
    p.add_argument("cmd", choices=["frame", "determinism"])
    p.add_argument("--out", required=True)
    args = p.parse_args()
    if not shutil.which("wine") and not shutil.which("wine64"):
        print("SKIP: wine not installed")
        return SKIP
    raster = load(args.raster_tools, "rl_raster_run")
    tools = raster.load_tap_tools(args.tap_tools)
    os.makedirs(args.out, exist_ok=True)
    return {"frame": cmd_frame, "determinism": cmd_determinism}[args.cmd](raster, tools, args)


if __name__ == "__main__":
    sys.exit(main())
