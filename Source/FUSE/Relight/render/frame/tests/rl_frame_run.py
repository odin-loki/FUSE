#!/usr/bin/env python3
"""FUSE Relight RL-4.1: Wine test driver for the in-process frame orchestration (d3d9.dll).

Runs an RL-0.4 test app through the Relight d3d9.dll / d3d8.dll under cmake/toolchains/fuse-wine-xvfb-run.sh (the
run directories and environment handling are RL-1.1's rl_tap_run.py, imported from --tap-tools):

  passthrough  reference FUSE_RELIGHT=0 (DXVK's own device, no tap), then the capture tap with
               relight.frame.mode = passthrough and relight.frame.textureSwap = true: every dump must be
               bit-identical, and the frame record (relight.frame.statsPath) must show an injection in every frame
               (composite through FUSE's timeline) and, for apps with textures, passthrough swaps.
  solid        relight.frame.mode = solid: the injection happens at the first UI draw; where the reference shows
               the scene, the dump shows FUSE's colour; the HUD pixels drawn after the injection point are the
               reference's (the UI draws land on top of the composite).

Both run with an rtx.conf (DXVK_RTX_CONFIG_FILE) that sets rtx.preTransformedVerticesIsUI = True: sky_ui_hud's ortho
HUD keeps z-writes on, which Remix's orthographic rule (rtx.orthographicIsUI) does not count as UI, so with the
defaults its injection point is Present; with the option its first POSITIONT draw (the crosshair) is the first UI
draw, and the injection happens mid-frame, before it.
  validation   the host validation layer with synchronization validation (injected into the host loader, as
               rl_tap_run.py's validation): the capture tap alone vs the capture tap with frame passthrough + swap,
               and with frame solid, in DXVK's legacy binding model (gate: no message id's count grows) and its
               default descriptor-buffer model (gate: no new message id).

  adopt        RL-4.1 renderer adoption inside d3d9.dll: (1) the DLL links fuse_rhi without volk's static
               auto-initialisation (nm: volkInitializeCustom present, fuse_rhi_volk_auto_init absent); (2) loader_probe.exe:
               no vulkan-1.dll / winevulkan.dll mapped after LoadLibrary(d3d9.dll) returned (DllMain done), one mapped
               after Direct3DCreate9 (first use); (3) an app with the frame passthrough: the header shows volk loaded
               from DXVK's vkGetInstanceProcAddr (not before the first attach, no auto-init), the device adopted with
               GPU bindless descriptors and GPU scene tables; every frame registers DXVK images in the renderer's heap
               (bindless.gpu), feeds the GPU scene at the injection point with that frame's draws (scene.feed = inject,
               gpu_frame = frame) and submits the scene's uploads under the host's queue lock.

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb, or no validation layer for 'validation').
"""
import argparse
import json
import os
import shutil
import sys

SKIP = 77
SOLID = 0x2050d0
W, H = 128, 96

CAPTURE = {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "capture"}
FRAME_PASSTHROUGH = dict(CAPTURE, FUSE_RELIGHT_FRAME_MODE="passthrough", FUSE_RELIGHT_FRAME_TEXTURE_SWAP="1",
                         FUSE_RELIGHT_FRAME_STATS="relight_frame.jsonl")
FRAME_SOLID = dict(CAPTURE, FUSE_RELIGHT_FRAME_MODE="solid", FUSE_RELIGHT_FRAME_SOLID_COLOR="%06x" % SOLID,
                   FUSE_RELIGHT_FRAME_STATS="relight_frame.jsonl")


def rtx_conf(out_dir):
    """An rtx.conf for the run (Wine path) that makes pre-transformed (POSITIONT) draws UI."""
    path = os.path.join(os.path.abspath(out_dir), "frame.rtx.conf")
    with open(path, "w") as f:
        f.write("rtx.preTransformedVerticesIsUI = True\n")
    return "Z:" + path.replace("/", "\\")


def load_tap_tools(path):
    sys.path.insert(0, path)
    import rl_tap_run  # noqa: E402
    return rl_tap_run


def frame_records(run_dir):
    path = os.path.join(run_dir, "relight_frame.jsonl")
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def run(tools, args, exe, run_dir, env):
    rc, text = tools.run_app(args, exe, run_dir, env)
    if rc != 0 and rc != SKIP:
        print(text.strip()[-4000:])
    return rc, text


def rgba(path):
    with open(path, "rb") as f:
        return f.read()


def pixel(data, x, y):
    o = (y * W + x) * 4
    return data[o], data[o + 1], data[o + 2]


def cmd_passthrough(tools, args):
    exe, app = args.exe[0], args.app
    ref_dir = os.path.join(args.out, "reference_relight_off")
    rc, text = run(tools, args, exe, ref_dir, {"FUSE_RELIGHT": "0"})
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: reference run exited with {rc}")
        return 1
    ref = tools.dumps(ref_dir)
    run_dir = os.path.join(args.out, "frame_passthrough")
    rc, _ = run(tools, args, exe, run_dir, dict(FRAME_PASSTHROUGH, DXVK_RTX_CONFIG_FILE=rtx_conf(args.out)))
    if rc != 0:
        print(f"FAIL: {app}: frame passthrough run exited with {rc}")
        return 1
    failed = False
    got = tools.dumps(run_dir)
    if got != ref or not ref:
        print(f"FAIL: {app}: dumps {got} != reference {ref}")
        return 1
    for d in ref:
        a, b = tools.sha(os.path.join(ref_dir, d)), tools.sha(os.path.join(run_dir, d))
        if a != b:
            print(f"FAIL: {app}: {d}: passthrough composite + texture swap differs from FUSE_RELIGHT=0 "
                  f"({b[:16]} != {a[:16]})")
            failed = True
    recs = frame_records(run_dir)
    if recs is None:
        print(f"FAIL: {app}: no frame record (relight_frame.jsonl): the frame tap did not attach")
        return 1
    frames = [r for r in recs if r.get("ev") == "frame"]
    injected = [r for r in frames if r["inject"] in ("ui", "present")]
    if not frames or len(injected) != len(frames):
        bad = [(r["frame"], r["inject"], r["error"]) for r in frames if r not in injected]
        print(f"FAIL: {app}: {len(injected)} of {len(frames)} frame(s) injected: {bad[:5]}")
        failed = True
    if injected and any(b["acquire"] <= a["acquire"] or b["release"] <= a["release"]
                        for a, b in zip(injected, injected[1:])):
        print(f"FAIL: {app}: timeline values do not advance")
        failed = True
    swaps = max((r["swaps"] for r in frames), default=0)
    ui = sum(1 for r in injected if r["inject"] == "ui")
    external = max((r["bindless"]["external"] for r in frames), default=0)
    instances = max((r["scene"]["instances"] for r in frames), default=0)
    if failed:
        return 1
    print(f"PASS: {app}: {len(ref)} dump(s) bit-identical to FUSE_RELIGHT=0 with the passthrough composite in "
          f"{len(injected)} frame(s) ({ui} at the first UI draw) and {swaps} passthrough texture swap(s); "
          f"bindless: {external} DXVK image(s) registered; GPU scene: {instances} instance(s)")
    return 0


# HUD probes of sky_ui_hud (its last frame) drawn after the UI injection point (the POSITIONT crosshair).
HUD_PROBES = [(64, 48, "POSITIONT crosshair"), (6, 6, "POSITIONT text block")]
# Pixels drawn before the injection point: sky, world, and the ortho HUD bar (z-writes on: not UI for Remix).
SCENE_PROBES = [(100, 40), (30, 60), (110, 20), (20, 88)]


def cmd_solid(tools, args):
    exe, app = args.exe[0], args.app
    ref_dir = os.path.join(args.out, "reference_relight_off")
    rc, text = run(tools, args, exe, ref_dir, {"FUSE_RELIGHT": "0"})
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: reference run exited with {rc}")
        return 1
    run_dir = os.path.join(args.out, "frame_solid")
    rc, _ = run(tools, args, exe, run_dir, dict(FRAME_SOLID, DXVK_RTX_CONFIG_FILE=rtx_conf(args.out)))
    if rc != 0:
        print(f"FAIL: {app}: frame solid run exited with {rc}")
        return 1
    ref = rgba(os.path.join(ref_dir, app + ".rgba"))
    got = rgba(os.path.join(run_dir, app + ".rgba"))
    solid = ((SOLID >> 16) & 0xff, (SOLID >> 8) & 0xff, SOLID & 0xff)
    failed = False
    for x, y, what in HUD_PROBES:
        if pixel(got, x, y) != pixel(ref, x, y):
            print(f"FAIL: {app}: HUD pixel {what} ({x},{y}) = {pixel(got, x, y)}, reference {pixel(ref, x, y)}")
            failed = True
    for x, y in SCENE_PROBES:
        if pixel(got, x, y) != solid:
            print(f"FAIL: {app}: scene pixel ({x},{y}) = {pixel(got, x, y)}, expected FUSE's {solid}")
            failed = True
        if pixel(ref, x, y) == solid:
            print(f"FAIL: {app}: reference pixel ({x},{y}) already has FUSE's colour (test is blind)")
            failed = True
    recs = frame_records(run_dir) or []
    frames = [r for r in recs if r.get("ev") == "frame"]
    ui = [r for r in frames if r["inject"] == "ui"]
    if not frames or len(ui) != len(frames):
        print(f"FAIL: {app}: {len(ui)} of {len(frames)} frame(s) injected at the first UI draw: "
              f"{[(r['frame'], r['inject'], r['error']) for r in frames][:5]}")
        failed = True
    covered = sum(1 for i in range(0, len(got), 4) if (got[i], got[i + 1], got[i + 2]) == solid)
    if failed:
        return 1
    print(f"PASS: {app}: FUSE's solid image composited at the first UI draw (draw {ui[-1]['inject_draw']}) in "
          f"{len(ui)} frame(s); {covered} of {W * H} pixels FUSE's colour, the HUD on top as without FUSE")
    return 0


def cmd_validation(tools, args):
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
    conf = rtx_conf(args.out)
    # DXVK's legacy binding model (per-id counts are deterministic there: the gate is "no count grows") and its
    # default descriptor-buffer model (the layer's duplicate limit makes counts batching-dependent, and FUSE's
    # mid-frame flush changes the batching: the gate is "no new message id"), as rl_tap_run.py's validation.
    legacy = {"DXVK_CONFIG": "dxvk.enableDescriptorBuffer = False"}
    runs = []
    for model, extra in (("legacy", legacy), ("default", {})):
        runs += [(model + "_capture", dict(CAPTURE, DXVK_RTX_CONFIG_FILE=conf, **extra)),
                 (model + "_frame_passthrough", dict(FRAME_PASSTHROUGH, DXVK_RTX_CONFIG_FILE=conf, **extra)),
                 (model + "_frame_solid", dict(FRAME_SOLID, DXVK_RTX_CONFIG_FILE=conf, **extra))]
    failed = False
    lines = []
    for exe in args.exe:
        app = tools.app_name_of(exe)
        result = {}
        for name, env in runs:
            e = dict(common)
            e.update(env)
            rc, text = run(tools, args, exe, os.path.join(args.out, app, name), e)
            if rc == SKIP:
                print(text.strip())
                return SKIP
            if rc != 0:
                print(f"FAIL: {app} ({name}): exited with {rc}")
                failed = True
                break
            result[name] = tools.validation_messages(text)
            if "frame" in name:
                recs = frame_records(os.path.join(args.out, app, name)) or []
                frames = [r for r in recs if r.get("ev") == "frame"]
                if not frames or any(r["inject"] not in ("ui", "present") for r in frames):
                    print(f"FAIL: {app} ({name}): not every frame injected")
                    failed = True
        if len(result) != len(runs):
            continue
        for mode in ("frame_passthrough", "frame_solid"):
            grown = result["legacy_" + mode][1] - result["legacy_capture"][1]
            if grown:
                failed = True
                print(f"FAIL: {app}: {mode} adds validation messages (legacy binding model): {dict(grown)}")
            new_ids = set(result["default_" + mode][1]) - set(result["default_capture"][1])
            if new_ids:
                failed = True
                print(f"FAIL: {app}: {mode} adds validation message ids (descriptor buffers): {sorted(new_ids)}")
        lines.append(f"{app}: messages capture/passthrough/solid = legacy "
                     f"{result['legacy_capture'][0]}/{result['legacy_frame_passthrough'][0]}/"
                     f"{result['legacy_frame_solid'][0]}, default {result['default_capture'][0]}/"
                     f"{result['default_frame_passthrough'][0]}/{result['default_frame_solid'][0]}; DXVK's own ids: "
                     f"{', '.join(sorted(set(result['legacy_capture'][1]) | set(result['default_capture'][1]))) or 'none'}")
    for l in lines:
        print("  " + l)
    if failed:
        return 1
    print(f"PASS: {len(args.exe)} app(s): the frame modes (injection, composite, timeline sync, texture swap) add no "
          f"validation or synchronization-validation message (legacy binding model: per-id counts; descriptor "
          f"buffers: ids) over the capture tap alone (host layer {layer})")
    return 0


def cmd_adopt(tools, args):
    import subprocess
    failed = False
    # (1) the DLL: fuse_rhi linked, the auto-init translation unit not.
    if args.nm:
        syms = subprocess.run([args.nm, args.d3d9], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode(
            errors="replace")
        if "volkInitializeCustom" not in syms:
            print("FAIL: d3d9.dll does not link fuse_rhi's volk (volkInitializeCustom missing)")
            failed = True
        if "fuse_rhi_volk_auto_init" in syms:
            print("FAIL: d3d9.dll links fuse_rhi's volk auto-initialisation (FUSE_RHI_VOLK_NO_AUTO_INIT not applied)")
            failed = True
    # (2) DllMain: no loader mapped by LoadLibrary(d3d9.dll).
    probe_dir = os.path.join(args.out, "loader_probe")
    rc, text = run(tools, args, args.probe, probe_dir, dict(FRAME_PASSTHROUGH))
    if rc == SKIP:
        print(text.strip())
        return SKIP
    line = next((l for l in text.splitlines() if l.startswith("probe ")), "")
    if rc != 0 or not line:
        print(f"FAIL: loader probe exited with {rc}: {line or text.strip()[-2000:]}")
        failed = True
    # (3) in-process adoption.
    exe, app = args.exe[0], args.app
    run_dir = os.path.join(args.out, "frame_passthrough")
    rc, _ = run(tools, args, exe, run_dir, dict(FRAME_PASSTHROUGH, DXVK_RTX_CONFIG_FILE=rtx_conf(args.out)))
    if rc != 0:
        print(f"FAIL: {app}: frame passthrough run exited with {rc}")
        return 1
    recs = frame_records(run_dir) or []
    header = next((r for r in recs if r.get("ev") == "header"), None)
    frames = [r for r in recs if r.get("ev") == "frame"]
    if not header or not frames:
        print(f"FAIL: {app}: no frame record header / frames")
        return 1
    ld, rd = header.get("loader", {}), header.get("renderer", {})
    for key, want in (("auto_init", False), ("loaded_before_attach", False), ("proc_addr", True), ("loaded", True)):
        if ld.get(key) != want:
            print(f"FAIL: {app}: loader.{key} = {ld.get(key)}, expected {want}")
            failed = True
    for key in ("adopted", "gpu_descriptors", "gpu_scene"):
        if rd.get(key) is not True:
            print(f"FAIL: {app}: renderer.{key} = {rd.get(key)} ({rd.get('error')})")
            failed = True
    for r in frames:
        f = r["frame"]
        if not r["bindless"]["gpu"] or r["bindless"]["external"] == 0:
            print(f"FAIL: {app}: frame {f}: DXVK images not in the renderer's GPU heap: {r['bindless']}")
            failed = True
        sc = r["scene"]
        if sc["feed"] != "inject" or sc["gpu_frame"] != f or not sc["sink"] or sc["gpu_instances"] == 0:
            print(f"FAIL: {app}: frame {f}: GPU scene not fed with this frame at the injection point: {sc}")
            failed = True
        if not r["renderer"]["adopted"] or r["renderer"]["queue_locks"] == 0:
            print(f"FAIL: {app}: frame {f}: renderer submissions outside the host's queue lock: {r['renderer']}")
            failed = True
    if failed:
        return 1
    last = frames[-1]
    print(f"PASS: {app}: {line.strip()}; volk from DXVK's vkGetInstanceProcAddr at the first attach (no auto-init); "
          f"device adopted ({rd.get('device')}, tier T{rd.get('tier')}), bindless {rd.get('bindless')} with "
          f"{last['bindless']['external']} DXVK image(s); GPU scene fed at the injection point in {len(frames)} "
          f"frame(s) ({last['scene']['gpu_instances']} instance(s)), {last['renderer']['upload_batches']} upload "
          f"batch(es) under {last['renderer']['queue_locks']} queue lock(s)")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--tap-tools", required=True)
    p.add_argument("--exe", action="append", required=True)
    p.add_argument("--app", required=True)
    p.add_argument("--d3d9", required=True)
    p.add_argument("--d3d8", required=True)
    p.add_argument("--runner", required=True)
    p.add_argument("--prefix-root", required=True)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("passthrough", "solid", "validation", "adopt"):
        s = sub.add_parser(name)
        s.add_argument("--out", required=True)
        if name == "adopt":
            s.add_argument("--probe", required=True)
            s.add_argument("--nm", default="")
    args = p.parse_args()
    if not shutil.which("wine") and not shutil.which("wine64"):
        print("SKIP: wine not installed")
        return SKIP
    tools = load_tap_tools(args.tap_tools)
    os.makedirs(args.out, exist_ok=True)
    return {"passthrough": cmd_passthrough, "solid": cmd_solid, "validation": cmd_validation,
            "adopt": cmd_adopt}[args.cmd](tools, args)


if __name__ == "__main__":
    sys.exit(main())
