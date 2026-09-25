#!/usr/bin/env python3
"""FUSE Relight RL-6.2: runs the remixapi_c app (Tests/relight/apps/remixapi_c) through Relight's d3d9.dll.

  rl_api_app_run.py --exe remixapi_c.exe --d3d9 d3d9.dll --runner fuse-wine-xvfb-run.sh --prefix-root DIR --out DIR

The app and d3d9.dll are copied into <out>/run, run under Wine + Xvfb + Lavapipe with the native d3d9 override, and
its remixapi_c.json (every check of the app, the last frame record) must say ok. Exit codes: 0 pass, 1 fail, 77 skip
(no Wine / Xvfb, or no Vulkan device).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

SKIP = 77


def main():
    ap = argparse.ArgumentParser()
    for opt in ("exe", "d3d9", "runner", "prefix_root", "out"):
        ap.add_argument("--" + opt.replace("_", "-"), required=True)
    args = ap.parse_args()
    run_dir = os.path.join(args.out, "run")
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    for src in (args.exe, args.d3d9):
        shutil.copy2(src, os.path.join(run_dir, os.path.basename(src)))
    env = dict(os.environ)
    env.setdefault("DXVK_LOG_LEVEL", "warn")
    env["DXVK_LOG_PATH"] = "none"
    cmd = ["bash", args.runner, "--native-d3d", args.prefix_root, os.path.join(run_dir, os.path.basename(args.exe))]
    proc = subprocess.run(cmd, cwd=run_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
    text = proc.stdout.decode(errors="replace")
    with open(os.path.join(run_dir, "run.log"), "w") as f:
        f.write(text)
    print(text.strip()[-4000:])
    if proc.returncode == SKIP:
        return SKIP
    record = os.path.join(run_dir, "remixapi_c.json")
    if not os.path.isfile(record):
        print("FAIL: remixapi_c: no remixapi_c.json (exit %d)" % proc.returncode)
        return 1
    with open(record) as f:
        result = json.load(f)
    failed = [c["check"] for c in result.get("checks", []) if not c.get("ok")]
    if proc.returncode != 0 or not result.get("ok") or failed:
        print("FAIL: remixapi_c (exit %d): %s" % (proc.returncode, "; ".join(failed) or "not ok"))
        return 1
    print("PASS: remixapi_c: %d checks, last record %s" % (len(result["checks"]), json.dumps(result["last_record"])))
    shutil.rmtree(run_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
