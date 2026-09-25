#!/usr/bin/env python3
"""FUSE Relight RL-1.2: capture expectations for the draw classifier.

Runs an RL-0.4 test app under Wine through the Relight d3d9.dll / d3d8.dll with the recording tap
(relight.tap.mode = record, the RL-1.1 driver rl_tap_run.py), then replays the recorded event stream
through the classifier (rl_classify_replay, the C++ ClassifyTap) once per configuration of
expectations/<scene>.json, and compares every draw of the app's recorded frames with the
expectation for its sidecar semantic tag (sky, world, hud, hud_positiont, shadow_volume, offscreen...).

Texture hashes: the recording keeps SHA-256 digests, not Remix hashes. The Remix texture hash of an
uploaded texture is XXH3-64 of its canonical mip-0 bytes, which the sidecar stores under the same
SHA-256 (Tools/FUSE/Relight/remix_hash_ref.py computes it). Render-target textures get Remix's
render-target image hash (XXH3 of the VkExtent3D, reseeded with a creation counter; NV-DXVK
D3D9CommonTexture::CreatePrimaryImage) and the descriptor hash of their D3D9_COMMON_TEXTURE_DESC.

  rl_classify_capture.py --exe app.exe --app sky_ui_hud --d3d9 d3d9.dll --d3d8 d3d8.dll --runner run.sh
      --prefix-root DIR --replay rl_classify_replay.exe [--emulator 'wine-run.sh|prefix'] --expect DIR --out DIR
  rl_classify_capture.py --from-run DIR ...   (reuse a recorded run: relight_tap.jsonl + <app>.json)
  rl_classify_capture.py selftest --expect DIR (expectation files parse; the matcher catches mismatches)

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb).
"""
import argparse
import base64
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP = 77
RT_USAGE = 0x1
TEXTURE_TYPE = 3


def load_hash_ref(path):
    sys.path.insert(0, path)
    import remix_hash_ref  # noqa: E402
    return remix_hash_ref


def read_stream(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def texture_hashes(events, sidecar, ref):
    """{texture id: Remix image hash}, the uploaded textures in order, the render-target textures in
    order with their descriptor hashes."""
    blobs = sidecar.get("blobs", {})
    hashes, uploads, rt_textures = {}, [], []
    rt_counter = 0
    for ev in events:
        if ev["ev"] == "texture_create" and ev["usage"] & RT_USAGE and ev["has_image"]:
            # NV-DXVK: every render-target image gets XXH3(extent) reseeded with a global counter.
            extent = struct.pack("<III", ev["width"], ev["height"], max(1, ev["depth"]))
            h = ref.xxh3_64(extent)
            h = ref.xxh3_64(struct.pack("<I", rt_counter), h)
            rt_counter += 1
            hashes[ev["id"]] = h
            if ev["type"] == TEXTURE_TYPE:
                pool = {"DEFAULT": 0, "MANAGED": 1, "SYSTEMMEM": 2, "SCRATCH": 3}.get(ev["pool"], 0)
                words = [ev["width"], ev["height"], max(1, ev["depth"]), ev["array_size"], ev["levels"], ev["usage"],
                         ev["format"], pool, ev["multisample"], 0]
                flags = (1 if ev["back_buffer"] else 0) << 1 | (1 if ev["attachment_only"] else 0) << 2
                rt_textures.append((ev["id"], ref.texture_descriptor(words, flags)))
        elif ev["ev"] == "texture_upload" and ev["level"] == 0 and ev["face"] == 0 and ev["texture"] not in hashes:
            blob = blobs.get(ev["blob"] or "")
            if blob is None:
                continue
            hashes[ev["texture"]] = ref.xxh3_64(base64.b64decode(blob))
            uploads.append(ev["texture"])
    return hashes, uploads, rt_textures


def expand_conf(lines, hashes, uploads, rt_textures):
    out = []
    for line in lines:
        for k, tex in enumerate(uploads):
            line = line.replace("{upload:%d}" % k, "0x%016X" % hashes[tex])
        for k, (_, desc) in enumerate(rt_textures):
            line = line.replace("{rtdesc:%d}" % k, "0x%016X" % desc)
        if "{" in line:
            raise ValueError("unresolved placeholder in conf line: " + line)
        out.append(line)
    return "\n".join(out) + "\n"


FIELDS = ("status", "reason", "inject", "sky_auto", "using_rt_rt", "drawing_to_rt_rt", "categories", "decision")


def check_draw(expect, got):
    """List of mismatch strings for one draw."""
    errors = []
    for f in FIELDS:
        if f in expect and got.get(f) != expect[f]:
            errors.append(f"{f}: expected {expect[f]!r}, got {got.get(f)!r}")
    for c in expect.get("categories_include", []):
        if c not in got.get("categories", "").split("|"):
            errors.append(f"categories: {c} missing from {got.get('categories')!r}")
    return errors


def compare(sidecar, classified, config):
    """Mismatches of one configuration: sidecar draws of the recorded frames vs classified draws."""
    errors = []
    by_frame = {}
    for c in classified:
        by_frame.setdefault(c["frame"], []).append(c)
    tags = config["tags"]
    checked = 0
    for frame in sidecar["recorded_frames"]:
        side = [d for d in sidecar["draws"] if d["frame"] == frame]
        got = by_frame.get(frame, [])
        if len(side) != len(got):
            errors.append(f"frame {frame}: sidecar has {len(side)} draw(s), the classifier saw {len(got)}")
            continue
        seen = {}
        for i, (d, g) in enumerate(zip(side, got)):
            tag = d.get("tag", "")
            if tag not in tags:
                errors.append(f"frame {frame} draw {i}: no expectation for tag {tag!r}")
                continue
            exp = tags[tag]
            n = seen.get(tag, 0)
            seen[tag] = n + 1
            if isinstance(exp, list):
                exp = exp[min(n, len(exp) - 1)]
            for e in check_draw(exp, g):
                errors.append(f"frame {frame} draw {i} ({tag} #{n}): {e}")
            checked += 1
        for tag in tags:
            if tag not in seen:
                errors.append(f"frame {frame}: expected tag {tag!r} has no draw")
    return errors, checked


def run_replay(args, stream, conf_path, hashes_path, out_path):
    cmd = []
    if args.emulator:
        cmd += [p for p in args.emulator.split("|") if p]
    cmd += [args.replay, "--stream", stream, "--texture-hashes", hashes_path, "--out", out_path]
    if conf_path:
        cmd += ["--conf", conf_path]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode(errors="replace")


def cmd_capture(args):
    scene = args.app[5:] if args.app.startswith("d3d8_") else args.app
    with open(os.path.join(args.expect, scene + ".json")) as f:
        expect = json.load(f)
    if args.from_run:
        run_dir = args.from_run
    else:
        sys.path.insert(0, os.path.join(HERE, "..", "tap"))
        import rl_tap_run  # noqa: E402
        run_dir = os.path.join(args.out, "run")
        rc, text = rl_tap_run.run_app(args, args.exe, run_dir, {
            "FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "record", "FUSE_RELIGHT_TAP_RECORD_PATH": "relight_tap.jsonl"})
        if rc == SKIP:
            print(text.strip())
            return SKIP
        if rc != 0:
            print(text.strip()[-4000:])
            print(f"FAIL: {args.app}: exited with {rc}")
            return 1
    stream = os.path.join(run_dir, "relight_tap.jsonl")
    sidecar_path = os.path.join(run_dir, args.app + ".json")
    for p in (stream, sidecar_path):
        if not os.path.isfile(p):
            print(f"FAIL: {args.app}: missing {p}")
            return 1
    with open(sidecar_path) as f:
        sidecar = json.load(f)
    ref = load_hash_ref(args.hash_ref)
    events = read_stream(stream)
    hashes, uploads, rt_textures = texture_hashes(events, sidecar, ref)
    os.makedirs(args.out, exist_ok=True)
    hashes_path = os.path.join(args.out, "texture_hashes.txt")
    with open(hashes_path, "w") as f:
        for tex, h in sorted(hashes.items()):
            f.write("%d 0x%016x\n" % (tex, h))

    failed = False
    summary = []
    for config in expect["configs"]:
        name = config["name"]
        conf_path = None
        if config["conf"]:
            conf_path = os.path.join(args.out, name + ".rtx.conf")
            with open(conf_path, "w") as f:
                f.write(expand_conf(config["conf"], hashes, uploads, rt_textures))
        out_path = os.path.join(args.out, name + ".classified.jsonl")
        rc, text = run_replay(args, stream, conf_path, hashes_path, out_path)
        if rc != 0 or not os.path.isfile(out_path):
            print(text.strip()[-4000:])
            print(f"FAIL: {args.app} [{name}]: rl_classify_replay exited with {rc}")
            failed = True
            continue
        classified = read_stream(out_path)
        errors, checked = compare(sidecar, classified, config)
        if errors:
            failed = True
            print(f"FAIL: {args.app} [{name}]: {len(errors)} mismatch(es) with the sidecar tags:")
            for e in errors[:40]:
                print("  " + e)
        summary.append(f"{name}: {checked} draw(s)")
    if failed:
        return 1
    print(f"PASS: {args.app}: classifications match the sidecar tags ({'; '.join(summary)}; "
          f"{len(uploads)} uploaded texture hash(es), {len(rt_textures)} render-target descriptor(s))")
    return 0


def cmd_selftest(args):
    """Expectation files parse, and the matcher reports seeded mismatches."""
    problems = 0
    for name in sorted(os.listdir(args.expect)):
        with open(os.path.join(args.expect, name)) as f:
            e = json.load(f)
        if e.get("schema") != "fuse.relight.classify_expect/1" or not e.get("configs"):
            print(f"FAIL: {name}: bad schema")
            problems += 1
        for c in e["configs"]:
            for tag, exp in c["tags"].items():
                for x in (exp if isinstance(exp, list) else [exp]):
                    unknown = set(x) - set(FIELDS) - {"categories_include", "why"}
                    if unknown:
                        print(f"FAIL: {name} [{c['name']}] {tag}: unknown fields {sorted(unknown)}")
                        problems += 1
    sidecar = {"recorded_frames": [1], "draws": [{"frame": 1, "tag": "a"}, {"frame": 1, "tag": "a"}]}
    config = {"tags": {"a": [{"status": "raytraced"}, {"status": "rasterized", "inject": True}]}}
    good = [{"frame": 1, "status": "raytraced"}, {"frame": 1, "status": "rasterized", "inject": True}]
    if compare(sidecar, good, config)[0]:
        print("FAIL: matcher rejects a matching stream")
        problems += 1
    bad = [{"frame": 1, "status": "raytraced"}, {"frame": 1, "status": "rasterized", "inject": False}]
    if len(compare(sidecar, bad, config)[0]) != 1:
        print("FAIL: matcher misses a seeded mismatch")
        problems += 1
    if not compare(sidecar, good[:1], config)[0]:
        print("FAIL: matcher misses a draw-count mismatch")
        problems += 1
    if problems:
        return 1
    print("PASS: expectation files parse; the matcher catches seeded mismatches")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", nargs="?", default="capture", choices=["capture", "selftest"])
    p.add_argument("--exe")
    p.add_argument("--app")
    p.add_argument("--d3d9")
    p.add_argument("--d3d8")
    p.add_argument("--runner")
    p.add_argument("--prefix-root")
    p.add_argument("--replay")
    p.add_argument("--emulator", default="")
    p.add_argument("--expect", required=True)
    p.add_argument("--hash-ref")
    p.add_argument("--from-run")
    p.add_argument("--out")
    args = p.parse_args()
    if args.mode == "selftest":
        return cmd_selftest(args)
    if not args.from_run:
        import shutil
        if not shutil.which("wine") and not shutil.which("wine64"):
            print("SKIP: wine not installed")
            return SKIP
    return cmd_capture(args)


if __name__ == "__main__":
    sys.exit(main())
