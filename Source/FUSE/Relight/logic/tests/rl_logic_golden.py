#!/usr/bin/env python3
"""FUSE Relight RL-3.5: a logic graph in a fixture mod drives the replacement engine of an RL-0.4 app under Wine.

One case = Tests/relight/fixtures/logic/golden/<case>.json:
  {"app": "ff_multi_instance",
   "mods": [{"fixture": "multi_logic", "root": "remix"}],        (Tests/relight/fixtures/logic/mods/<fixture>)
   "env": {"FUSE_RELIGHT_LOGIC_FIXED_DELTA_TIME": "0.0625"},    (optional)
   "switch": {"stat": "material_replaced", "before": 6, "after": 0, "frame": 20}}   (optional)

1. Copies the fixture mods (text only) into the run directory's search roots (rtx-remix/mods/<name> for Remix mods,
   fuse-relight/mods/<name> for FUSE-native ones: the defaults of relight.replace.modPaths).
2. Runs the app through the Relight d3d9.dll / d3d8.dll under Wine + Xvfb (tests/tap/rl_tap_run.py's runner) with
   relight.tap.mode = capture: the replacement engine (RL-3.4) writes "replace_frame" lines and the logic runtime
   (logic/logic_live.hpp) adds a "logic" member to each of them.
3. Summarises the record (runs of identical replacement stats; runs of identical logic state: instances, owners,
   batches, added / removed, the option layers the graphs hold; the first instance's outputs at the first, the
   switch and the last frames) and compares it with <case>.expected.json (--update writes it). With "switch", the
   stat must hold `before` on every frame before `frame` and `after` from `frame` on: the graph's option layer is
   observable in the capture record exactly one frame after the graph requested it.

  rl_logic_golden.py --case CASE.json --fixtures DIR --exe app.exe --d3d9 d3d9.dll --d3d8 d3d8.dll --runner run.sh
      --prefix-root DIR --out DIR [--keep] [--update]
The run directory is removed on a pass unless --keep. Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
SKIP = 77
ROOTS = {"remix": os.path.join("rtx-remix", "mods"), "fuse": os.path.join("fuse-relight", "mods")}


def link_or_copy(src, dst):
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def run_app(args, case, run_dir):
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    for src in (args.exe, args.d3d9, args.d3d8):
        link_or_copy(src, os.path.join(run_dir, os.path.basename(src)))
    for m in case["mods"]:
        src = os.path.join(args.fixtures, "mods", m["fixture"])
        if not os.path.isdir(src):
            raise RuntimeError("no fixture mod %s" % src)
        shutil.copytree(src, os.path.join(run_dir, ROOTS[m.get("root", "remix")], m.get("name", m["fixture"])))
    env = dict(os.environ)
    env.setdefault("DXVK_LOG_LEVEL", "warn")
    env["DXVK_LOG_PATH"] = "none"
    env.update({"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "capture",
                "FUSE_RELIGHT_TAP_CAPTURE_PATH": "relight_capture.jsonl"})
    env.update(case.get("env", {}))
    cmd = ["bash", args.runner, "--native-d3d", args.prefix_root, os.path.join(run_dir, os.path.basename(args.exe)),
           "--out", ".", "--quiet"]
    proc = subprocess.run(cmd, cwd=run_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode(errors="replace")
    with open(os.path.join(run_dir, "run.log"), "w") as f:
        f.write(text)
    return proc.returncode, text


def runs_of(frames, key):
    runs = []
    for fr in frames:
        value = key(fr)
        if runs and runs[-1]["value"] == value and runs[-1]["last"] + 1 == fr["frame"]:
            runs[-1]["last"] = fr["frame"]
        else:
            runs.append({"first": fr["frame"], "last": fr["frame"], "value": value})
    return runs


def summarize(capture_path):
    lines = []
    with open(capture_path) as f:
        for l in f:
            if l.strip():
                lines.append(json.loads(l))
    frames = [l for l in lines if l["ev"] == "replace_frame"]
    if not frames:
        return None, frames
    logic_frames = [fr for fr in frames if "logic" in fr]

    def logic_state(fr):
        lg = fr["logic"]
        return {k: lg[k] for k in ("enabled", "paused", "graphs", "owners", "instances", "batches", "added", "removed", "layers")}

    def first_outputs(fr):
        values = fr["logic"]["values"]
        return values[0]["outputs"] if values else {}

    picked = {}
    if logic_frames:
        for fr in (logic_frames[0], logic_frames[-1]):
            picked[str(fr["frame"])] = first_outputs(fr)
    summary = {
        "frames": len(frames),
        "mods": [{"name": m["name"], "ok": m["ok"], "errors": m["errors"]} for m in frames[-1]["mods"]],
        "stats_runs": [{"first": r["first"], "last": r["last"], "stats": r["value"]}
                       for r in runs_of(frames, lambda fr: fr["stats"])],
        "logic_frames": len(logic_frames),
        "logic_runs": [{"first": r["first"], "last": r["last"], "state": r["value"]} for r in runs_of(logic_frames, logic_state)],
        "first_instance_outputs": picked,
    }
    return summary, frames


def diff(a, b, path="", out=None):
    out = [] if out is None else out
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                out.append("%s.%s: %s" % (path, k, "missing from the run" if k not in a else "not expected"))
            else:
                diff(a[k], b[k], path + "." + k, out)
    elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b)):
            diff(x, y, "%s[%d]" % (path, i), out)
    elif a != b:
        out.append("%s: run %s, expected %s" % (path, json.dumps(a)[:300], json.dumps(b)[:300]))
    return out


def main():
    ap = argparse.ArgumentParser()
    for opt in ("case", "fixtures", "exe", "d3d9", "d3d8", "runner", "prefix_root", "out"):
        ap.add_argument("--" + opt.replace("_", "-"), required=True)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--update", action="store_true")
    args = ap.parse_args()
    with open(args.case) as f:
        case = json.load(f)
    name = os.path.splitext(os.path.basename(args.case))[0]
    run_dir = os.path.join(args.out, "run")
    try:
        rc, text = run_app(args, case, run_dir)
    except RuntimeError as e:
        print("FAIL: %s: %s" % (name, e))
        return 1
    if rc == SKIP:
        print(text.strip())
        shutil.rmtree(args.out, ignore_errors=True)
        return SKIP
    if rc != 0:
        print(text.strip()[-4000:])
        print("FAIL: %s: the app exited with %d" % (name, rc))
        return 1
    capture = os.path.join(run_dir, "relight_capture.jsonl")
    if not os.path.isfile(capture):
        print("FAIL: %s: no capture record" % name)
        return 1
    summary, frames = summarize(capture)
    if summary is None:
        print(text.strip()[-2000:])
        print("FAIL: %s: the capture record has no replace_frame line (no replacement engine?)" % name)
        return 1
    errors = []
    if not summary["logic_frames"]:
        errors.append("no replace_frame line has a \"logic\" member (no logic runtime or no graphs loaded; see run.log)")
    for m in summary["mods"]:
        if m["errors"]:
            errors.append("mod %s reports %d error(s) (see run.log)" % (m["name"], m["errors"]))
    sw = case.get("switch")
    if sw:
        for fr in frames:
            want = sw["before"] if fr["frame"] < sw["frame"] else sw["after"]
            if fr["stats"][sw["stat"]] != want:
                errors.append("frame %d: %s = %d, expected %d (the graph's option layer switches it at frame %d)"
                              % (fr["frame"], sw["stat"], fr["stats"][sw["stat"]], want, sw["frame"]))
    expected_path = os.path.join(os.path.dirname(args.case), name + ".expected.json")
    if args.update:
        with open(expected_path, "w") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
            f.write("\n")
        print("updated %s" % expected_path)
    elif not os.path.isfile(expected_path):
        errors.append("no %s (run with --update to write it)" % expected_path)
    else:
        with open(expected_path) as f:
            expected = json.load(f)
        errors += diff(summary, expected)
    if errors:
        print("FAIL: %s (%s): %d difference(s):" % (name, case["app"], len(errors)))
        for e in errors[:60]:
            print("  " + e)
        return 1
    last = summary["logic_runs"][-1]["state"] if summary["logic_runs"] else {}
    print("PASS: %s (%s): %d frame(s), %d with logic; last: %d graph instance(s), %d layer(s) held%s"
          % (name, case["app"], summary["frames"], summary["logic_frames"], last.get("instances", 0), len(last.get("layers", [])),
             ("; %s %d -> %d at frame %d" % (sw["stat"], sw["before"], sw["after"], sw["frame"])) if sw else ""))
    if not args.keep:
        shutil.rmtree(args.out, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
