#!/usr/bin/env python3
"""FUSE Relight RL-2.2: Wine driver for the bridge client tests (rl_bridge_x64_* / rl_bridge_x86_*).

Runs an RL-0.4 test app (Tests/relight/apps) under cmake/toolchains/fuse-wine-xvfb-run.sh twice:

  reference  the x64 app on the vendored DXVK d3d9.dll / d3d8.dll directly (FUSE_RELIGHT=0: DXVK's
             own device, no tap)
  bridged    the app (x64, or the i686 build for --arch x86) with the bridge client d3d9.dll /
             d3d8.dll next to it; the client starts the test-local stub host (x64), which replays
             the command stream on the same vendored DXVK d3d9.dll in its own process

and requires:
  - every dump (<app>.rgba, <app>.<name>.rgba) byte-identical to the reference, and the sidecar
    JSON equal (it holds the app's ground truth and the adapter string it read through the API);
  - the stub host decoded and dispatched every command ("stub host: done (exit 0" in the log);
    with --host-kind real the host is RL-2.3's fuse_relight_host.exe (FUSE_RELIGHT=0 via --relight 0)
    instead, and no fallback may have happened;
  - with --fault crash@N|hang@N (real host): the host fails mid-run, the client's RL-2.3 link falls
    back to in-process DXVK (FUSE_RELIGHT_PASSTHROUGH_D3D9 = the vendored d3d9.dll) and the app still
    completes with identical dumps;
  - for --arch x86: the command stream recorded by the stub host (name, handle, payload size per
    command) equal to the stream of the x64 app through the bridge.

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb / wine32, or no i686 build tree).
"""
import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys

SKIP = 77


def winpath(p):
    return "Z:" + os.path.abspath(p).replace("/", "\\")


def link_into(src, run_dir, name=None):
    dst = os.path.join(run_dir, name or os.path.basename(src))
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def run(args, exe, dlls, run_dir, env_extra):
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    link_into(exe, run_dir)
    for d in dlls:
        link_into(d, run_dir)
    env = dict(os.environ)
    env.setdefault("DXVK_LOG_LEVEL", "warn")
    env["DXVK_LOG_PATH"] = "none"
    env["FUSE_RELIGHT"] = "0"
    env.update(env_extra)
    local_exe = os.path.join(run_dir, os.path.basename(exe))
    cmd = ["bash", args.runner, "--native-d3d", args.prefix_root, local_exe, "--out", ".", "--quiet"]
    proc = subprocess.run(cmd, cwd=run_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode(errors="replace")
    with open(os.path.join(run_dir, "run.log"), "w") as f:
        f.write(text)
    return proc.returncode, text


def dumps(run_dir):
    return sorted(os.path.basename(p) for p in glob.glob(os.path.join(run_dir, "*.rgba")))


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def bridged(args, exe, client_dir_dlls, run_dir):
    env = {
        "FUSE_RELIGHT_BRIDGE_HOST": winpath(args.host),
        "FUSE_RELIGHT_STUB_D3D9": winpath(args.dxvk_d3d9),
        "FUSE_RELIGHT_STUB_RECORD": winpath(os.path.join(run_dir, "stream.txt")),
        "FUSE_RELIGHT_PASSTHROUGH_D3D9": winpath(args.dxvk_d3d9),
        "FUSE_RELIGHT_BRIDGE_TIMEOUT_MS": "8000" if args.fault.startswith("hang") else "120000",
    }
    if args.host_kind == "real":
        env["FUSE_RELIGHT_BRIDGE_HOST_ARGS"] = " ".join(
            ["--d3d9", winpath(args.dxvk_d3d9), "--relight", "0"] + (["--test-fault", args.fault] if args.fault else []))
    return run(args, exe, client_dir_dlls, run_dir, env)


FALLBACK_MARK = "continuing on in-process DXVK"


def check_bridged(app, name, rc, text, run_dir, ref_dir, ref, args):
    errors = []
    if rc != 0:
        print(text.strip()[-4000:])
        return [f"{name}: exited with {rc}"]
    if args.host_kind == "stub" and "stub host: done (exit 0" not in text:
        errors.append(f"{name}: the stub host did not finish cleanly (unhandled or failed commands; see {run_dir}/run.log)")
    fell_back = FALLBACK_MARK in text
    if args.fault and not fell_back:
        errors.append(f"{name}: the host fault {args.fault} did not trigger the passthrough fallback")
    if not args.fault and fell_back:
        errors.append(f"{name}: the client fell back to in-process DXVK (the host failed; see {run_dir}/run.log)")
    got = dumps(run_dir)
    if got != ref:
        errors.append(f"{name}: dumps {got} != reference {ref}")
    else:
        for d in ref:
            a, b = sha(os.path.join(ref_dir, d)), sha(os.path.join(run_dir, d))
            if a != b:
                errors.append(f"{name}: {d} differs from the direct DXVK run ({b[:16]} != {a[:16]})")
    sa, sb = os.path.join(ref_dir, app + ".json"), os.path.join(run_dir, app + ".json")
    if os.path.isfile(sa):
        if not os.path.isfile(sb):
            errors.append(f"{name}: no sidecar {app}.json")
        else:
            with open(sa) as fa, open(sb) as fb:
                ja, jb = json.load(fa), json.load(fb)
            if ja != jb:
                keys = sorted(k for k in set(ja) | set(jb) if ja.get(k) != jb.get(k))
                errors.append(f"{name}: sidecar differs from the direct run in {keys}")
    if errors:
        print(text.strip()[-3000:])
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--arch", choices=["x64", "x86"], required=True)
    ap.add_argument("--x86-dir")
    ap.add_argument("--dxvk-d3d9", required=True)
    ap.add_argument("--dxvk-d3d8", required=True)
    ap.add_argument("--client-d3d9", required=True)
    ap.add_argument("--client-d3d8", required=True)
    ap.add_argument("--host", required=True)
    ap.add_argument("--runner", required=True)
    ap.add_argument("--prefix-root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--host-kind", choices=["stub", "real"], default="stub")
    ap.add_argument("--fault", default="")
    args = ap.parse_args()
    app = args.app

    x86_exe = x86_client = None
    if args.arch == "x86":
        base = os.path.abspath(args.x86_dir or "")
        x86_exe = os.path.join(base, "relight", "apps", os.path.basename(args.exe))
        x86_client = [os.path.join(base, "relight", "bridge", "client", n) for n in ("d3d9.dll", "d3d8.dll")]
        missing = [p for p in [x86_exe] + x86_client if not os.path.isfile(p)]
        if missing:
            print(f"SKIP: no i686 build of {app} / the bridge client ({', '.join(missing)}); build the i686 tree "
                  f"(cmake/toolchains/mingw-w64-i686.cmake) and set FUSE_RELIGHT_BRIDGE_X86_DIR")
            return SKIP

    ref_dir = os.path.join(args.out, "reference_dxvk_direct")
    rc, text = run(args, args.exe, [args.dxvk_d3d9, args.dxvk_d3d8], ref_dir, {})
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(text.strip()[-4000:])
        print(f"FAIL: {app}: reference run on DXVK exited with {rc}")
        return 1
    ref = dumps(ref_dir)
    if not ref:
        print(f"FAIL: {app}: the reference run wrote no .rgba dump")
        return 1

    errors = []
    x64_dir = os.path.join(args.out, "bridged_x64")
    rc, text = bridged(args, args.exe, [args.client_d3d9, args.client_d3d8], x64_dir)
    if rc == SKIP:
        print(text.strip())
        return SKIP
    errors += check_bridged(app, "x64 via bridge", rc, text, x64_dir, ref_dir, ref, args)

    if args.arch == "x86":
        x86_dir = os.path.join(args.out, "bridged_x86")
        rc, text = bridged(args, x86_exe, x86_client, x86_dir)
        if rc == SKIP:
            print(text.strip())
            print(f"SKIP: {app}: the x86 run needs wine32")
            return SKIP
        errors += check_bridged(app, "x86 via bridge", rc, text, x86_dir, ref_dir, ref, args)
        s64, s86 = os.path.join(x64_dir, "stream.txt"), os.path.join(x86_dir, "stream.txt")
        if args.host_kind != "stub":
            pass
        elif os.path.isfile(s64) and os.path.isfile(s86):
            with open(s64) as a, open(s86) as b:
                la, lb = a.read().splitlines(), b.read().splitlines()
            if la != lb:
                first = next((i for i, (p, q) in enumerate(zip(la, lb)) if p != q), min(len(la), len(lb)))
                errors.append(f"x86 command stream differs from x64 at command {first}: "
                              f"{la[first] if first < len(la) else '<end>'} vs {lb[first] if first < len(lb) else '<end>'}"
                              f" ({len(la)} vs {len(lb)} commands)")
        else:
            errors.append("missing stub host command stream(s)")

    if errors:
        for e in errors:
            print(f"FAIL: {app}: {e}")
        return 1
    what = "x64 and x86 builds" if args.arch == "x86" else "x64 build"
    if args.host_kind == "stub":
        with open(os.path.join(x64_dir, "stream.txt")) as f:
            n = len(f.read().splitlines())
        detail = (f"{n} commands decoded and dispatched by the stub host"
                  f"{', x86 command stream identical to x64' if args.arch == 'x86' else ''}")
    else:
        detail = "RL-2.3 host" + (f", host fault {args.fault} -> in-process DXVK fallback" if args.fault else "")
    print(f"PASS: {app}: {len(ref)} dump(s) + sidecar identical to DXVK direct for the {what} through the bridge "
          f"({detail})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
