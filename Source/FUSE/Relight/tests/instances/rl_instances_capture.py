#!/usr/bin/env python3
"""FUSE Relight RL-1.7: instance tracking on a real run (plan RL-1.7 exit criteria, app ff_multi_instance).

  capture   runs the RL-0.4 app under Wine through the Relight d3d9.dll / d3d8.dll with
            relight.tap.mode = capture + captureRecord (the RL-1.1 driver tests/tap/rl_tap_run.py, as
            tests/tap_capture does): the recording tap writes the event stream (relight_tap.jsonl) and CaptureTap
            the live geometry / texture record (relight_capture.jsonl). rl_instances_replay feeds the stream
            through TranslateTap and every committed draw into SceneModel, and this script checks, on all 60
            recorded frames:
              1. ids: each of the 9 logical instances (draw order within the frame) keeps one instance id over the
                 60 frames, except the teleported one, which gets a new id (never used before) at the teleport
                 frame and keeps it after; the 9 ids of a frame are distinct; the old id is garbage-collected at
                 the end of the teleport frame; 9 instances are alive after every frame;
              2. transforms: objectToWorld equals the app's recorded WORLD (float32 exact) and the analytic scene
                 (below, within 1e-4); the previous transform equals the previous frame's WORLD, or the current one
                 for a new instance (Remix teleport: new instances are still);
              3. motion: the per-instance motion of the object-space centroid (current - previous) equals the
                 analytic motion of the scene within 1e-3 units; static instances (0..3) are static and take the
                 preserve path from their third frame on; the teleported instance is matched as "new".
  selftest  the checker passes a synthetic replay made from the analytic scene, and catches seeded faults
            (an id change, a teleport keeping its id, a wrong previous transform, a leaked old id, a transform
            that differs from the app's record).

The analytic scene is Tests/relight/apps/scenes/ff_multi_instance.cpp: instance i of frame f at
x = -600 + 300 (i mod 5), z = 0 (i < 5) or 400; i = 4..6 move (x + 4f, 3 ((f (i + 1)) mod 7 - 3), z - 2f);
i = 7 spins (rotationY(0.1 f) x translation(x, 0, z)); i = 8 sits at (-600, 150, 800) and teleports to
(600, 150, 800) at frame 30 (the sidecar annotation gives instance, frame and distance); the others are static.

  rl_instances_capture.py capture --exe app.exe --app ff_multi_instance --d3d9 d3d9.dll --d3d8 d3d8.dll
      --runner run.sh --prefix-root DIR --replay rl_instances_replay.exe [--emulator 'runner|prefix'] --out DIR
  rl_instances_capture.py capture --from-run DIR ...   (reuse a run: relight_tap.jsonl, relight_capture.jsonl, <app>.json)
  rl_instances_capture.py selftest

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb).
"""
import argparse
import importlib.util
import json
import math
import os
import shutil
import struct
import subprocess
import sys

# The helper imported from tests/tap (another package's directory) must not leave __pycache__ behind.
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP = 77
INSTANCES = 9


def f32(x):
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def identity():
    return [1.0 if i % 5 == 0 else 0.0 for i in range(16)]


def translation(x, y, z):
    m = identity()
    m[12], m[13], m[14] = x, y, z
    return m


def rotation_y(a):
    m = identity()
    c, s = math.cos(a), math.sin(a)
    m[0], m[2], m[8], m[10] = c, -s, s, c
    return m


def mul(a, b):
    return [sum(a[i * 4 + k] * b[k * 4 + j] for k in range(4)) for i in range(4) for j in range(4)]


def point(m, p):
    x, y, z = p
    return [x * m[0] + y * m[4] + z * m[8] + m[12], x * m[1] + y * m[5] + z * m[9] + m[13],
            x * m[2] + y * m[6] + z * m[10] + m[14]]


class Scene:
    """The analytic ff_multi_instance scene, parameterized by the sidecar's teleport annotation."""

    def __init__(self, annotations):
        tp = annotations.get("instances", {}).get("teleport", {})
        self.teleport_instance = int(tp.get("instance", 8))
        self.teleport_frame = int(tp.get("frame", 30))
        self.teleport_distance = float(tp.get("distance", 1200.0))

    def world(self, i, frame):
        f = float(frame)
        x = -600.0 + 300.0 * float(i % 5)
        z = 0.0 if i < 5 else 400.0
        if 4 <= i <= 6:
            return translation(x + 4.0 * f, 3.0 * (float((frame * (i + 1)) % 7) - 3.0), z - 2.0 * f)
        if i == 7:
            return mul(rotation_y(0.1 * f), translation(x, 0.0, z))
        if i == self.teleport_instance:
            start = -600.0
            return translation(start if frame < self.teleport_frame else start + self.teleport_distance, 150.0, 800.0)
        return translation(x, 0.0, z)

    def is_static(self, i):
        return i < 4


def close(a, b, tol):
    return all(abs(x - y) <= tol * max(1.0, abs(y)) for x, y in zip(a, b))


def f32_equal(a, b):
    return all(f32(x) == f32(y) for x, y in zip(a, b))


def sidecar_worlds(sidecar):
    """{frame: [WORLD of draw 0, 1, ...]} in the app's draw order."""
    worlds = {}
    for d in sidecar["draws"]:
        worlds.setdefault(d["frame"], []).append(d.get("transforms", {}).get("WORLD", identity()))
    return worlds


def check(sidecar, lines):
    """Returns (errors, counters)."""
    errors = []
    scene = Scene(sidecar.get("annotations", {}))
    worlds = sidecar_worlds(sidecar)
    rf = sidecar.get("recorded_frames", 0)
    frames_count = len(rf) if isinstance(rf, list) else int(rf)
    draws = {}
    frame_lines = {}
    for l in lines:
        if l["ev"] == "draw":
            draws.setdefault(l["frame"], []).append(l)
        elif l["ev"] == "frame":
            frame_lines[l["frame"]] = l
    c = {"frames": 0, "draws": 0, "moving": 0, "preserved": 0}
    ids = {i: [] for i in range(INSTANCES)}
    tpi, tpf = scene.teleport_instance, scene.teleport_frame
    for frame in range(frames_count):
        fd = [d for d in draws.get(frame, []) if d.get("committed")]
        if len(fd) != INSTANCES:
            errors.append("frame %d: %d committed draw(s), expected %d" % (frame, len(fd), INSTANCES))
            continue
        c["frames"] += 1
        seen = set()
        for i, d in enumerate(fd):
            c["draws"] += 1
            where = "frame %d instance %d" % (frame, i)
            ids[i].append(d["instance"])
            if d["instance"] in seen:
                errors.append("%s: id %d used twice in the frame" % (where, d["instance"]))
            seen.add(d["instance"])
            rec = worlds.get(frame, [])
            if i >= len(rec) or not f32_equal(d["world"], rec[i]):
                errors.append("%s: objectToWorld %s != the app's WORLD %s" % (where, d["world"][12:15],
                                                                              rec[i][12:15] if i < len(rec) else None))
            if not close(d["world"], scene.world(i, frame), 1e-4):
                errors.append("%s: objectToWorld %s != analytic %s" % (where, d["world"][12:15], scene.world(i, frame)[12:15]))
            fresh = frame == 0 or (i == tpi and frame == tpf)
            if fresh:
                if not d["created"]:
                    errors.append("%s: expected a new instance" % where)
                if d["prev"] != d["world"]:
                    errors.append("%s: a new instance's previous transform must be its current one" % where)
                expected_motion = [0.0, 0.0, 0.0]
            else:
                if d["created"]:
                    errors.append("%s: unexpected new instance" % where)
                prev_rec = worlds.get(frame - 1, [])
                if i >= len(prev_rec) or not f32_equal(d["prev"], prev_rec[i]):
                    errors.append("%s: previous transform %s != last frame's WORLD" % (where, d["prev"][12:15]))
                expected_motion = [a - b for a, b in zip(point(scene.world(i, frame), (0, 0, 0)),
                                                         point(scene.world(i, frame - 1), (0, 0, 0)))]
            motion = [a - b for a, b in zip(point(d["world"], (0, 0, 0)), point(d["prev"], (0, 0, 0)))]
            if any(abs(a - b) > 1e-3 for a, b in zip(motion, expected_motion)):
                errors.append("%s: motion %s != analytic %s" % (where, motion, expected_motion))
            if any(abs(v) > 0 for v in expected_motion):
                c["moving"] += 1
            if scene.is_static(i) and not d["static"]:
                errors.append("%s: a static instance is not static" % where)
            if scene.is_static(i) and frame >= 2 and d["path"] != "preserve":
                errors.append("%s: path %s, expected preserve" % (where, d["path"]))
            if d["path"] == "preserve":
                c["preserved"] += 1
            if i == tpi and frame == tpf and d["match"] != "new":
                errors.append("%s: the teleported draw matched as %s" % (where, d["match"]))
        fl = frame_lines.get(frame)
        if fl is None:
            errors.append("frame %d: no frame line" % frame)
        elif fl["instances"] != INSTANCES:
            errors.append("frame %d: %d instance(s) alive after GC, expected %d" % (frame, fl["instances"], INSTANCES))
    # ids stable over the frames, teleport -> new id.
    for i in range(INSTANCES):
        v = ids[i]
        if len(v) != c["frames"]:
            continue
        if i != tpi:
            if len(set(v)) != 1:
                errors.append("instance %d: ids %s over %d frames, expected one" % (i, sorted(set(v)), len(v)))
            continue
        before, after = set(v[:tpf]), set(v[tpf:])
        if len(before) != 1 or len(after) != 1:
            errors.append("instance %d: ids %s before / %s after the teleport, expected one each" % (i, before, after))
        elif before == after:
            errors.append("instance %d: the teleport past uniqueObjectDistance kept id %s" % (i, before))
        else:
            used = set(x for j in range(INSTANCES) for x in ids[j][:tpf])
            if after & used:
                errors.append("instance %d: the new id %s was used before" % (i, after))
            old = next(iter(before))
            if tpf - 1 in frame_lines and old not in frame_lines[tpf - 1]["ids"]:
                errors.append("frame %d: id %d not alive before the teleport" % (tpf - 1, old))
            if tpf in frame_lines and old in frame_lines[tpf]["ids"]:
                errors.append("frame %d: the teleported instance's old id %d is still alive" % (tpf, old))
    c["ids"] = len(set(x for v in ids.values() for x in v))
    return errors, c


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
    stream = os.path.join(run_dir, "relight_tap.jsonl")
    capture = os.path.join(run_dir, "relight_capture.jsonl")
    sidecar_path = os.path.join(run_dir, args.app + ".json")
    for p in (stream, capture, sidecar_path):
        if not os.path.isfile(p):
            print("FAIL: %s: missing %s" % (args.app, p))
            return 1
    with open(sidecar_path) as f:
        sidecar = json.load(f)
    os.makedirs(args.out, exist_ok=True)
    out_path = os.path.join(args.out, "instances.jsonl")
    cmd = [p for p in args.emulator.split("|") if p] if args.emulator else []
    cmd += [args.replay, "--stream", stream, "--capture", capture, "--out", out_path]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode(errors="replace")
    if proc.returncode != 0 or not os.path.isfile(out_path):
        print(text.strip()[-4000:])
        print("FAIL: %s: rl_instances_replay exited with %d" % (args.app, proc.returncode))
        return 1
    errors, c = check(sidecar, read_jsonl(out_path))
    if errors:
        print("FAIL: %s: %d mismatch(es):" % (args.app, len(errors)))
        for e in errors[:60]:
            print("  " + e)
        return 1
    print("PASS: %s: %d frame(s), %d draw(s): %d instance id(s) (one per logical instance, a new one for the "
          "teleport), transforms = the app's WORLD, %d moving draw(s) with analytic motion, %d preserved draw(s)"
          % (args.app, c["frames"], c["draws"], c["ids"], c["moving"], c["preserved"]))
    return 0


# ---------------------------------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------------------------------

def synthetic(frames=60):
    """A sidecar and the replay output a correct scene model gives on it (built from the analytic scene)."""
    annotations = {"instances": {"teleport": {"instance": 8, "frame": 30, "distance": 1200.0}}}
    scene = Scene(annotations)
    sidecar = {"recorded_frames": frames, "annotations": annotations, "draws": []}
    lines = []
    ids = list(range(1, INSTANCES + 1))
    next_id = INSTANCES + 1
    for frame in range(frames):
        if frame == scene.teleport_frame:
            ids[scene.teleport_instance] = next_id
            next_id += 1
        for i in range(INSTANCES):
            world = [f32(v) for v in scene.world(i, frame)]
            sidecar["draws"].append({"frame": frame, "transforms": {"WORLD": world}})
            fresh = frame == 0 or (i == scene.teleport_instance and frame == scene.teleport_frame)
            prev = world if fresh else [f32(v) for v in scene.world(i, frame - 1)]
            lines.append({"ev": "draw", "frame": frame, "index": i, "committed": True, "instance": ids[i],
                          "created": fresh, "match": "new" if fresh else "identity" if scene.is_static(i) else "nearest",
                          "path": "preserve" if scene.is_static(i) and frame >= 2 else "instance",
                          "static": scene.is_static(i) or prev == world, "world": world, "prev": prev})
        lines.append({"ev": "frame", "frame": frame, "instances": INSTANCES, "ids": sorted(ids)})
    return sidecar, lines


def cmd_selftest(args):
    del args
    sidecar, lines = synthetic()
    errors, c = check(sidecar, lines)
    if errors:
        print("FAIL: the synthetic replay does not pass:")
        for e in errors[:20]:
            print("  " + e)
        return 1
    if c["ids"] != INSTANCES + 1 or c["frames"] != 60:
        print("FAIL: synthetic counters %s" % c)
        return 1

    def draw(ls, frame, i):
        return [l for l in ls if l["ev"] == "draw" and l["frame"] == frame][i]

    def frame_line(ls, frame):
        return [l for l in ls if l["ev"] == "frame" and l["frame"] == frame][0]

    faults = []

    def fault_id_change(ls):
        for l in ls:
            if l["ev"] == "draw" and l["index"] == 5 and l["frame"] >= 17:
                l["instance"] = 99
    faults.append(("an id change of a moving instance", fault_id_change))

    def fault_teleport_keeps_id(ls):
        for l in ls:
            if l["ev"] == "draw" and l["index"] == 8 and l["frame"] >= 30:
                l["instance"] = 9
    faults.append(("a teleport keeping its id", fault_teleport_keeps_id))

    def fault_prev(ls):
        d = draw(ls, 10, 4)
        d["prev"] = list(d["world"])
    faults.append(("a moving instance without motion", fault_prev))

    def fault_leak(ls):
        frame_line(ls, 30)["ids"] = sorted(frame_line(ls, 30)["ids"] + [9])
    faults.append(("the old id alive after the teleport", fault_leak))

    def fault_world(ls):
        d = draw(ls, 20, 2)
        d["world"] = list(d["world"])
        d["world"][12] += 0.5
    faults.append(("a transform that is not the app's", fault_world))

    def fault_teleport_prev(ls):
        d = draw(ls, 30, 8)
        d["prev"] = [f32(v) for v in Scene({}).world(8, 29)]
    faults.append(("a new instance inheriting a previous transform", fault_teleport_prev))

    def fault_missing_draw(ls):
        ls.remove(draw(ls, 40, 3))
    faults.append(("a missing draw", fault_missing_draw))

    for what, seed in faults:
        s2, l2 = synthetic()
        seed(l2)
        errors, _ = check(s2, l2)
        if not errors:
            print("FAIL: the checker missed %s" % what)
            return 1
    print("PASS: the checker passes the analytic scene and catches %d seeded fault(s)" % len(faults))
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
