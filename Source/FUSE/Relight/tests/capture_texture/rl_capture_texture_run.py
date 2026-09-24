#!/usr/bin/env python3
"""FUSE Relight RL-1.4: texture hashes of an RL-0.4 app through the recording tap under Wine.

1. Runs the app (texture_formats / d3d8_texture_formats) through the Relight d3d9.dll / d3d8.dll
   with relight.tap.mode = record, under cmake/toolchains/fuse-wine-xvfb-run.sh (Xvfb + Wine +
   Lavapipe), reusing the RL-1.1 driver (tests/tap/rl_tap_run.run_app).
2. Turns the recorded texture events (texture_create / texture_write_lock / texture_upload /
   texture_copy / image_destroy, the device's back buffer, and each draw's bound textures, shader
   presence and first disabled colour stage) into a replay script. The tap records only the
   SHA-256 of the bytes an upload locked; the bytes come from the app sidecar's blobs, which are
   keyed by the same SHA-256 (DXVK hands out Remix's canonical pitch, so the locked rows are the
   canonical rows). An upload whose bytes cannot be found fails the test.
3. Replays the script into TextureTracker (fuse_relight_capture_texture_tests --replay, a PE run
   under the Wine emulator in the MinGW tree) and checks, against Tools/FUSE/Relight/remix_hash_ref.py
   applied to the sidecar (independent layout table and xxHash):
     * every created format: a MANAGED texture drawn in frame 0 has hash = XXH3 over its canonical
       mip 0 at the end of frame 0; a created-but-never-drawn one (bump formats) is still pending
       there (upstream uploads - and hashes - managed textures at their first sampling draw) and
       gets the same reference hash once flushed;
     * UpdateTexture / UpdateSurface (CopyRects) destinations inherit the source's subresource-0
       hash (origin "inherited", from = the source); SYSTEMMEM sources have no hash of their own;
     * the back buffer's render-target hashes (counter 0, D3D9_COMMON_TEXTURE_DESC) and no hash on
       the auto depth-stencil;
     * every hashed texture is in the external image registry with the same hash.
Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb).
"""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
import subprocess
import sys

SKIP = 77
HERE = os.path.dirname(os.path.abspath(__file__))
TAP_TESTS = os.path.normpath(os.path.join(HERE, "..", "tap"))
POOLS = {"DEFAULT": 0, "MANAGED": 1, "SYSTEMMEM": 2, "SCRATCH": 3}
SLOT_OF_STAGE = lambda s: s if s < 16 else (16 + s - 256 if 256 <= s <= 260 else None)  # noqa: E731


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_stream(path):
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


class Script:
    def __init__(self, stream, sidecar):
        self.lines = []
        self.errors = []
        self.blobs = {k: base64.b64decode(v) for k, v in sidecar["blobs"].items()}
        self.state_blocks = {}

    def upload(self, e):
        blob = e.get("blob")
        data = self.blobs.get(blob) if blob else None
        if data is None or hashlib.sha256(data).hexdigest() != blob:
            self.errors.append(f"texture {e['texture']} level {e['level']}: upload bytes {blob} not in the sidecar")
            hexdata = "-"
        else:
            hexdata = data.hex()
        full = 1 if e["full"] else 0
        # The recording tap does not record the box of partial locks; the apps lock whole levels.
        box = (0, 0, e["width"], e["height"])
        self.lines.append(f"upload {e['texture']} {e['face']} {e['level']} {e['width']} {e['height']} "
                          f"{e['row_pitch']} {e['rows']} {e['lock_flags']} {full} {' '.join(map(str, box))} {hexdata}")

    def draw(self, e):
        st = self.state_blocks.get(e["state"], {})
        disabled = 8
        for stage in sorted(st.get("texture_stages", []), key=lambda x: x["stage"]):
            if stage["states"].get("COLOROP") == 1:  # D3DTOP_DISABLE
                disabled = stage["stage"]
                break
        # Stages the app never touched keep D3D9 defaults: stage 0 MODULATE, stages 1+ DISABLE.
        touched = {s["stage"] for s in st.get("texture_stages", [])}
        if disabled == 8:
            for s in range(1, 8):
                if s not in touched:
                    disabled = s
                    break
        ps = 1 if e.get("pixel_shader") else 0
        vs = 1 if e.get("vertex_shader") else 0
        slots = []
        for t in st.get("textures", []):
            slot = SLOT_OF_STAGE(t["stage"])
            if slot is not None:
                slots.append(f"{slot}:{t['texture']}")
        self.lines.append(f"draw {ps} {vs} {disabled} {' '.join(slots)}".rstrip())


def build_script(stream, sidecar):
    s = Script(stream, sidecar)
    frame = 0
    for e in stream:
        ev = e["ev"]
        if e.get("frame", frame) != frame:
            s.lines.append(f"snapshot frame{frame}")
            frame = e["frame"]
        if ev == "texture_create":
            vk = 0x100000 + e["id"] if e["has_image"] else 0
            s.lines.append(f"create {e['id']} {e['type']} {e['width']} {e['height']} {e['depth']} {e['levels']} "
                           f"{e['array_size']} {e['format']} {e['usage']} {POOLS[e['pool']]} {e['multisample']} "
                           f"{int(e['back_buffer'])} {int(e['attachment_only'])} {vk}")
        elif ev in ("device_create", "device_reset"):
            s.lines.append(f"device {e['back_buffer']} 0")
        elif ev == "texture_write_lock":
            s.lines.append(f"wlock {e['texture']} {e['face']} {e['level']} {e['lock_flags']}")
        elif ev == "texture_upload":
            s.upload(e)
        elif ev == "texture_copy":
            s.lines.append(f"copy {e['method']} {e['source']} {e['destination']} {e['source_face']} "
                           f"{e['source_level']} {e['dest_face']} {e['dest_level']} {int(e['has_rect'])}")
        elif ev == "state_block":
            s.state_blocks[e["index"]] = e["state"]
        elif ev == "draw":
            s.draw(e)
        elif ev == "present":
            pass
        elif ev == "image_destroy":
            if not any(l == "snapshot before_destroy" for l in s.lines):
                s.lines.append("snapshot before_destroy")
                s.lines.append("flushall")
                s.lines.append("snapshot flushed")
            s.lines.append(f"destroy {e['texture']}")
    if not any(l == "snapshot before_destroy" for l in s.lines):
        s.lines += ["snapshot before_destroy", "flushall", "snapshot flushed"]
    s.lines.append("snapshot end")
    return s


def parse_output(text):
    snaps = {}
    cur = None
    stats = ""
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "snapshot":
            cur = snaps.setdefault(parts[1], {})
        elif parts[0] == "tex" and cur is not None:
            kv = dict(p.split("=", 1) for p in parts[1:])
            cur[int(kv["id"])] = kv
        elif parts[0] == "stats":
            stats = line
    return snaps, stats


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--exe", required=True)
    p.add_argument("--app", required=True)
    p.add_argument("--d3d9", required=True)
    p.add_argument("--d3d8", required=True)
    p.add_argument("--runner", required=True)
    p.add_argument("--prefix-root", required=True)
    p.add_argument("--tool", required=True, help="fuse_relight_capture_texture_tests (replay driver)")
    p.add_argument("--emulator", nargs="+", default=[], help="CMAKE_CROSSCOMPILING_EMULATOR for --tool")
    p.add_argument("--ref", required=True, help="Tools/FUSE/Relight/remix_hash_ref.py")
    p.add_argument("--out", required=True)
    p.add_argument("--stream", help="reuse a recorded relight_tap.jsonl (with --sidecar) instead of running the app")
    p.add_argument("--sidecar")
    args = p.parse_args()
    for k in ("exe", "d3d9", "d3d8", "runner", "prefix_root", "tool", "ref", "out"):
        setattr(args, k, os.path.abspath(getattr(args, k)))
    app = args.app
    os.makedirs(args.out, exist_ok=True)

    if args.stream:
        stream_path, sidecar_path = args.stream, args.sidecar
    else:
        import shutil
        if not shutil.which("wine") and not shutil.which("wine64"):
            print("SKIP: wine not installed")
            return SKIP
        sys.path.insert(0, TAP_TESTS)
        import rl_tap_run  # noqa: E402  (RL-1.1 driver: run directory, DLLs, runner)
        run_dir = os.path.join(args.out, "run")
        rc, text = rl_tap_run.run_app(args, args.exe, run_dir,
                                      {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "record",
                                       "FUSE_RELIGHT_TAP_RECORD_PATH": "relight_tap.jsonl"})
        if rc == SKIP:
            print(text.strip())
            return SKIP
        if rc != 0:
            print(text.strip()[-4000:])
            print(f"FAIL: {app}: exited with {rc}")
            return 1
        stream_path = os.path.join(run_dir, "relight_tap.jsonl")
        sidecar_path = os.path.join(run_dir, app + ".json")
    stream = load_stream(stream_path)
    with open(sidecar_path) as f:
        sidecar = json.load(f)
    ref = load_module("remix_hash_ref", args.ref)

    script = build_script(stream, sidecar)
    script_path = os.path.join(args.out, "replay.txt")
    with open(script_path, "w") as f:
        f.write("\n".join(script.lines) + "\n")
    cmd = list(args.emulator) + [os.path.abspath(args.tool), "--replay", "replay.txt"]
    proc = subprocess.run(cmd, cwd=args.out, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = proc.stdout.decode(errors="replace")
    with open(os.path.join(args.out, "replay.log"), "w") as f:
        f.write(out)
    if proc.returncode == SKIP:
        print(out.strip())
        return SKIP
    if proc.returncode != 0:
        print(out.strip()[-4000:])
        print(f"FAIL: {app}: replay exited with {proc.returncode}")
        return 1
    snaps, stats = parse_output(out)
    errors = list(script.errors)

    # ---- map sidecar textures to tap ids (rl_tap_expect rule: creation order, implicit excluded)
    dev = next(e for e in stream if e["ev"] == "device_create")
    implicit = {dev["back_buffer"], dev["auto_depth_stencil"]}
    creates = {e["id"]: e for e in stream if e["ev"] == "texture_create"}
    tap_tex = [e for e in stream if e["ev"] == "texture_create" and e["id"] not in implicit]
    sc_tex = sorted(sidecar["textures"], key=lambda t: t["id"])
    tex_map = {s["id"]: t["id"] for s, t in zip(sc_tex, tap_tex)}
    sc_by_id = {t["id"]: t for t in sc_tex}

    frame0 = snaps.get("frame0", {})
    before = snaps.get("before_destroy", {})
    flushed = snaps.get("flushed", {})
    drawn = set()
    for sb in (e for e in stream if e["ev"] == "state_block"):
        for t in sb["state"].get("textures", []):
            drawn.add(t["texture"])

    def ref_hash(sc_tex_entry, blob_key):
        fmt = sc_tex_entry["format_value"]
        w, h = sc_tex_entry["width"], sc_tex_entry["height"]
        _, _, _, _, row_bytes, rows, size = ref.texture_layout(fmt, w, h, 1, 15)
        data = script.blobs[blob_key]
        up = next(u for u in sc_tex_entry["uploads"] if u.get("blob") == blob_key)
        pitch = up.get("row_bytes", row_bytes)
        take = min(pitch, row_bytes)
        packed = b"".join(data[r * pitch:r * pitch + take] + bytes(row_bytes - take) for r in range(rows))
        if len(packed) != size:
            errors.append(f"{sc_tex_entry['format']}: reference layout size {size} != {len(packed)}")
        return "%016X" % ref.xxh3_64(packed)

    checked_formats = []
    inherit_cases = []
    names = {}
    for sc in sc_tex:
        tid = tex_map.get(sc["id"])
        if tid is None:
            errors.append(f"sidecar texture {sc['id']}: no tap texture")
            continue
        uploads0 = [u for u in sc["uploads"] if u["level"] == 0 and u.get("blob")]
        if not uploads0:
            continue
        first = uploads0[0]
        name = f"{sc['format']}#{sc['id']}"
        names[tid] = name
        if sc["pool"] == "SYSTEMMEM":
            got = before.get(tid, {})
            if got.get("hash") != "0" * 16:
                errors.append(f"{name} (SYSTEMMEM source): hash {got.get('hash')} != 0 (no image, no hash)")
            continue
        if first["method"] == "LockRect":
            want = ref_hash(sc, first["blob"])
            if tid in drawn:
                got = frame0.get(tid, {})
                ok = got.get("hash") == want and got.get("origin") == "upload"
                where = "end of frame 0"
            else:
                got0 = frame0.get(tid, {})
                if got0.get("hash") != "0" * 16 or got0.get("pending") != "1" or got0.get("preview") != want:
                    errors.append(f"{name}: never drawn, expected pending with preview {want} at the end of frame 0, "
                                  f"got hash={got0.get('hash')} pending={got0.get('pending')} preview={got0.get('preview')}")
                got = flushed.get(tid, {})
                ok = got.get("hash") == want and got.get("origin") == "upload"
                where = "after flush (not drawn)"
            if not ok:
                errors.append(f"{name}: hash {got.get('hash')} ({got.get('origin')}) != reference {want} ({where})")
            if got.get("registered") != "1":
                errors.append(f"{name}: not in the external image registry with its hash")
            checked_formats.append((sc["format"], want, where))
        else:  # UpdateTexture / UpdateSurface / CopyRects destination
            src_sc = sc_by_id[first["source"]]
            src0 = next(u for u in src_sc["uploads"] if u["level"] == 0)
            want = ref_hash(src_sc, src0["blob"])
            got = frame0.get(tid, {})
            want_from = str(tex_map[src_sc["id"]])
            if got.get("hash") != want or got.get("origin") != "inherited" or got.get("from") != want_from:
                errors.append(f"{first['method']} {name}: hash {got.get('hash')} origin {got.get('origin')} "
                              f"from {got.get('from')} != reference {want} inherited from {want_from}")
            inherit_cases.append((first["method"], sc["format"], want))

    # ---- render targets and depth
    bb = frame0.get(dev["back_buffer"], {})
    c = creates[dev["back_buffer"]]
    extent = ref.u32le(c["width"]) + ref.u32le(c["height"]) + ref.u32le(max(1, c["depth"]))
    want_rt = "%016X" % ref.xxh3_64(ref.u32le(0), ref.xxh3_64(extent))
    words = [c["width"], c["height"], c["depth"], c["array_size"], c["levels"], c["usage"], c["format"],
             POOLS[c["pool"]], c["multisample"], 0]
    want_desc = "%016X" % ref.texture_descriptor(words, (1 if c["back_buffer"] else 0) << 1 |
                                                 (1 if c["attachment_only"] else 0) << 2)
    if bb.get("hash") != want_rt or bb.get("desc") != want_desc or bb.get("origin") != "render_target":
        errors.append(f"back buffer: hash {bb.get('hash')} desc {bb.get('desc')} != {want_rt} / {want_desc}")
    ds = frame0.get(dev["auto_depth_stencil"], {})
    if ds.get("hash") != "0" * 16 or ds.get("desc") != "0" * 16:
        errors.append(f"auto depth-stencil: hash {ds.get('hash')} desc {ds.get('desc')} (expected none)")

    created = sidecar.get("annotations", {}).get("created_formats", [])
    got_formats = {f for f, _, _ in checked_formats}
    for f in created:
        if f not in got_formats:
            errors.append(f"created format {f} was not checked")
    if not inherit_cases:
        errors.append("no inheritance case (UpdateTexture / UpdateSurface) found")

    with open(os.path.join(args.out, "hashes.txt"), "w") as f:
        for fm, h, where in checked_formats:
            f.write(f"{fm} {h} {where}\n")
        for m, fm, h in inherit_cases:
            f.write(f"{m} {fm} {h} inherited\n")
        f.write(f"backbuffer {want_rt} desc {want_desc}\n")
    if errors:
        print(f"FAIL: {app}: {len(errors)} difference(s):")
        for e in errors[:80]:
            print("  " + e)
        return 1
    skipped = sidecar.get("annotations", {}).get("skipped_formats", [])
    print(f"PASS: {app}: {len(checked_formats)} formats equal remix_hash_ref.py "
          f"({sum(1 for _, _, w in checked_formats if w.startswith('after'))} hashed only after flush: never drawn), "
          f"{len(inherit_cases)} inheritance case(s) ({', '.join(m for m, _, _ in inherit_cases)}), back buffer RT hashes; "
          f"skipped by the device: {', '.join(skipped) or 'none'}; {stats}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
