#!/usr/bin/env python3
"""FUSE Relight RL-1.8: capture writers on a real run (plan RL-1.8 exit criteria).

  capture   runs an RL-0.4 app under Wine through the Relight d3d9.dll / d3d8.dll with relight.tap.mode =
            capture + captureRecord (the RL-1.1 driver tests/tap/rl_tap_run.py, as tests/instances does): the
            recording tap writes the event stream (relight_tap.jsonl), CaptureTap the live, in-process capture
            record (relight_capture.jsonl). Then:
              1. rl_capture_export_replay replays the stream (bytes restored from the app sidecar by SHA-256)
                 through CaptureTap + SceneModel + GameCapturer and writes the capture (USDA + DDS + POCO store);
                 it re-ingests what it wrote and fails on any inconsistency (see its header);
              2. capture -> re-ingest -> identical key set: the key set the store re-ingests, the key set the
                 USDA implies and the key set written must be equal; and the Remix keys must equal the ones the
                 live, in-process capture record implies: the replayed draws are exactly the live record's
                 committed draws with captured geometry, each with the live asset key and colour texture; the
                 first draw of each instance gives remix.geom.asset and remix.tex (+ remix.rtdesc for a render
                 target), as GameCapturer::newInstance takes them (later draws of an instance with new vertex
                 data are time samples of its mesh), and every light of a captured frame gives remix.light;
              3. every captured texture's DDS was read back (dds_checked == textures);
              4. a second replay gives an identical capture (Tools/FUSE/Relight/capture_diff.py exit 0), and a
                 copy with one hash_key row dropped is reported as different (exit 1).
  selftest  the key-set checker on a synthetic live record + replay summary passes, and seeded faults (a
            missing mesh key, an extra light, a wrong texture key, an ingest mismatch, a replay failure) are
            caught.

  rl_capture_export.py capture --exe app.exe --app ff_textured --d3d9 d3d9.dll --d3d8 d3d8.dll --runner run.sh
      --prefix-root DIR --replay rl_capture_export_replay.exe --diff capture_diff.py [--emulator 'runner|prefix']
      --out DIR
  rl_capture_export.py capture --from-run DIR ...   (reuse a run: relight_tap.jsonl, relight_capture.jsonl, <app>.json)
  rl_capture_export.py selftest

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb).
"""
import argparse
import copy
import importlib.util
import json
import os
import shutil
import subprocess
import sys

# The helper imported from tests/tap (another package's directory) must not leave __pycache__ behind.
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP = 77
REMIX_ALGOS = ("remix.geom.asset", "remix.tex", "remix.tex.obsolete", "remix.light")  # the keys the USDA names


def hex16(v):
    return "%016X" % (int(v, 16) if isinstance(v, str) else int(v))


def live_draws(lines):
    """{(frame, di): (asset key, colour texture, rt descriptor or 0)} of the committed draws with captured geometry
    in the live, in-process capture record, and the Remix light keys of its presented frames {frame: set}."""
    draws, lights = {}, {}
    for l in lines:
        if l.get("ev") == "draw":
            geo = l.get("geometry") or {}
            cls = l.get("classification") or {}
            if cls.get("status") != "raytraced" or l.get("translation") is None or geo.get("status") != "captured":
                continue
            tex = int(cls.get("color_texture", "0"), 16)
            desc = 0
            for t in l.get("textures", []):
                if tex and int(t.get("hash", "0"), 16) == tex:
                    desc = int(t.get("desc", "0"), 16)
            draws[(l["frame"], l["di"])] = (int(geo.get("key", "0"), 16), tex, desc)
        elif l.get("ev") == "translate_frame":
            lights[l["frame"]] = set(int(x.get("hash", "0"), 16) for x in l.get("lights", []))
    return draws, lights


def expected_keys(lines, replay_draws):
    """The Remix keys the capture must hold, from the live record: GameCapturer takes an instance's mesh and
    material from its first draw (newInstance); later draws of the instance with new vertex data (dynamic
    geometry) become time samples of that mesh, not new meshes. The instance a draw became comes from the
    replay (SceneModel); every key comes from the live record. Returns (keys, errors)."""
    live, lights = live_draws(lines)
    errors = []
    frames = set(d["frame"] for d in replay_draws)
    replay_pos = set((d["frame"], d["di"]) for d in replay_draws)
    live_pos = set(p for p in live if p[0] in frames)
    if replay_pos != live_pos:
        errors.append("captured draws != the live record's committed draws with geometry: only replay %s, only live %s" %
                      (sorted(replay_pos - live_pos)[:6], sorted(live_pos - replay_pos)[:6]))
    keys, seen = set(), set()
    for d in sorted(replay_draws, key=lambda d: (d["frame"], d["di"])):
        pos = (d["frame"], d["di"])
        if pos not in live:
            continue
        key, tex, desc = live[pos]
        if int(d["key"], 16) != key:
            errors.append("draw %s: replay asset key %s != live %016X" % (pos, d["key"], key))
        if int(d["material"], 16) != tex:
            errors.append("draw %s: replay material %s != live colour texture %016X" % (pos, d["material"], tex))
        if d["instance"] == 0 or d["instance"] in seen:
            continue
        seen.add(d["instance"])
        if key:
            keys.add(("remix.geom.asset", hex16(key)))
        if tex:
            keys.add(("remix.tex", hex16(tex)))
            if desc:
                keys.add(("remix.rtdesc", hex16(desc)))
    for f in frames:
        for h in lights.get(f, ()):
            if h:
                keys.add(("remix.light", hex16(h)))
    return keys, errors


def check(lines, summary):
    """Returns a list of errors."""
    errors = list("replay: " + f for f in summary.get("failures", []))
    written = set((k["algo"], k["value"]) for k in summary.get("keys", []))
    ingested = set((k["algo"], k["value"]) for k in summary.get("ingested_keys", []))
    usda = set((k["algo"], k["value"]) for k in summary.get("usda_keys", []))
    if ingested != written:
        errors.append("re-ingest key set != written key set: only written %s, only ingested %s" %
                      (sorted(written - ingested)[:8], sorted(ingested - written)[:8]))
    remix = set(k for k in written if k[0] in REMIX_ALGOS)
    if usda != remix:
        errors.append("USDA key set != the store's Remix keys: only USDA %s, only store %s" %
                      (sorted(usda - remix)[:8], sorted(remix - usda)[:8]))
    # remix.tex.obsolete is the same key under rtx.useObsoleteHashOnTextureUpload.
    normalised = set(("remix.tex" if a == "remix.tex.obsolete" else a, v) for a, v in written
                     if a in REMIX_ALGOS or a == "remix.rtdesc")
    live, errs = expected_keys(lines, summary.get("draws", []))
    errors += errs
    if normalised != live:
        errors.append("capture key set != the live record's: missing %s, unexpected %s" %
                      (sorted(live - normalised)[:8], sorted(normalised - live)[:8]))
    c = summary.get("counts", {})
    if c.get("dds_checked", 0) != c.get("textures", 0) - c.get("textures_without_bytes", 0):
        errors.append("%s of %s texture(s) with bytes have a DDS file that was read back" %
                      (c.get("dds_checked"), c.get("textures", 0) - c.get("textures_without_bytes", 0)))
    if c.get("frames_captured", 0) < 1:
        errors.append("no frame captured")
    shas = [k for k in written if k[0] == "fuse.capture.sha256"]
    if c.get("meshes", 0) > 0 and not shas:
        errors.append("no fuse.capture.sha256 key")
    return errors


# ---------------------------------------------------------------------------------------------------
# capture
# ---------------------------------------------------------------------------------------------------

def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_jsonl(path):
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


def run_replay(args, run_dir, out_dir):
    cmd = [p for p in args.emulator.split("|") if p] if args.emulator else []
    cmd += [args.replay, "--stream", os.path.join(run_dir, "relight_tap.jsonl"),
            "--sidecar", os.path.join(run_dir, args.app + ".json"), "--out", out_dir, "--game", args.app]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode(errors="replace")
    summary_path = os.path.join(out_dir, "summary.json")
    summary = None
    if os.path.isfile(summary_path):
        with open(summary_path) as f:
            summary = json.load(f)
    return proc.returncode, text, summary


def run_diff(args, a, b):
    proc = subprocess.run([sys.executable, args.diff, a, b, "--max", "10"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode(errors="replace")


def cmd_capture(args):
    if args.from_run:
        run_dir = args.from_run
    else:
        if not shutil.which("wine") and not shutil.which("wine64"):
            print("SKIP: wine not installed")
            return SKIP
        tap_run = load_module("rl_tap_run", os.path.join(HERE, "..", "tap", "rl_tap_run.py"))
        run_dir = os.path.join(args.out, "run")
        rc, text = tap_run.run_app(args, args.exe, run_dir, {
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
    for name in ("relight_tap.jsonl", "relight_capture.jsonl", args.app + ".json"):
        if not os.path.isfile(os.path.join(run_dir, name)):
            print("FAIL: %s: missing %s" % (args.app, os.path.join(run_dir, name)))
            return 1
    os.makedirs(args.out, exist_ok=True)
    first = os.path.join(args.out, "replay1")
    second = os.path.join(args.out, "replay2")
    rc, text, summary = run_replay(args, run_dir, first)
    if summary is None or rc not in (0, 1):
        print(text.strip()[-4000:])
        print("FAIL: %s: rl_capture_export_replay exited with %d and no summary" % (args.app, rc))
        return 1
    errors = check(read_jsonl(os.path.join(run_dir, "relight_capture.jsonl")), summary)
    rc2, text2, summary2 = run_replay(args, run_dir, second)
    if summary2 is None:
        errors.append("the second replay wrote no summary (exit %d): %s" % (rc2, text2.strip()[-400:]))
    else:
        drc, dtext = run_diff(args, os.path.join(first, "capture"), os.path.join(second, "capture"))
        if drc != 0:
            errors.append("two replays of the same run differ (capture_diff exit %d):\n%s" % (drc, dtext.strip()))
        seeded = os.path.join(args.out, "seeded")
        if os.path.isdir(seeded):
            shutil.rmtree(seeded)
        shutil.copytree(os.path.join(first, "capture"), seeded)
        db_path = os.path.join(seeded, "store", "db", "remaster_db.json")
        with open(db_path) as f:
            db = json.load(f)
        if db.get("hash_key"):
            db["hash_key"].pop()
            with open(db_path, "w") as f:
                json.dump(db, f, indent=2)
            drc, dtext = run_diff(args, os.path.join(first, "capture"), seeded)
            if drc != 1 or "keys" not in dtext:
                errors.append("capture_diff misses a dropped hash_key row (exit %d): %s" % (drc, dtext.strip()[-400:]))
    if errors:
        print(text.strip()[-2000:])
        print("FAIL: %s: %d problem(s):" % (args.app, len(errors)))
        for e in errors[:40]:
            print("  " + e)
        return 1
    c = summary["counts"]
    print("PASS: %s: %d frame(s) captured, %d mesh(es), %d material(s), %d DDS texture(s) round-tripped, %d instance(s), "
          "%d light(s), camera %s, %d USDA layer(s); %d key(s): written == re-ingested == USDA == live record; "
          "replay deterministic (capture_diff), seeded difference caught; unrestorable bytes: %d buffer write(s), %d upload(s)"
          % (args.app, c["frames_captured"], c["meshes"], c["materials"], c["dds_checked"], c["instances"],
             c["sphere_lights"] + c["distant_lights"], "yes" if c["camera"] else "no", c["usda_layers"],
             len(summary["keys"]), c["buffer_writes_unrestored"], c["uploads_unrestored"]))
    return 0


# ---------------------------------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------------------------------

def synthetic():
    lines = [
        {"ev": "draw", "frame": 0, "di": 0, "geometry": {"status": "captured", "key": "06a0e3abadeb3249"},
         "classification": {"status": "raytraced", "color_texture": "0x6808d561b4c64ec4"}, "translation": {},
         "textures": [{"slot": 0, "hash": "6808D561B4C64EC4", "desc": "0000000000000000"}]},
        {"ev": "draw", "frame": 0, "di": 1, "geometry": {"status": "captured", "key": "59d6cc6c20964c16"},
         "classification": {"status": "raytraced", "color_texture": "0x0d147dec03eb30ef"}, "translation": {},
         "textures": [{"slot": 0, "hash": "0D147DEC03EB30EF", "desc": "00000000C0FFEE00"}]},
        # rasterized (UI) and not captured draws give no key
        {"ev": "draw", "frame": 0, "di": 2, "geometry": {"status": "captured", "key": "1111111111111111"},
         "classification": {"status": "rasterized", "color_texture": "0x2222222222222222"}, "translation": {}},
        {"ev": "draw", "frame": 0, "di": 3, "geometry": {"status": "NoPosition"},
         "classification": {"status": "raytraced", "color_texture": "0x3333333333333333"}, "translation": {}},
        {"ev": "translate_frame", "frame": 0, "lights": [{"hash": "0x00000000abcdef01"}]},
        # frame 1: instance 7 again with new vertex data (dynamic geometry): no new mesh key
        {"ev": "draw", "frame": 1, "di": 0, "geometry": {"status": "captured", "key": "4444444444444444"},
         "classification": {"status": "raytraced", "color_texture": "0x6808d561b4c64ec4"}, "translation": {},
         "textures": [{"slot": 0, "hash": "6808D561B4C64EC4", "desc": "0000000000000000"}]},
    ]
    draws = [{"frame": 0, "di": 0, "instance": 7, "key": "06A0E3ABADEB3249", "material": "6808D561B4C64EC4"},
             {"frame": 0, "di": 1, "instance": 8, "key": "59D6CC6C20964C16", "material": "0D147DEC03EB30EF"},
             {"frame": 1, "di": 0, "instance": 7, "key": "4444444444444444", "material": "6808D561B4C64EC4"}]
    keys = [("remix.geom.asset", "06A0E3ABADEB3249"), ("remix.geom.asset", "59D6CC6C20964C16"),
            ("remix.tex", "6808D561B4C64EC4"), ("remix.tex", "0D147DEC03EB30EF"), ("remix.rtdesc", "00000000C0FFEE00"),
            ("remix.light", "00000000ABCDEF01"), ("fuse.capture.sha256", "ab" * 32)]
    rows = [{"algo": a, "value": v, "kind": "x"} for a, v in keys]
    summary = {"failures": [], "keys": rows, "ingested_keys": rows, "draws": draws,
               "usda_keys": [r for r in rows if r["algo"] in REMIX_ALGOS],
               "counts": {"dds_checked": 1, "textures": 2, "textures_without_bytes": 1, "frames_captured": 2, "meshes": 2}}
    return lines, summary


def cmd_selftest(args):
    del args
    lines, summary = synthetic()
    errors = check(lines, summary)
    if errors:
        print("FAIL: the synthetic capture does not pass: %s" % errors)
        return 1
    faults = []

    def drop_mesh(l, s):
        s["keys"] = [k for k in s["keys"] if k["value"] != "59D6CC6C20964C16"]
        s["ingested_keys"] = s["keys"]
        s["usda_keys"] = [k for k in s["usda_keys"] if k["value"] != "59D6CC6C20964C16"]
    faults.append(("a mesh key missing from the capture", drop_mesh))

    def extra_light(l, s):
        row = {"algo": "remix.light", "value": "00000000DEADBEEF", "kind": "light"}
        s["keys"].append(row)
        s["ingested_keys"].append(row)
        s["usda_keys"].append(row)
    faults.append(("a light the live record does not have", extra_light))

    def wrong_texture(l, s):
        l[0]["classification"]["color_texture"] = "0x6808d561b4c64ec5"
    faults.append(("a texture key that differs from the live record", wrong_texture))

    def ingest_mismatch(l, s):
        s["ingested_keys"] = s["ingested_keys"][:-1]
    faults.append(("a re-ingest that lost a row", ingest_mismatch))

    def usda_mismatch(l, s):
        s["usda_keys"] = s["usda_keys"][1:]
    faults.append(("a USDA without one mesh", usda_mismatch))

    def replay_failure(l, s):
        s["failures"] = ["dds 0000: does not read back"]
    faults.append(("a failed replay check", replay_failure))

    def dds_unchecked(l, s):
        s["counts"]["dds_checked"] = 0
    faults.append(("a texture without a DDS round trip", dds_unchecked))

    def live_light(l, s):
        l.append({"ev": "translate_frame", "frame": 1, "lights": [{"hash": "0x0000000000000042"}]})
    faults.append(("a live light missing from the capture", live_light))

    def replay_key(l, s):
        s["draws"][1]["key"] = "59D6CC6C20964C17"
    faults.append(("a replay asset key that differs from the live one", replay_key))

    def lost_draw(l, s):
        s["draws"].pop(1)
    faults.append(("a committed draw the replay did not capture", lost_draw))

    def missing_rtdesc(l, s):
        s["keys"] = [k for k in s["keys"] if k["algo"] != "remix.rtdesc"]
        s["ingested_keys"] = s["keys"]
    faults.append(("a render target without its descriptor key", missing_rtdesc))

    def dynamic_as_new(l, s):
        s["draws"][2]["instance"] = 9
    faults.append(("dynamic geometry captured as a new instance", dynamic_as_new))

    for what, seed in faults:
        l2, s2 = synthetic()
        l2, s2 = copy.deepcopy(l2), copy.deepcopy(s2)
        seed(l2, s2)
        if not check(l2, s2):
            print("FAIL: the checker missed %s" % what)
            return 1
    print("PASS: the key-set checker passes the synthetic capture and catches %d seeded fault(s)" % len(faults))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["capture", "selftest"])
    p.add_argument("--exe")
    p.add_argument("--app")
    p.add_argument("--d3d9")
    p.add_argument("--d3d8")
    p.add_argument("--runner")
    p.add_argument("--prefix-root")
    p.add_argument("--replay")
    p.add_argument("--diff")
    p.add_argument("--emulator", default="")
    p.add_argument("--from-run")
    p.add_argument("--out")
    args = p.parse_args()
    if args.mode == "selftest":
        return cmd_selftest(args)
    args.out = os.path.abspath(args.out)
    return cmd_capture(args)


if __name__ == "__main__":
    sys.exit(main())
