#!/usr/bin/env python3
"""FUSE Relight RL-1.1: the live in-process capture (relight.tap.mode = capture) equals the replay path.

1. Runs an RL-0.4 app under Wine through the Relight d3d9.dll / d3d8.dll (the RL-1.1 driver
   tests/tap/rl_tap_run.run_app) with FUSE_RELIGHT_TAP_MODE=capture and
   FUSE_RELIGHT_TAP_CAPTURE_RECORD=1: d3d9.dll captures live (CaptureTap: TextureTracker,
   GeometryCapture, the classifier) and writes relight_capture.jsonl, and the recording tap writes
   the event stream of the same run to relight_tap.jsonl.
2. Replays that stream through the existing replay tools (the oracles), exactly as their own tests
   do: geometry_replay on rl_capture_geometry.convert_stream (buffer / UP bytes restored from the
   sidecar blobs by SHA-256), the TextureTracker replay (fuse_relight_capture_texture_tests --replay)
   on rl_capture_texture_run.build_script, and rl_classify_replay with the texture replay's hashes.
3. Compares, on every draw: the geometry fields (status, texcoord index / stage, the 9 hash
   components, counts, min / max, topology, index type, position stride, asset key, legacy keys,
   memoization, AABB, skinning), the classification (every rl_classify_replay field), and the bound
   textures' hashes; per frame, every live texture's hashes, origin and state (texture replay
   snapshot of that frame, for the frames the replay script snapshots before its synthetic
   flush-all of never-drawn managed textures). Draws whose bytes the sidecar cannot restore are compared on everything
   but geometry and counted.

  rl_capture_live.py --exe app.exe --app ff_lit --d3d9 d3d9.dll --d3d8 d3d8.dll --runner run.sh
      --prefix-root DIR --geometry-replay geometry_replay.exe --texture-replay tests.exe
      --classify-replay rl_classify_replay.exe [--emulator 'runner|prefix'] --out DIR
  rl_capture_live.py --from-run DIR ...   (reuse a run directory: relight_tap.jsonl, relight_capture.jsonl,
                                           <app>.json)
Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb).
"""
import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys

SKIP = 77
HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.normpath(os.path.join(HERE, ".."))
sys.dont_write_bytecode = True  # no __pycache__ in the other packages' source directories


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


GEO = load_module("rl_capture_geometry", os.path.join(TESTS, "capture_geometry", "rl_capture_geometry.py"))
TEX = load_module("rl_capture_texture_run", os.path.join(TESTS, "capture_texture", "rl_capture_texture_run.py"))
TAP = load_module("rl_tap_run", os.path.join(TESTS, "tap", "rl_tap_run.py"))

GEOMETRY_FIELDS = ("status", "tci", "stage", "f", "ic", "vc", "min", "max", "topo", "it", "ps", "key", "leg0", "leg1",
                   "memo", "aabb", "skin")
CLASSIFY_FIELDS = ("draw_call_id", "status", "reason", "inject", "categories", "decision", "sky_auto", "using_rt_rt",
                   "drawing_to_rt_rt", "color_texture")
TEXTURE_FIELDS = ("hash", "desc", "origin", "from", "pending", "obsolete", "preview", "registered")


def read_jsonl(path):
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


def emulator(args):
    return [p for p in args.emulator.split("|") if p] if args.emulator else []


def run_tool(cmd, cwd=None):
    proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc.returncode, proc.stdout.decode(errors="replace"), proc.stderr.decode(errors="replace")


# ---- oracles ------------------------------------------------------------------------------------------

def geometry_oracle(args, lines, sidecar, work):
    """{n: replay kv}, {n: resolved} from geometry_replay with the default options (as the live capture)."""
    script, draws = GEO.convert_stream(lines, GEO.Blobs(sidecar))
    path = os.path.join(work, "geometry_replay.txt")
    with open(path, "w") as f:
        f.write("\n".join(script) + "\n")
    got, _ = GEO.run_replay(emulator(args) + [args.geometry_replay], path, [])
    return got, {d["n"]: d["resolved"] for d in draws}


def texture_oracle(args, stream, sidecar, work):
    """({label: {id: kv}}, script errors) from the TextureTracker replay."""
    script = TEX.build_script(stream, sidecar)
    with open(os.path.join(work, "replay.txt"), "w") as f:
        f.write("\n".join(script.lines) + "\n")
    rc, out, err = run_tool(emulator(args) + [os.path.abspath(args.texture_replay), "--replay", "replay.txt"], cwd=work)
    with open(os.path.join(work, "texture_replay.log"), "w") as f:
        f.write(out + err)
    if rc != 0:
        raise RuntimeError("texture replay exited with %d: %s" % (rc, (out + err)[-2000:]))
    snaps, _ = TEX.parse_output(out)
    # build_script flushes every pending managed texture ("flushall") at the first image_destroy,
    # to snapshot the never-drawn ones; the live capture does not (Remix hashes a managed texture at
    # its first sampling draw only). Frame snapshots after that synthetic flush are not comparable.
    flush_at = script.lines.index("flushall") if "flushall" in script.lines else len(script.lines)
    comparable = {l.split()[1] for l in script.lines[:flush_at] if l.startswith("snapshot frame")}
    snaps = {k: v for k, v in snaps.items() if not k.startswith("frame") or k in comparable}
    return snaps, script.errors


def classify_oracle(args, stream_path, snaps, work):
    """Classified draws (rl_classify_replay) with the texture replay's hashes (first hash per texture,
    in snapshot order: the frames, then before any destruction; never the final flush-all)."""
    hashes = {}
    labels = sorted((l for l in snaps if l.startswith("frame")), key=lambda l: int(l[5:])) + ["before_destroy"]
    for label in labels:
        for tid, kv in snaps.get(label, {}).items():
            if tid not in hashes and int(kv["hash"], 16) != 0:
                hashes[tid] = int(kv["hash"], 16)
    hashes_path = os.path.join(work, "texture_hashes.txt")
    with open(hashes_path, "w") as f:
        for tid, h in sorted(hashes.items()):
            f.write("%d 0x%016x\n" % (tid, h))
    out_path = os.path.join(work, "classified.jsonl")
    cmd = emulator(args) + [args.classify_replay, "--stream", stream_path, "--texture-hashes", hashes_path,
                            "--out", out_path]
    rc, out, err = run_tool(cmd)
    if rc != 0 or not os.path.isfile(out_path):
        raise RuntimeError("rl_classify_replay exited with %d: %s" % (rc, (out + err)[-2000:]))
    return read_jsonl(out_path)


# ---- comparison ---------------------------------------------------------------------------------------

def jstr(v):
    return ("true" if v else "false") if isinstance(v, bool) else str(v)


def compare(capture, geo, resolved, snaps, tex_errors, classified):
    errors = []
    draws = [r for r in capture if r["ev"] == "draw"]
    frames = {r["frame"]: r for r in capture if r["ev"] == "textures"}
    counts = dict(draws=len(draws), geometry=0, captured=0, unresolved=0, classified=0, textures=0, frames=0,
                  texture_entries=0)
    if not draws:
        errors.append("the live capture recorded no draw")
    if len(classified) != len(draws):
        errors.append("live capture has %d draws, rl_classify_replay %d" % (len(draws), len(classified)))
    if len(geo) != len(draws):
        errors.append("live capture has %d draws, geometry_replay %d" % (len(draws), len(geo)))
    for i, d in enumerate(draws):
        n = d["n"]
        where = "draw %d (frame %d, #%d)" % (n, d["frame"], d["di"])
        if n != i:
            errors.append("%s: out of order (position %d)" % (where, i))
        # geometry
        g = geo.get(n)
        live = d.get("geometry") or {}
        if g is None:
            errors.append("%s: missing from geometry_replay" % where)
        elif not resolved.get(n, False):
            counts["unresolved"] += 1  # the sidecar cannot restore the bytes: the replay has no oracle
        else:
            for k in ("frame", "di"):
                if g.get(k) != str(d[k]):
                    errors.append("%s: geometry %s: replay %s, live %s" % (where, k, g.get(k), d[k]))
            for k in GEOMETRY_FIELDS:
                if g.get(k) != live.get(k):
                    errors.append("%s: geometry %s: replay %s, live %s" % (where, k, g.get(k), live.get(k)))
            counts["geometry"] += 1
            counts["captured"] += 1 if live.get("status") == "captured" else 0
        # classification
        if i < len(classified):
            c, lc = classified[i], d["classification"]
            if (c["frame"], c["index"]) != (d["frame"], d["di"]):
                errors.append("%s: classify replay draw is frame %d #%d" % (where, c["frame"], c["index"]))
            for k in CLASSIFY_FIELDS:
                if jstr(c.get(k)) != jstr(lc.get(k)):
                    errors.append("%s: classification %s: replay %s, live %s" % (where, k, c.get(k), lc.get(k)))
            counts["classified"] += 1
        # bound textures: the texture replay's hash of that texture at the end of the frame
        snap = snaps.get("frame%d" % d["frame"], {})
        for t in d["textures"]:
            kv = snap.get(t["texture"])
            if kv is None:
                continue
            counts["textures"] += 1
            if kv["hash"] != t["hash"] or kv["desc"] != t["desc"]:
                errors.append("%s: slot %d texture %d: replay hash %s desc %s, live %s / %s"
                              % (where, t["slot"], t["texture"], kv["hash"], kv["desc"], t["hash"], t["desc"]))
    # per-frame texture state
    for frame, rec in sorted(frames.items()):
        snap = snaps.get("frame%d" % frame)
        if snap is None:
            continue  # the replay snapshots a frame when the next frame's first event arrives
        counts["frames"] += 1
        live = {t["id"]: t for t in rec["textures"]}
        if set(live) != set(snap):
            errors.append("frame %d: live textures %s, replay %s" % (frame, sorted(live), sorted(snap)))
        for tid in sorted(set(live) & set(snap)):
            counts["texture_entries"] += 1
            for k in TEXTURE_FIELDS:
                if jstr(live[tid][k]) != snap[tid].get(k):
                    errors.append("frame %d texture %d: %s: replay %s, live %s"
                                  % (frame, tid, k, snap[tid].get(k), live[tid][k]))
    if frames and counts["frames"] == 0:
        errors.append("no frame's texture state could be compared")
    errors.extend("texture replay script: " + e for e in tex_errors)
    return errors, counts


def check(args, run_dir, work):
    stream_path = os.path.join(run_dir, "relight_tap.jsonl")
    capture_path = os.path.join(run_dir, "relight_capture.jsonl")
    sidecar_path = os.path.join(run_dir, args.app + ".json")
    for p in (stream_path, capture_path, sidecar_path):
        if not os.path.isfile(p):
            return ["missing " + os.path.basename(p)], {}
    with open(sidecar_path) as f:
        sidecar = json.load(f)
    with open(stream_path) as f:
        lines = [l for l in f if l.strip()]
    stream = [json.loads(l) for l in lines]
    capture = read_jsonl(capture_path)
    if not capture or capture[0].get("schema") != "fuse.relight.capture/1":
        return ["relight_capture.jsonl: bad header"], {}
    os.makedirs(work, exist_ok=True)
    geo, resolved = geometry_oracle(args, lines, sidecar, work)
    snaps, tex_errors = texture_oracle(args, stream, sidecar, work)
    classified = classify_oracle(args, os.path.abspath(stream_path), snaps, work)
    return compare(capture, geo, resolved, snaps, tex_errors, classified)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--exe")
    p.add_argument("--app", required=True)
    p.add_argument("--d3d9")
    p.add_argument("--d3d8")
    p.add_argument("--runner")
    p.add_argument("--prefix-root")
    p.add_argument("--geometry-replay", required=True)
    p.add_argument("--texture-replay", required=True)
    p.add_argument("--classify-replay", required=True)
    p.add_argument("--emulator", default="")
    p.add_argument("--from-run")
    p.add_argument("--out", required=True)
    args = p.parse_args()
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    if args.from_run:
        run_dir = args.from_run
    else:
        if not shutil.which("wine") and not shutil.which("wine64"):
            print("SKIP: wine not installed")
            return SKIP
        run_dir = os.path.join(args.out, "run")
        rc, text = TAP.run_app(args, args.exe, run_dir, {
            "FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "capture",
            "FUSE_RELIGHT_TAP_CAPTURE_PATH": "relight_capture.jsonl", "FUSE_RELIGHT_TAP_CAPTURE_RECORD": "1",
            "FUSE_RELIGHT_TAP_RECORD_PATH": "relight_tap.jsonl"})
        if rc == SKIP:
            print(text.strip())
            return SKIP
        if rc != 0:
            print(text.strip()[-4000:])
            print("FAIL: %s: exited with %d" % (args.app, rc))
            return 1
    try:
        errors, c = check(args, run_dir, os.path.join(args.out, "check"))
    except RuntimeError as e:
        print("FAIL: %s: %s" % (args.app, e))
        return 1
    if errors:
        print("FAIL: %s: %d difference(s) between the live capture and the replay path:" % (args.app, len(errors)))
        for e in errors[:80]:
            print("  " + e)
        return 1
    print("PASS: %s: live capture == replay on %d draws (geometry %d, %d captured, %d unresolved by the sidecar; "
          "classification %d; %d bound-texture hashes), texture state of %d frame(s) (%d entries)"
          % (args.app, c["draws"], c["geometry"], c["captured"], c["unresolved"], c["classified"], c["textures"],
             c["frames"], c["texture_entries"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
