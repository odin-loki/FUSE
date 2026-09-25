#!/usr/bin/env python3
"""FUSE Relight RL-5.5: relight.frame.mode = pathtrace + relight.post.enable + relight.denoise.enable under Wine
(d3d9.dll, PathTraceFrameRenderer -> the RL-5.7 post hook -> PtDenoiser).

RL-5.7's run machinery (rl_post_frame_run.py -> rl_pt_frame_run.py -> the RL-4.2 raster driver) at 1 sample per pixel
with FUSE_RELIGHT_DENOISE=1 and the native upscaler ("none": the path tracer renders at the output extent):

  frame        every frame is path traced, denoised and posted: the frame record's "denoise" block reports status "ran",
               the in-tree backend ("rdn"), A-SVGF gradients, history from the second frame on; the post block is Ok
               with 0 non-finite values; the output dump is lit, ff_lit's black clear stays black, and its
               high-frequency energy (mean |Laplacian| of the luminance over the lit pixels) is below
               kMaxNoiseRatio x the same run without the denoiser.
  determinism  two denoised runs: bit-identical output dumps and frame records.
  validation   the host validation layer + synchronization validation injected (rl_post_frame_run.py's method): per
               DXVK binding model, the denoised runs add no validation message (no id's count grows) over the path
               tracer + post without the denoiser.

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb / validation layer).
"""
import argparse
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "pathtrace", "tests"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "post", "tests"))
import rl_pt_frame_run as base  # noqa: E402
import rl_post_frame_run as post  # noqa: E402

SKIP = 77
UPSCALER = "none"
MAX_NOISE_RATIO = 0.7
DENOISE = {"FUSE_RELIGHT_DENOISE": "1", "FUSE_RELIGHT_POST_RESOLUTION_SCALE": "0"}
NO_DENOISE = {"FUSE_RELIGHT_DENOISE": "0", "FUSE_RELIGHT_POST_RESOLUTION_SCALE": "0"}


def lum(rgb, i):
    return 0.2126 * rgb[i] + 0.7152 * rgb[i + 1] + 0.0722 * rgb[i + 2]


def noise(rgb):
    """Mean |4-neighbour Laplacian| of the 8-bit luminance over lit interior pixels (a high-frequency energy)."""
    w, h = post.W, post.H
    total, count = 0.0, 0
    for y in range(1, h - 1):
        for x in range(1, w - 1):
            i = (y * w + x) * 3
            c = lum(rgb, i)
            if c <= 8:
                continue
            lap = 4 * c - lum(rgb, i - 3) - lum(rgb, i + 3) - lum(rgb, i - 3 * w) - lum(rgb, i + 3 * w)
            total += abs(lap)
            count += 1
    return total / count if count else 0.0, count


def check_denoise(recs, fail):
    frames = [r for r in recs if r.get("ev") == "frame"]
    if not frames:
        fail("no frame records")
    for k, r in enumerate(frames):
        p = r.get("raster", {})
        q = p.get("post")
        d = p.get("denoise")
        if r.get("pass") != "pathtrace" or not p.get("rendered") or not isinstance(q, dict) or not isinstance(d, dict):
            fail(f"frame {r.get('frame')}: not path traced + posted + denoised ({r.get('pass')}, {p.get('error')}, "
                 f"{q}, {d})")
            continue
        if d.get("status") != "ran" or d.get("ran") is not True or d.get("backend") != "rdn":
            fail(f"frame {r['frame']}: denoiser did not run: {d}")
        if d.get("gradients") is not True:
            fail(f"frame {r['frame']}: A-SVGF gradients off: {d}")
        if k > 0 and d.get("history") is not True:
            fail(f"frame {r['frame']}: no temporal history after the first frame: {d}")
        if q.get("status") != "Ok" or q.get("nonfinite") != 0 or q.get("error"):
            fail(f"frame {r['frame']}: post status {q.get('status')} nonfinite {q.get('nonfinite')} "
                 f"error {q.get('error')}")
    if frames:
        d = frames[-1].get("raster", {}).get("denoise", {})
        print(f"  {len(frames)} frames denoised; last: {json.dumps(d, sort_keys=True)}")


def cmd_frame(raster, tools, args):
    failures = []
    fail = failures.append
    rgbs = {}
    for name, extra in (("denoised", DENOISE), ("raw", NO_DENOISE)):
        rc, rgb, recs, text = post.post_run(raster, tools, args, os.path.join(args.out, name), UPSCALER, extra)
        if rc == SKIP:
            print(text.strip())
            return SKIP
        if rc != 0:
            print(f"FAIL: {args.app}: {name}: exited with {rc}")
            print(text[-4000:])
            return 1
        if name == "denoised":
            check_denoise(recs, fail)
        rgbs[name] = rgb
        if rgb is not None:
            lit = sum(1 for i in range(0, len(rgb), 3) if max(rgb[i:i + 3]) > 8)
            bg = raster.px(rgb, 1, 1)
            if lit < 200:
                fail(f"{name}: output nearly black ({lit} lit pixels)")
            if max(bg) > 2:
                fail(f"{name}: background (1, 1) not black: {bg}")
        else:
            fail(f"{name}: no output dump")
    if rgbs.get("denoised") is not None and rgbs.get("raw") is not None:
        nd, cd = noise(rgbs["denoised"])
        nr, cr = noise(rgbs["raw"])
        print(f"  high-frequency energy (mean |Laplacian|, 8-bit luminance): denoised {nd:.3f} over {cd} px, "
              f"raw 1 spp {nr:.3f} over {cr} px (ratio {nd / nr if nr else 0:.3f})")
        if not nr or nd > MAX_NOISE_RATIO * nr:
            fail(f"denoised output not smoother than the raw 1-spp output ({nd:.3f} vs {nr:.3f})")
    for f in failures:
        print("FAIL: " + f)
    if failures:
        return 1
    print(f"PASS: {args.app}: every frame path traced at 1 spp, denoised (A-SVGF producer + rdn passes) and posted; "
          f"finite; smoother than the raw frame")
    return 0


def cmd_determinism(raster, tools, args):
    outs = []
    for k in range(2):
        rc, rgb, recs, text = post.post_run(raster, tools, args, os.path.join(args.out, f"run{k}"), UPSCALER, DENOISE)
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
    print(f"PASS: {args.app}: two denoised path-traced runs bit-identical (dumps and frame records)")
    return 0


def cmd_validation(raster, tools, args):
    layer = next((p for p in tools.HOST_LAYER_JSON if os.path.isfile(p)), None)
    if not layer:
        print("SKIP: VK_LAYER_KHRONOS_validation is not installed on the host")
        return SKIP
    common = {
        "VK_INSTANCE_LAYERS": "VK_LAYER_KHRONOS_validation",
        "VK_KHRONOS_VALIDATION_REPORT_FLAGS": "error,warn",
        "VK_LAYER_ENABLES": "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT",
        "VK_KHRONOS_VALIDATION_VALIDATE_SYNC": "true",
        "DXVK_LOG_LEVEL": "info",
        "FUSE_RELIGHT_VK_VALIDATION": "1",
    }
    failed = False
    lines = []
    for model, extra in (("legacy", {"DXVK_CONFIG": "dxvk.enableDescriptorBuffer = False"}), ("default", {})):
        result = {}
        for name, env in (("post", NO_DENOISE), ("denoise", DENOISE)):
            e = dict(common)
            e.update(extra)
            e.update(env)
            rc, rgb, recs, text = post.post_run(raster, tools, args, os.path.join(args.out, model, name), UPSCALER, e)
            if rc == SKIP:
                print(text.strip())
                return SKIP
            if rc != 0:
                print(f"FAIL: {args.app}: {model} {name}: exited with {rc}")
                return 1
            count, ids = tools.validation_messages(text)
            frames = [r for r in (recs or []) if r.get("ev") == "frame"]
            ran = [r for r in frames if r.get("raster", {}).get("denoise", {}).get("ran") is True]
            result[name] = (count, ids)
            print(f"  {model} {name}: {len(ran)}/{len(frames)} denoised frames, {count} validation messages "
                  f"{dict(ids)}")
            if name == "denoise" and not ran:
                failed = True
                print(f"FAIL: {model}: no denoised frame")
        grown = {k: v for k, v in result["denoise"][1].items() if v > result["post"][1].get(k, 0)}
        if grown:
            failed = True
            print(f"FAIL: {model}: the denoiser adds validation messages: {grown}")
        lines.append(f"{model}: post {result['post'][0]}, post + denoise {result['denoise'][0]}")
    for line in lines:
        print("  " + line)
    if failed:
        return 1
    print(f"PASS: {args.app}: the denoiser (producer + rdn passes) adds no validation / synchronization-validation "
          f"message over the path tracer + post (host layer {layer})")
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
    p.add_argument("cmd", choices=["frame", "determinism", "validation"])
    p.add_argument("--out", required=True)
    args = p.parse_args()
    if not shutil.which("wine") and not shutil.which("wine64"):
        print("SKIP: wine not installed")
        return SKIP
    raster = base.load(args.raster_tools, "rl_raster_run")
    tools = raster.load_tap_tools(args.tap_tools)
    os.makedirs(args.out, exist_ok=True)
    return {"frame": cmd_frame, "determinism": cmd_determinism, "validation": cmd_validation}[args.cmd](
        raster, tools, args)


if __name__ == "__main__":
    sys.exit(main())
