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
4. With --translate-replay (the RL-1.5 apps ff_lit, ff_alpha, ff_fog and their D3D8 twins): also replays the
   stream through rl_translate_replay (TranslateTap: classifier + fixed-function translation, with the same
   texture hashes as rl_classify_replay) and compares, on every draw, the live record's "translation"
   (material, fog, texture stage, transforms, clip plane, lights added, depth state, camera, alpha swizzle)
   with the replay's draw line, and, per presented frame, the live "translate_frame" (light list, fog,
   fog states, cameras, camera cut) with the replay's frame line - exactly, as both print through
   scene/translate/translate_json.hpp.

5. With --export-replay and --capture-diff (RL-1.8 live): the run also has relight.tap.captureExport on
   (FUSE_RELIGHT_TAP_CAPTURE_EXPORT=1, _DIR=capture_export; with --export-window F:N also _FIRST_FRAME=F and
   _FRAMES=N, so d3d9.dll writes the capture when frame F+N-1 is presented): d3d9.dll writes the USDA + POCO
   store + DDS capture itself (tap/capture/capture_export_live.hpp: SceneModel + GameCapturer on CaptureTap's
   frames). rl_capture_export_replay replays the recorded stream of the same run into DIR/check/export_replay
   (--game <app>, and --first-frame F --frames N), and Tools/FUSE/Relight/capture_diff.py must find the live
   capture and the replayed one identical (files, hash_key rows, assets, POCO records, blobs, DDS bytes, USD
   prims, attributes and transforms).

  rl_capture_live.py --exe app.exe --app ff_lit --d3d9 d3d9.dll --d3d8 d3d8.dll --runner run.sh
      --prefix-root DIR --geometry-replay geometry_replay.exe --texture-replay tests.exe
      --classify-replay rl_classify_replay.exe [--translate-replay rl_translate_replay.exe]
      [--export-replay rl_capture_export_replay.exe --capture-diff capture_diff.py [--export-window F:N]]
      [--emulator 'runner|prefix'] --out DIR [--keep]
  rl_capture_live.py --from-run DIR ...   (reuse a run directory: relight_tap.jsonl, relight_capture.jsonl,
                                           <app>.json, capture_export/)
On a pass the bulky outputs (the run's streams and captures, the replayed capture) are removed unless --keep.
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


def translate_oracle(args, stream_path, work):
    """rl_translate_replay's lines, with the texture hashes classify_oracle wrote."""
    out_path = os.path.join(work, "translated.jsonl")
    cmd = emulator(args) + [args.translate_replay, "--stream", stream_path, "--texture-hashes",
                            os.path.join(work, "texture_hashes.txt"), "--out", out_path]
    rc, out, err = run_tool(cmd)
    if rc != 0 or not os.path.isfile(out_path):
        raise RuntimeError("rl_translate_replay exited with %d: %s" % (rc, (out + err)[-2000:]))
    return read_jsonl(out_path)


def compare_translation(capture, translated):
    """Live TranslateTap (the capture record) vs rl_translate_replay: (errors, counts)."""
    errors = []
    draws = [r for r in capture if r["ev"] == "draw"]
    live_frames = {r["frame"]: r for r in capture if r["ev"] == "translate_frame"}
    rdraws = [t for t in translated if t["ev"] == "draw"]
    rframes = {t["frame"]: t for t in translated if t["ev"] == "frame"}
    counts = dict(draws=0, translated=0, lights=0, frames=0, frame_lights=0)
    if len(rdraws) != len(draws):
        errors.append("live capture has %d draws, rl_translate_replay %d" % (len(draws), len(rdraws)))
    for d, r in zip(draws, rdraws):
        where = "draw %d (frame %d, #%d)" % (d["n"], d["frame"], d["di"])
        live = d.get("translation")
        if live is None:
            errors.append("%s: no translation in the live record" % where)
            continue
        want = {k: v for k, v in r.items() if k != "ev"}
        if live != want:
            keys = sorted(k for k in set(live) | set(want) if live.get(k) != want.get(k))
            errors.append("%s: translation differs in %s" % (where, ", ".join(keys)))
            for k in keys[:3]:
                errors.append("    %s: replay %s, live %s" % (k, json.dumps(want.get(k))[:300], json.dumps(live.get(k))[:300]))
        counts["draws"] += 1
        counts["translated"] += 1 if live.get("translated") else 0
        counts["lights"] += len(live.get("lights", []))
    if not live_frames:
        errors.append("the live capture recorded no translate_frame")
    for frame, lf in sorted(live_frames.items()):
        rf = rframes.get(frame)
        if rf is None:
            errors.append("frame %d: no frame line from rl_translate_replay" % frame)
            continue
        want = {k: v for k, v in rf.items() if k != "ev"}
        got = {k: v for k, v in lf.items() if k != "ev"}
        if got != want:
            keys = sorted(k for k in set(got) | set(want) if got.get(k) != want.get(k))
            errors.append("frame %d: translate_frame differs in %s" % (frame, ", ".join(keys)))
        counts["frames"] += 1
        counts["frame_lights"] += len(lf.get("lights", []))
    return errors, counts


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


def export_window(args):
    """(first frame, frame count) of --export-window F:N, or None."""
    if not args.export_window:
        return None
    first, _, count = args.export_window.partition(":")
    return int(first), int(count or 0)


def export_env(args):
    if not args.export_replay:
        return {}
    env = {"FUSE_RELIGHT_TAP_CAPTURE_EXPORT": "1", "FUSE_RELIGHT_TAP_CAPTURE_EXPORT_DIR": "capture_export"}
    window = export_window(args)
    if window:
        env["FUSE_RELIGHT_TAP_CAPTURE_EXPORT_FIRST_FRAME"] = str(window[0])
        env["FUSE_RELIGHT_TAP_CAPTURE_EXPORT_FRAMES"] = str(window[1])
    return env


def check_export(args, run_dir, work):
    """RL-1.8 live: the capture d3d9.dll wrote == rl_capture_export_replay's capture of the same run
    (capture_diff). Returns (errors, counts)."""
    live = os.path.join(run_dir, "capture_export")
    if not os.path.isdir(live):
        return ["d3d9.dll wrote no capture (%s)" % live], {}
    extra = [d for d in os.listdir(run_dir) if d.startswith("capture_export") and d != "capture_export"]
    errors = ["unexpected capture directories %s" % sorted(extra)] if extra else []
    out = os.path.join(work, "export_replay")
    if os.path.isdir(out):
        shutil.rmtree(out)
    cmd = emulator(args) + [args.export_replay, "--stream", os.path.abspath(os.path.join(run_dir, "relight_tap.jsonl")),
                            "--sidecar", os.path.abspath(os.path.join(run_dir, args.app + ".json")),
                            "--out", out, "--game", args.app]
    window = export_window(args)
    if window:
        cmd += ["--first-frame", str(window[0]), "--frames", str(window[1])]
    rc, stdout, stderr = run_tool(cmd)
    summary_path = os.path.join(out, "summary.json")
    if rc not in (0, 1) or not os.path.isfile(summary_path):
        raise RuntimeError("rl_capture_export_replay exited with %d: %s" % (rc, (stdout + stderr)[-2000:]))
    with open(summary_path) as f:
        summary = json.load(f)
    # The replay's own RL-1.8 checks (re-ingest, USDA, DDS) are rl_capture_export_<app>'s; here they are
    # reported, and the capture it wrote is the oracle of the live one.
    replay_failures = summary.get("failures", [])
    rc, text = run_tool([sys.executable, args.capture_diff, live, os.path.join(out, "capture"), "--max", "20"])[:2]
    if rc != 0:
        errors.append("live capture != replayed capture (capture_diff exit %d):" % rc)
        errors.extend("    " + l for l in text.strip().splitlines()[:40])
    c = summary.get("counts", {})
    counts = dict(frames=c.get("frames_captured", 0), meshes=c.get("meshes", 0), materials=c.get("materials", 0),
                  textures=c.get("textures", 0), instances=c.get("instances", 0),
                  lights=c.get("sphere_lights", 0) + c.get("distant_lights", 0), keys=len(summary.get("keys", [])),
                  replay_failures=replay_failures)
    if counts["frames"] < 1:
        errors.append("the replay captured no frame")
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
    errors, counts = compare(capture, geo, resolved, snaps, tex_errors, classified)
    if args.translate_replay:
        translated = translate_oracle(args, os.path.abspath(stream_path), work)
        terr, tcounts = compare_translation(capture, translated)
        errors.extend("translation: " + e for e in terr)
        counts["translate"] = tcounts
    if args.export_replay:
        eerr, ecounts = check_export(args, run_dir, work)
        errors.extend("capture export: " + e for e in eerr)
        counts["export"] = ecounts
    return errors, counts


def cleanup(args, run_dir, work):
    """Removes the bulky outputs of a passing run (disk space: 28 apps x captures)."""
    if args.keep:
        return
    for p in (os.path.join(work, "export_replay"), os.path.join(run_dir, "capture_export")):
        if os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
    if not args.from_run:
        for name in ("relight_tap.jsonl", "relight_capture.jsonl", args.app + ".json", args.app + ".rgba"):
            p = os.path.join(run_dir, name)
            if os.path.isfile(p):
                os.remove(p)


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
    p.add_argument("--translate-replay")
    p.add_argument("--export-replay")
    p.add_argument("--capture-diff")
    p.add_argument("--export-window")
    p.add_argument("--keep", action="store_true")
    p.add_argument("--emulator", default="")
    p.add_argument("--from-run")
    p.add_argument("--out", required=True)
    args = p.parse_args()
    args.out = os.path.abspath(args.out)
    if bool(args.export_replay) != bool(args.capture_diff):
        p.error("--export-replay and --capture-diff go together")
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
            "FUSE_RELIGHT_TAP_RECORD_PATH": "relight_tap.jsonl", **export_env(args)})
        if rc == SKIP:
            print(text.strip())
            return SKIP
        if rc != 0:
            print(text.strip()[-4000:])
            print("FAIL: %s: exited with %d" % (args.app, rc))
            return 1
    work = os.path.join(args.out, "check")
    try:
        errors, c = check(args, run_dir, work)
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
    if "translate" in c:
        t = c["translate"]
        print("PASS: %s: live TranslateTap == rl_translate_replay on %d draws (%d translated, %d light(s) added) and "
              "%d frame(s) (%d frame light(s))" % (args.app, t["draws"], t["translated"], t["lights"], t["frames"],
                                                   t["frame_lights"]))
    if "export" in c:
        e = c["export"]
        print("PASS: %s: live capture export == rl_capture_export_replay (capture_diff) on %d frame(s)%s: %d mesh(es), "
              "%d material(s), %d texture(s), %d instance(s), %d light(s), %d key(s)%s"
              % (args.app, e["frames"], " (window %s)" % args.export_window if args.export_window else "", e["meshes"],
                 e["materials"], e["textures"], e["instances"], e["lights"], e["keys"],
                 "; replay's own checks: %s" % "; ".join(e["replay_failures"][:3]) if e["replay_failures"] else ""))
    cleanup(args, run_dir, work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
