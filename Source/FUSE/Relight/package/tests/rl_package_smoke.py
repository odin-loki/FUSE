#!/usr/bin/env python3
"""FUSE Relight RL-6.4: Wine smoke test of the packaged layout (rl_package_wine_smoke).

Everything runs from copies of the staged package (<build>/relight/package/FUSE-Relight), laid out the way
docs/relight.md tells users to install it, under cmake/toolchains/fuse-wine-xvfb-run.sh (Wine + Xvfb +
Lavapipe), with the package's own rtx.conf in the game folder and no FUSE_RELIGHT_* configuration in the
environment (only the per-frame stats file is requested, to observe what happened):

  x64       an x64 RL-0.4 app (ff_triangle) in a game folder holding x64/*: the app must exit 0, write its
            dumps, and the frame records must show Relight attached in the packaged rtx.conf's mode
            (relight.frame.mode = raster) with every frame injected;
  plugins   x64/fuse_relight_plugins.exe from the game folder: one line per plugin, all absent (nothing is
            installed), exit 0; --require dlss exits 1; --file-version reads the packaged d3d8.dll;
  bridge    the same x64 app with the x64 bridge client d3d9.dll / d3d8.dll (from the build tree: the package
            carries the i686 client only) plus the packaged x86/fuse_relight/ host folder and x86/rtx.conf: the
            client must find fuse_relight\\fuse_relight_host.exe on its own (no FUSE_RELIGHT_BRIDGE_HOST), the
            host must render every frame (no fallback to in-process DXVK), and the dumps must exist;
  x86       when the package has the x86 bridge and the i686 app exists: the i686 app in a folder holding
            x86/*, same checks as bridge (skipped without wine32).

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb, or no package).
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys

SKIP = 77
FALLBACK_MARK = "continuing on in-process DXVK"


def copy_tree(src, dst):
    for root, _dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            s, d = os.path.join(root, f), os.path.join(dst, rel, f)
            try:
                os.link(s, d)
            except OSError:
                shutil.copy2(s, d)


def fresh(path):
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)
    return path


def wine_run(args, game_dir, exe, exe_args, env_extra):
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("FUSE_RELIGHT") or k.startswith("DXVK_RTX_CONFIG"):
            del env[k]
    env.setdefault("DXVK_LOG_LEVEL", "warn")
    env["DXVK_LOG_PATH"] = "none"
    env.update(env_extra)
    cmd = ["bash", args.runner, "--native-d3d", args.prefix_root, os.path.join(game_dir, exe)] + exe_args
    proc = subprocess.run(cmd, cwd=game_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode(errors="replace")
    with open(os.path.join(game_dir, "run_%s.log" % os.path.splitext(exe)[0]), "w") as f:
        f.write(text)
    return proc.returncode, text


def frame_records(game_dir):
    path = os.path.join(game_dir, "relight_frame.jsonl")
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def check_frames(name, game_dir, errors, require_records=True):
    recs = frame_records(game_dir)
    if recs is None:
        if require_records:
            errors.append(f"{name}: no relight_frame.jsonl: Relight did not attach with the packaged rtx.conf")
        return 0
    frames = [r for r in recs if r.get("ev") == "frame"]
    injected = [r for r in frames if r.get("inject") in ("ui", "present")]
    if not frames or len(injected) != len(frames):
        bad = [(r.get("frame"), r.get("inject"), r.get("error")) for r in frames if r not in injected]
        errors.append(f"{name}: {len(injected)} of {len(frames)} frame(s) injected: {bad[:5]}")
    modes = sorted({str(r.get("mode")) for r in frames if "mode" in r})
    if modes and modes != ["raster"]:
        errors.append(f"{name}: frame mode {modes}, expected the packaged rtx.conf's raster")
    return len(injected)


def app_run(args, name, game_dir, app_exe, errors, env_extra=None):
    exe = os.path.basename(app_exe)
    rc, text = wine_run(args, game_dir, exe, ["--out", ".", "--quiet"],
                        dict({"FUSE_RELIGHT_FRAME_STATS": "relight_frame.jsonl"}, **(env_extra or {})))
    if rc == SKIP:
        return SKIP, text
    if rc != 0:
        errors.append(f"{name}: {exe} exited with {rc} (see {game_dir})")
        print(text.strip()[-3000:])
    if not glob.glob(os.path.join(game_dir, "*.rgba")):
        errors.append(f"{name}: the app wrote no dumps")
    return rc, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pkg", required=True)
    ap.add_argument("--app", required=True)
    ap.add_argument("--x64-client-d3d9", required=True)
    ap.add_argument("--x64-client-d3d8", required=True)
    ap.add_argument("--x86-app", default="")
    ap.add_argument("--runner", required=True)
    ap.add_argument("--prefix-root", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.pkg, "manifest.json")):
        print(f"SKIP: no package at {args.pkg} (build the relight_package target)")
        return SKIP
    with open(os.path.join(args.pkg, "manifest.json")) as f:
        manifest = json.load(f)
    errors = []
    notes = []

    # ---- x64: the packaged x64/ folder next to an x64 game ----
    game = fresh(os.path.join(args.out, "x64_game"))
    copy_tree(os.path.join(args.pkg, "x64"), game)
    shutil.copy2(args.app, game)
    rc, _ = app_run(args, "x64", game, args.app, errors)
    if rc == SKIP:
        print("SKIP: Wine / Xvfb not available")
        return SKIP
    n = check_frames("x64", game, errors)
    notes.append(f"x64: {n} frame(s) injected")

    # ---- plugins: the packaged report tool, nothing installed ----
    rc, text = wine_run(args, game, "fuse_relight_plugins.exe", [], {})
    lines = [l for l in text.splitlines() if l.startswith("fuse-relight plugins: ")]
    if rc != 0 or len(lines) != 5 or not all(": absent: not installed" in l for l in lines):
        errors.append(f"plugins: expected 5 'absent' lines and exit 0, got rc {rc}:\n{text.strip()[-2000:]}")
    if "fuse_relight_plugins" not in text:
        errors.append("plugins: the report does not name the packaged fuse_relight_plugins folder")
    rc, text = wine_run(args, game, "fuse_relight_plugins.exe", ["--require", "dlss"], {})
    if rc != 1:
        errors.append(f"plugins: --require dlss without DLSS must exit 1 (got {rc})")
    rc, text = wine_run(args, game, "fuse_relight_plugins.exe", ["--file-version", "d3d8.dll"], {})
    if rc != 0 or "d3d8.dll: " not in text:
        errors.append(f"plugins: --file-version d3d8.dll failed (rc {rc}): {text.strip()[-500:]}")
    else:
        notes.append("d3d8.dll version " + text.strip().splitlines()[-1].split(": ")[-1])

    # ---- bridge: x64 client + the packaged host folder (the layout of x86/) ----
    game = fresh(os.path.join(args.out, "bridge_game"))
    copy_tree(os.path.join(args.pkg, "x86", "fuse_relight"), os.path.join(game, "fuse_relight"))
    shutil.copy2(os.path.join(args.pkg, "x86", "rtx.conf"), game)
    shutil.copy2(args.x64_client_d3d9, os.path.join(game, "d3d9.dll"))
    shutil.copy2(args.x64_client_d3d8, os.path.join(game, "d3d8.dll"))
    shutil.copy2(args.app, game)
    bridge_env = {"FUSE_RELIGHT_BRIDGE_TIMEOUT_MS": "120000", "FUSE_RELIGHT_BRIDGE_LOG": "bridge.log"}
    rc, text = app_run(args, "bridge", game, args.app, errors, bridge_env)
    blog = ""
    if os.path.isfile(os.path.join(game, "bridge.log")):
        with open(os.path.join(game, "bridge.log"), errors="replace") as f:
            blog = f.read()
    if FALLBACK_MARK in text + blog or "not available" in text + blog:
        errors.append(f"bridge: the client did not run on the packaged host (see {game}):\n{(text + blog).strip()[-2000:]}")
    n = check_frames("bridge", game, errors, require_records=False)
    notes.append(f"bridge: host from fuse_relight\\, {n} frame(s) injected" if n else "bridge: host from fuse_relight\\")

    # ---- x86: the packaged x86/ folder next to the i686 app ----
    if manifest.get("x86_bridge") and args.x86_app and os.path.isfile(args.x86_app):
        game = fresh(os.path.join(args.out, "x86_game"))
        copy_tree(os.path.join(args.pkg, "x86"), game)
        shutil.copy2(args.x86_app, game)
        sub = []
        rc, text = app_run(args, "x86", game, args.x86_app, sub, bridge_env)
        if rc == SKIP:
            notes.append("x86: skipped (no wine32)")
        else:
            errors.extend(sub)
            if FALLBACK_MARK in text:
                errors.append("x86: the bridge fell back to in-process DXVK")
            notes.append("x86: packaged bridge ran")
    else:
        notes.append("x86: not packaged (no i686 tree)" if not manifest.get("x86_bridge") else "x86: no i686 app")

    if errors:
        for e in errors:
            print("FAIL: " + e)
        return 1
    print("PASS: packaged layout runs under Wine; " + "; ".join(notes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
