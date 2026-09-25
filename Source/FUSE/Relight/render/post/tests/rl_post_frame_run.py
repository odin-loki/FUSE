#!/usr/bin/env python3
"""FUSE Relight RL-5.7: relight.frame.mode = pathtrace + relight.post.enable under Wine (d3d9.dll, PathTraceFrameRenderer
with the RL-5.7 post hook).

RL-5.1's run machinery (rl_pt_frame_run.py -> the RL-4.2 raster driver -> rl_tap_run.py) with FUSE_RELIGHT_POST=1,
FUSE_RELIGHT_POST_UPSCALER=<id>, FUSE_RELIGHT_POST_RESOLUTION_SCALE=0.5 and 1 sample per pixel:

  frame        one run per upscaler id (none, native_taau, fsr1, nis, cas, fsr3, dlss, xess): every frame is path traced
               at the post pipeline's render extent (half the 128 x 96 output for the upscalers, the output for none /
               cas) and upscaled to the swapchain; the frame record's "post" block names the backend that ran
               (in-tree ids: themselves; fsr3: itself when the device runs it, otherwise native_taau with a reason
               naming fsr3; dlss / xess: native_taau with a reason naming the plugin), the mip bias
               log2(render / output), status Ok, 0 non-finite values, and composite == the path tracer's radiance
               section (max relative error <= 1e-5 at 1 spp with the denoiser off); the output dump is lit and
               ff_lit's black clear stays black.
  determinism  native_taau twice: bit-identical output dumps and frame records.
  validation   the host validation layer + synchronization validation injected (as rl_raster_run.py's validation):
               per DXVK binding model, the post runs (native_taau, fsr1, fsr3) add no message over the path tracer
               without post and over the capture tap alone (DXVK's own draws report messages in every mode).

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb / validation layer).
"""
import argparse
import json
import math
import os
import shutil
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "pathtrace", "tests"))
import rl_pt_frame_run as base  # noqa: E402

SKIP = 77
W, H = 128, 96
SCALE = "0.5"
CASES = [("none", "none"), ("native_taau", "native_taau"), ("fsr1", "fsr1"), ("nis", "nis"), ("cas", "cas"),
         ("fsr3", None), ("dlss", "native_taau"), ("xess", "native_taau")]


def env_for(upscaler):
    return {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "capture", "FUSE_RELIGHT_FRAME_MODE": "pathtrace",
            "FUSE_RELIGHT_FRAME_STATS": "relight_frame.jsonl", "FUSE_RELIGHT_FRAME_DUMP": "relight_output.rgba",
            "FUSE_RELIGHT_PT_SPP": "1", "FUSE_RELIGHT_PT_REFERENCE_SPP": "0", "FUSE_RELIGHT_POST": "1",
            "FUSE_RELIGHT_POST_UPSCALER": upscaler, "FUSE_RELIGHT_POST_RESOLUTION_SCALE": SCALE}


def post_run(raster, tools, args, run_dir, upscaler, extra=None, need_dump=True):
    env = dict(env_for(upscaler), DXVK_RTX_CONFIG_FILE=raster.rtx_conf(args.out, args.app))
    env.update(extra or {})
    rc, text = raster.run(tools, args, args.exe[0], run_dir, env)
    if rc != 0:
        return rc, None, None, text
    recs = raster.records(run_dir) or []
    path = os.path.join(run_dir, "relight_output.rgba")
    if not os.path.isfile(path):
        return (1 if need_dump else 0), None, recs, text + "\nno relight_output.rgba"
    with open(path, "rb") as f:
        rgba = f.read()
    return rc, raster.rgb_of(rgba), recs, text


def check_post(recs, upscaler, expected, fail):
    frames = [r for r in recs if r.get("ev") == "frame"]
    if not frames:
        fail(f"{upscaler}: no frame records")
    ran = None
    for r in frames:
        p = r.get("raster", {})
        q = p.get("post")
        if r.get("pass") != "pathtrace" or not p.get("rendered") or not isinstance(q, dict):
            fail(f"{upscaler} frame {r.get('frame')}: not path traced + posted ({r.get('pass')}, {p.get('error')}, "
                 f"{q})")
            continue
        ran = q.get("upscaler")
        want = expected
        if want is None:  # fsr3: itself when the device runs it, else the documented fallback
            want = "fsr3" if not q.get("fallback") else "native_taau"
        if q.get("upscaler") != want:
            fail(f"{upscaler} frame {r['frame']}: backend {q.get('upscaler')} (want {want})")
        if q.get("fallback") and not str(q.get("reason", "")).startswith(upscaler + ":"):
            fail(f"{upscaler} frame {r['frame']}: fallback without a reason naming it: {q.get('reason')}")
        dw, dh = q.get("display", [0, 0])
        rw, rh = q.get("render", [0, 0])
        if (dw, dh) != (W, H):
            fail(f"{upscaler} frame {r['frame']}: display {dw}x{dh}")
        native = q.get("upscaler") in ("none", "cas")
        want_rw = W if native else round(W * float(SCALE))
        want_rh = H if native else round(H * float(SCALE))
        if (rw, rh) != (want_rw, want_rh):
            fail(f"{upscaler} frame {r['frame']}: render {rw}x{rh} (want {want_rw}x{want_rh})")
        bias = 0.0 if rw == dw else math.log2(rw / dw)
        if abs(q.get("mip_bias", 99) - bias) > 1e-5:
            fail(f"{upscaler} frame {r['frame']}: mip bias {q.get('mip_bias')} != log2(render / output) {bias}")
        if q.get("status") != "Ok" or q.get("nonfinite") != 0 or q.get("error"):
            fail(f"{upscaler} frame {r['frame']}: status {q.get('status')} nonfinite {q.get('nonfinite')} "
                 f"error {q.get('error')}")
        cm = q.get("composite_max_rel", -1)
        if q.get("accumulated_input") is False and not (0 <= cm <= 1e-5):
            fail(f"{upscaler} frame {r['frame']}: composite != radiance section (max rel {cm})")
    if frames:
        q = frames[-1].get("raster", {}).get("post", {})
        print(f"  {upscaler:11s} -> {ran}: render {q.get('render')} display {q.get('display')} mip {q.get('mip_bias')} "
              f"tm {q.get('tonemapping')} composite_max_rel {q.get('composite_max_rel')} frames {len(frames)}"
              + (f" reason: {q.get('reason')}" if q.get("reason") else ""))


def cmd_frame(raster, tools, args):
    failures = []
    fail = failures.append
    for upscaler, expected in CASES:
        rc, rgb, recs, text = post_run(raster, tools, args, os.path.join(args.out, upscaler), upscaler)
        if rc == SKIP:
            print(text.strip())
            return SKIP
        if rc != 0:
            print(f"FAIL: {args.app}: {upscaler}: exited with {rc}")
            return 1
        check_post(recs, upscaler, expected, fail)
        if rgb is not None:
            lit = sum(1 for i in range(0, len(rgb), 3) if max(rgb[i:i + 3]) > 8)
            bg = raster.px(rgb, 1, 1)
            if lit < 200:
                fail(f"{upscaler}: output nearly black ({lit} lit pixels)")
            if max(bg) > 2:
                fail(f"{upscaler}: background (1, 1) not black: {bg}")
    for f in failures:
        print("FAIL: " + f)
    if failures:
        return 1
    print(f"PASS: {args.app}: path traced at the render extent, composited, upscaled by {len(CASES)} upscaler "
          f"selections (plugin ids fell back with a reason), finite, mip bias log2(render / output)")
    return 0


def cmd_determinism(raster, tools, args):
    outs = []
    for k in range(2):
        rc, rgb, recs, text = post_run(raster, tools, args, os.path.join(args.out, f"run{k}"), "native_taau")
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
    print(f"PASS: {args.app}: two post-processed path-traced runs bit-identical (dumps and frame records)")
    return 0


def cmd_validation(raster, tools, args):
    """Host validation + synchronization validation. DXVK's own draws report messages with or without Relight (the
    capture tap alone has them too), so the gate is RL-4.2's: per binding model (legacy, DXVK's default descriptor
    buffers), the path tracer + post runs add no validation message (no id's count grows) over the path tracer with
    the post pipeline off, and over the capture tap alone."""
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
    capture_only = {"FUSE_RELIGHT_FRAME_MODE": "off", "FUSE_RELIGHT_POST": "0"}
    failed = False
    lines = []
    for model, extra in (("legacy", {"DXVK_CONFIG": "dxvk.enableDescriptorBuffer = False"}), ("default", {})):
        runs = [("capture", "native_taau", dict(capture_only)), ("pt", "native_taau", {"FUSE_RELIGHT_POST": "0"})]
        runs += [(u, u, {}) for u in ("native_taau", "fsr1", "fsr3")]
        result = {}
        for name, upscaler, env in runs:
            e = dict(common)
            e.update(extra)
            e.update(env)
            rc, rgb, recs, text = post_run(raster, tools, args, os.path.join(args.out, model, name), upscaler, e,
                                           need_dump=name != "capture")
            if rc == SKIP:
                print(text.strip())
                return SKIP
            if rc != 0:
                print(f"FAIL: {args.app}: {model} {name}: exited with {rc}")
                return 1
            count, ids = tools.validation_messages(text)
            frames = [r for r in (recs or []) if r.get("ev") == "frame"]
            posted = [r for r in frames if isinstance(r.get("raster", {}).get("post"), dict)]
            result[name] = (count, ids)
            ran = posted[-1]["raster"]["post"].get("upscaler") if posted else None
            print(f"  {model} {name}: {len(posted)}/{len(frames)} posted frames ({ran}), {count} validation messages "
                  f"{dict(ids)}")
            if name not in ("capture", "pt") and not posted:
                failed = True
                print(f"FAIL: {model} {name}: no posted frame")
        for name in ("native_taau", "fsr1", "fsr3"):
            for base_name in ("pt", "capture"):
                grown = {k: v for k, v in result[name][1].items() if v > result[base_name][1].get(k, 0)}
                if grown:
                    failed = True
                    print(f"FAIL: {model} {name}: adds validation messages over {base_name}: {grown}")
        lines.append(f"{model}: capture {result['capture'][0]}, pt {result['pt'][0]}, post " +
                     "/".join(str(result[n][0]) for n in ("native_taau", "fsr1", "fsr3")))
    for line in lines:
        print("  " + line)
    if failed:
        print(f"FAIL: {args.app}: the post pipeline adds validation messages")
        return 1
    print(f"PASS: {args.app}: path tracer + post (native_taau, fsr1, fsr3) add no validation / synchronization-"
          f"validation message over the path tracer alone and the capture tap alone (host layer {layer})")
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
