#!/usr/bin/env python3
"""FUSE Relight test-app checker (RL-0.4, plan §6.2).

Subcommands
  run       Run one app N times (default 2) under Wine + Xvfb (cmake/toolchains/fuse-wine-xvfb-run.sh,
            or xvfb-run + wine when that runner is absent), validate every run's sidecar and dumps,
            and require the runs to agree: the sidecar byte-for-byte, the images byte-for-byte unless
            tolerances.json allows a per-app difference. Exit 77 when Wine or Xvfb is missing.
  validate  Validate one output directory (sidecar schema + consistency + dumps + probes).
  compare   Compare two output directories of the same app.
  selftest  Check the schema validator on generated good and bad sidecars (no Wine needed).

Only the Python standard library is required. When the `jsonschema` package is importable it is
used as well (Draft 2020-12); the built-in validator implements the subset the schema uses.
"""
import argparse
import base64
import copy
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

SKIP = 77


# ---- minimal JSON Schema (2020-12 subset) validator ----------------------------------------------
class SchemaValidator:
    def __init__(self, schema):
        self.root = schema

    def resolve(self, ref):
        if not ref.startswith("#/"):
            raise ValueError("only local $ref supported: " + ref)
        node = self.root
        for part in ref[2:].split("/"):
            node = node[part]
        return node

    @staticmethod
    def type_ok(value, t):
        if t == "object":
            return isinstance(value, dict)
        if t == "array":
            return isinstance(value, list)
        if t == "string":
            return isinstance(value, str)
        if t == "boolean":
            return isinstance(value, bool)
        if t == "null":
            return value is None
        if t == "integer":
            return isinstance(value, int) and not isinstance(value, bool)
        if t == "number":
            return isinstance(value, (int, float)) and not isinstance(value, bool)
        raise ValueError("unknown type " + t)

    def errors(self, value, schema, path="$"):
        out = []
        self._check(value, schema, path, out)
        return out

    def _check(self, v, s, path, out):
        if s is True or s == {}:
            return
        if s is False:
            out.append(f"{path}: not allowed")
            return
        if "$ref" in s:
            self._check(v, self.resolve(s["$ref"]), path, out)
        if "type" in s:
            types = s["type"] if isinstance(s["type"], list) else [s["type"]]
            if not any(self.type_ok(v, t) for t in types):
                out.append(f"{path}: expected {s['type']}, got {type(v).__name__}")
                return
        if "const" in s and not (v == s["const"] and type(v) is type(s["const"])):
            out.append(f"{path}: expected const {s['const']!r}")
        if "enum" in s and not any(v == e and type(v) is type(e) for e in s["enum"]):
            out.append(f"{path}: {v!r} not in {s['enum']}")
        if isinstance(v, str):
            if "pattern" in s and not re.search(s["pattern"], v):
                out.append(f"{path}: {v[:40]!r} does not match {s['pattern']}")
            if "minLength" in s and len(v) < s["minLength"]:
                out.append(f"{path}: shorter than {s['minLength']}")
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            if "minimum" in s and v < s["minimum"]:
                out.append(f"{path}: {v} < {s['minimum']}")
            if "maximum" in s and v > s["maximum"]:
                out.append(f"{path}: {v} > {s['maximum']}")
        if isinstance(v, list):
            if "minItems" in s and len(v) < s["minItems"]:
                out.append(f"{path}: fewer than {s['minItems']} items")
            if "maxItems" in s and len(v) > s["maxItems"]:
                out.append(f"{path}: more than {s['maxItems']} items")
            if "items" in s:
                for i, item in enumerate(v):
                    self._check(item, s["items"], f"{path}[{i}]", out)
        if isinstance(v, dict):
            for k in s.get("required", []):
                if k not in v:
                    out.append(f"{path}: missing required {k!r}")
            props = s.get("properties", {})
            pats = s.get("patternProperties", {})
            for k, item in v.items():
                matched = False
                if k in props:
                    matched = True
                    self._check(item, props[k], f"{path}.{k}", out)
                for pat, sub in pats.items():
                    if re.search(pat, k):
                        matched = True
                        self._check(item, sub, f"{path}.{k}", out)
                if not matched and "additionalProperties" in s:
                    ap = s["additionalProperties"]
                    if ap is False:
                        out.append(f"{path}: unexpected property {k!r}")
                    elif isinstance(ap, dict):
                        self._check(item, ap, f"{path}.{k}", out)
        for key in ("oneOf", "anyOf"):
            if key in s:
                ok = sum(1 for sub in s[key] if not self.errors(v, sub, path))
                if (key == "oneOf" and ok != 1) or (key == "anyOf" and ok < 1):
                    out.append(f"{path}: {key} matched {ok} alternatives")
        if "allOf" in s:
            for sub in s["allOf"]:
                self._check(v, sub, path, out)


def schema_errors(doc, schema):
    errs = SchemaValidator(schema).errors(doc, schema)
    try:
        import jsonschema  # optional
    except ImportError:
        return errs
    v = jsonschema.Draft202012Validator(schema)
    errs += ["jsonschema: " + e.message for e in v.iter_errors(doc)]
    return errs


# ---- PNG reader (for the dumps the apps write: RGBA8, filter 0 or any standard filter) ------------
def read_png_rgba8(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w = 8, b"", None
    while pos < len(data):
        (n,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        (crc,) = struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            raise ValueError("bad CRC in " + ctype.decode())
        if ctype == b"IHDR":
            w, h, depth, color = struct.unpack(">IIBB", body[:10])
            if depth != 8 or color != 6:
                raise ValueError("not RGBA8")
        elif ctype == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, bytes(out)


# ---- validation of one output directory ------------------------------------------------------------
def load_sidecar(out_dir, app):
    path = os.path.join(out_dir, app + ".json")
    with open(path, "rb") as f:
        raw = f.read()
    return raw, json.loads(raw)


def validate_dir(out_dir, app, schema):
    errs = []
    try:
        raw, doc = load_sidecar(out_dir, app)
    except (OSError, ValueError) as e:
        return [f"{app}.json: {e}"]
    errs += schema_errors(doc, schema)
    if errs:
        return errs
    if doc["app"] != app:
        errs.append(f"sidecar app {doc['app']!r} != {app!r}")

    blobs = doc["blobs"]
    for key, b64 in blobs.items():
        data = base64.b64decode(b64)
        if hashlib.sha256(data).hexdigest() != key:
            errs.append(f"blob {key[:12]}: content hash mismatch")

    def need_blob(key, where):
        if key is not None and key not in blobs:
            errs.append(f"{where}: blob {key[:12]} missing")

    tex = {t["id"]: t for t in doc["textures"]}
    bufs = {b["id"]: b for b in doc["buffers"]}
    decls = {d["id"] for d in doc["declarations"]}
    shaders = {s["id"]: s for s in doc["shaders"]}
    for t in doc["textures"]:
        for u in t["uploads"]:
            need_blob(u["blob"], f"texture {t['id']} upload")
            if u["method"] == "LockRect" and u["blob"] in blobs:
                size = len(base64.b64decode(blobs[u["blob"]]))
                if size != u["row_bytes"] * u["rows"] or u["row_bytes"] % 4:
                    errs.append(f"texture {t['id']} level {u['level']}: blob size {size} != row_bytes*rows")
            if "source" in u and u["source"] not in tex:
                errs.append(f"texture {t['id']}: unknown upload source {u['source']}")
    for b in doc["buffers"]:
        for v in b["versions"]:
            need_blob(v["blob"], f"buffer {b['id']}")
            if v["blob"] in blobs and len(base64.b64decode(blobs[v["blob"]])) != b["size"]:
                errs.append(f"buffer {b['id']}: version blob size != buffer size")
            if v["offset"] + v["size"] > b["size"]:
                errs.append(f"buffer {b['id']}: write past the end")
    for s in doc["shaders"]:
        need_blob(s["blob"], f"shader {s['id']}")
        if s["declaration"] is not None and s["declaration"] not in decls:
            errs.append(f"shader {s['id']}: unknown declaration")
    for i, sb in enumerate(doc["state_blocks"]):
        for tb in sb["textures"]:
            if tb["texture"] not in tex:
                errs.append(f"state_blocks[{i}]: unknown texture {tb['texture']}")
        for key in ("vertex_shader", "pixel_shader"):
            if sb[key] is not None and sb[key] not in shaders:
                errs.append(f"state_blocks[{i}]: unknown {key} {sb[key]}")
        if isinstance(sb["render_target"], int) and sb["render_target"] not in tex:
            errs.append(f"state_blocks[{i}]: unknown render target")
    recorded = set(doc["recorded_frames"])
    last_seq = {}
    for i, d in enumerate(doc["draws"]):
        where = f"draws[{i}]"
        if d["frame"] not in recorded:
            errs.append(f"{where}: frame {d['frame']} not recorded")
        if d["state"] >= len(doc["state_blocks"]):
            errs.append(f"{where}: state index out of range")
        if last_seq.get(d["frame"], -1) >= d["seq"]:
            errs.append(f"{where}: seq not increasing")
        last_seq[d["frame"]] = d["seq"]
        for s in d["streams"]:
            b = bufs.get(s["buffer"])
            if b is None or b["kind"] != "vertex" or s["version"] >= len(b["versions"]):
                errs.append(f"{where}: bad stream binding {s}")
        ib = d["index_buffer"]
        if ib is not None:
            b = bufs.get(ib["buffer"])
            if b is None or b["kind"] != "index" or ib["version"] >= len(b["versions"]):
                errs.append(f"{where}: bad index buffer {ib}")
        if d["call"].endswith("UP"):
            if "up" not in d:
                errs.append(f"{where}: UP draw without 'up'")
            else:
                need_blob(d["up"]["vertex_blob"], where)
                need_blob(d["up"]["index_blob"], where)
        elif not d["streams"]:
            errs.append(f"{where}: no vertex stream bound")
        if not d["vertex_format"]["elements"]:
            errs.append(f"{where}: empty vertex format")
        if d["call"] == "DrawIndexedPrimitive" and ib is None:
            errs.append(f"{where}: indexed draw without index buffer")

    backbuffer = None
    for dump in doc["dumps"]:
        w, h = dump["width"], dump["height"]
        rawp = os.path.join(out_dir, dump["raw"])
        pngp = os.path.join(out_dir, dump["png"])
        try:
            px = open(rawp, "rb").read()
            pw, ph, ppx = read_png_rgba8(pngp)
        except (OSError, ValueError) as e:
            errs.append(f"dump {dump['name']}: {e}")
            continue
        if len(px) != w * h * 4:
            errs.append(f"dump {dump['name']}: raw size {len(px)} != {w}x{h}x4")
        if (pw, ph, ppx) != (w, h, px):
            errs.append(f"dump {dump['name']}: PNG and raw differ")
        if dump["alpha"] == "forced_opaque" and any(px[i] != 255 for i in range(3, len(px), 4)):
            errs.append(f"dump {dump['name']}: alpha not opaque")
        if dump["name"] == "backbuffer":
            backbuffer = px
    if backbuffer is None:
        errs.append("no backbuffer dump")
    else:
        for p in doc["probes"]:
            o = (p["y"] * doc["width"] + p["x"]) * 4
            got = list(backbuffer[o:o + 3])
            if any(abs(a - b) > p["tolerance"] for a, b in zip(got, p["rgb"])):
                errs.append(f"probe {p['what']!r} at ({p['x']},{p['y']}): got {got}, expected {p['rgb']} "
                            f"+-{p['tolerance']}")
    return errs


def image_diff(a, b):
    """(max channel difference, number of differing pixels) of two RGBA8 buffers."""
    if len(a) != len(b):
        return 255, max(len(a), len(b)) // 4
    maxd, npx = 0, 0
    for i in range(0, len(a), 4):
        if a[i:i + 4] != b[i:i + 4]:
            npx += 1
            maxd = max(maxd, max(abs(x - y) for x, y in zip(a[i:i + 4], b[i:i + 4])))
    return maxd, npx


def compare_dirs(dir_a, dir_b, app, tol):
    errs = []
    raw_a, doc = load_sidecar(dir_a, app)
    raw_b, _ = load_sidecar(dir_b, app)
    if raw_a != raw_b:
        errs.append(f"{app}.json differs between runs (sidecars must be byte-identical)")
    for dump in doc["dumps"]:
        a = open(os.path.join(dir_a, dump["raw"]), "rb").read()
        b = open(os.path.join(dir_b, dump["raw"]), "rb").read()
        if a == b:
            pa = open(os.path.join(dir_a, dump["png"]), "rb").read()
            pb = open(os.path.join(dir_b, dump["png"]), "rb").read()
            if pa != pb:
                errs.append(f"{dump['png']}: PNG bytes differ although pixels match")
            continue
        maxd, npx = image_diff(a, b)
        ok = maxd <= tol.get("max_channel_diff", 0) and npx <= tol.get("max_differing_pixels", 0)
        msg = f"{dump['raw']}: {npx} pixels differ, max channel diff {maxd}"
        if ok:
            print(f"note: {msg} (within tolerance: {tol.get('reason', '')})")
        else:
            errs.append(msg + f" (tolerance: max_channel_diff {tol.get('max_channel_diff', 0)}, "
                              f"max_differing_pixels {tol.get('max_differing_pixels', 0)})")
    return errs


# ---- running under Wine ------------------------------------------------------------------------------
def find_wine():
    for c in ("wine64", "/usr/lib/wine/wine64", "/usr/lib/x86_64-linux-gnu/wine/wine64", "wine"):
        p = shutil.which(c)
        if p:
            return p
    return None


def run_once(args, run_dir, extra_env=None):
    os.makedirs(run_dir, exist_ok=True)
    for f in os.listdir(run_dir):
        os.remove(os.path.join(run_dir, f))
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    exe = os.path.abspath(args.exe)
    if args.runner and os.path.isfile(args.runner):
        cmd = ["bash", args.runner, args.prefix_root, exe, "--out", ".", "--quiet"]
    else:
        # Fallback until cmake/toolchains/fuse-wine-xvfb-run.sh exists: xvfb-run + wine, own prefix.
        wine = find_wine()
        if not wine or not shutil.which("xvfb-run"):
            print("SKIP: wine or xvfb-run not installed")
            return SKIP
        prefix = os.path.join(os.path.abspath(args.prefix_root), "rl-apps-fallback")
        env.update({"WINEPREFIX": prefix, "WINEARCH": "win64", "WINEDEBUG": env.get("WINEDEBUG", "-all"),
                    "WINEDLLOVERRIDES": "mscoree,mshtml=;winemenubuilder.exe=d", "LIBGL_ALWAYS_SOFTWARE": "1"})
        xvfb = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24 +extension GLX"]
        if not os.path.isfile(os.path.join(prefix, "system.reg")):
            os.makedirs(prefix, exist_ok=True)
            subprocess.run(xvfb + [wine, "wineboot", "--init"], env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        cmd = xvfb + [wine, exe, "--out", ".", "--quiet"]
    proc = subprocess.run(cmd, cwd=run_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode(errors="replace")
    if proc.returncode == SKIP:
        print(text.strip())
        return SKIP
    if proc.returncode != 0:
        print(text.strip())
        print(f"FAIL: {os.path.basename(exe)} exited with {proc.returncode}")
    return proc.returncode


def load_tolerance(path, app):
    if not path or not os.path.isfile(path):
        return {}
    with open(path) as f:
        doc = json.load(f)
    tol = dict(doc.get("default", {}))
    tol.update(doc.get("apps", {}).get(app, {}))
    return tol


def cmd_run(args):
    schema = json.load(open(args.schema))
    tol = load_tolerance(args.tolerances, args.app)
    runs = []
    variants = [None] * args.runs
    if args.thread_variant:
        variants.append({"LP_NUM_THREADS": "1"})
    for i, env in enumerate(variants):
        run_dir = os.path.join(args.out, f"run{i + 1}")
        rc = run_once(args, run_dir, env)
        if rc == SKIP:
            return SKIP
        if rc != 0:
            return 1
        errs = validate_dir(run_dir, args.app, schema)
        if errs:
            print(f"FAIL: run {i + 1} ({run_dir}):")
            for e in errs[:50]:
                print("  " + e)
            return 1
        runs.append(run_dir)
    failed = False
    for other in runs[1:]:
        errs = compare_dirs(runs[0], other, args.app, tol)
        if errs:
            failed = True
            print(f"FAIL: {runs[0]} vs {other}:")
            for e in errs:
                print("  " + e)
    if failed:
        return 1
    doc = json.load(open(os.path.join(runs[0], args.app + ".json")))
    print(f"PASS: {args.app}: {len(runs)} runs agree; {len(doc['draws'])} draws, {len(doc['probes'])} probes, "
          f"{len(doc['textures'])} textures, {len(doc['buffers'])} buffers, {len(doc['shaders'])} shaders")
    return 0


def cmd_validate(args):
    schema = json.load(open(args.schema))
    errs = validate_dir(args.dir, args.app, schema)
    for e in errs:
        print(e)
    print("PASS" if not errs else "FAIL")
    return 1 if errs else 0


def cmd_compare(args):
    errs = compare_dirs(args.dir_a, args.dir_b, args.app, load_tolerance(args.tolerances, args.app))
    for e in errs:
        print(e)
    print("PASS" if not errs else "FAIL")
    return 1 if errs else 0


def cmd_selftest(args):
    schema = json.load(open(args.schema))
    blob = b"\x00\x01\x02\x03" * 4
    key = hashlib.sha256(blob).hexdigest()
    ident = [1.0 if i % 5 == 0 else 0.0 for i in range(16)]
    good = {
        "schema": "fuse.relight.app_sidecar/1", "app": "selftest", "scene": "selftest", "api": "d3d9",
        "covers": "self-test", "width": 128, "height": 96, "frames": 1, "recorded_frames": [0],
        "device": {"backbuffer_format": "X8R8G8B8", "depth_stencil_format": "D24S8", "vertex_processing": "hardware"},
        "dumps": [{"name": "backbuffer", "source": "test", "frame": 0, "width": 128, "height": 96,
                   "png": "selftest.png", "raw": "selftest.rgba", "alpha": "forced_opaque"}],
        "probes": [{"x": 0, "y": 0, "rgb": [1, 2, 3], "tolerance": 0, "what": "corner"}],
        "annotations": {},
        "textures": [{"id": 1, "kind": "texture", "width": 2, "height": 2, "levels": 1, "format": "A8R8G8B8",
                      "format_value": 21, "usage": 0, "pool": "MANAGED",
                      "uploads": [{"frame": -1, "level": 0, "method": "LockRect", "width": 2, "height": 2,
                                   "row_bytes": 8, "rows": 2, "blob": key}]}],
        "buffers": [{"id": 0, "kind": "vertex", "size": 16, "usage": 8, "pool": "MANAGED", "fvf": 2,
                     "versions": [{"frame": -1, "offset": 0, "size": 16, "lock_flags": 0, "blob": key}]}],
        "declarations": [], "shaders": [],
        "state_blocks": [{"render_states": {"LIGHTING": 0}, "render_states_float": {}, "texture_stages": [],
                          "samplers": [], "textures": [{"stage": 0, "texture": 1}], "lights": [], "material": None,
                          "viewport": {"x": 0, "y": 0, "width": 128, "height": 96, "min_z": 0, "max_z": 1},
                          "render_target": "backbuffer", "depth_stencil": "default", "vertex_shader": None,
                          "pixel_shader": None, "vs_const_f": [], "vs_const_i": [], "vs_const_b": [],
                          "ps_const_f": []}],
        "clears": [{"frame": 0, "seq": 0, "flags": 3, "color": "0xff010203", "z": 1, "stencil": 0,
                    "render_target": "backbuffer"}],
        "draws": [{"frame": 0, "seq": 1, "call": "DrawPrimitive", "primitive": "TRIANGLELIST", "prim_count": 1,
                   "tag": "world", "start_vertex": 0, "vertex_count": 3, "index_buffer": None,
                   "vertex_format": {"fvf": 2, "declaration": None,
                                     "elements": [{"stream": 0, "offset": 0, "type": "FLOAT3", "size": 12,
                                                   "method": 0, "usage": "POSITION", "usage_index": 0}]},
                   "streams": [{"stream": 0, "buffer": 0, "version": 0, "offset": 0, "stride": 12}],
                   "transforms": {"WORLD": ident, "VIEW": ident, "PROJECTION": ident}, "state": 0}],
        "blobs": {key: base64.b64encode(blob).decode()},
    }
    bad_cases = {
        "missing draws": lambda d: d.pop("draws"),
        "wrong schema id": lambda d: d.__setitem__("schema", "other/1"),
        "bad api": lambda d: d.__setitem__("api", "d3d10"),
        "bad blob key": lambda d: d["blobs"].__setitem__("XYZ", "AAAA"),
        "matrix of 15": lambda d: d["draws"][0]["transforms"].__setitem__("WORLD", ident[:15]),
        "unknown draw field": lambda d: d["draws"][0].__setitem__("extra", 1),
        "bad primitive": lambda d: d["draws"][0].__setitem__("primitive", "QUADS"),
        "string state": lambda d: d["state_blocks"][0]["render_states"].__setitem__("LIGHTING", "0"),
        "bool as integer": lambda d: d.__setitem__("frames", True),
        "probe out of image": lambda d: d["probes"][0].__setitem__("x", 128),
        "render target name": lambda d: d["state_blocks"][0].__setitem__("render_target", "front"),
        "bad transform key": lambda d: d["draws"][0]["transforms"].__setitem__("world", ident),
    }
    failures = 0
    errs = schema_errors(good, schema)
    if errs:
        failures += 1
        print("FAIL: good sidecar rejected:\n  " + "\n  ".join(errs))
    for name, mutate in bad_cases.items():
        d = copy.deepcopy(good)
        mutate(d)
        if not schema_errors(d, schema):
            failures += 1
            print(f"FAIL: bad sidecar accepted: {name}")
    # PNG reader round trip on a filtered image (Sub + Paeth rows).
    rows = [bytes([1]) + bytes(range(8)), bytes([4]) + bytes(range(8, 16))]
    png = b"\x89PNG\r\n\x1a\n"
    for ctype, body in ((b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0)),
                        (b"IDAT", zlib.compress(b"".join(rows))), (b"IEND", b"")):
        png += struct.pack(">I", len(body)) + ctype + body + struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF)
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"rl_app_selftest_{os.getpid()}.png")
    with open(tmp, "wb") as f:
        f.write(png)
    try:
        w, h, px = read_png_rgba8(tmp)
    finally:
        os.remove(tmp)
    expect0 = bytes([0, 1, 2, 3, 4, 6, 8, 10])
    if (w, h) != (2, 2) or px[:8] != expect0:
        failures += 1
        print(f"FAIL: PNG reader: {px[:8]!r}")
    print(f"{'PASS' if not failures else 'FAIL'}: selftest ({len(bad_cases)} bad cases, 1 good, PNG filters)")
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", required=True)
    r.add_argument("--app", required=True)
    r.add_argument("--schema", required=True)
    r.add_argument("--tolerances")
    r.add_argument("--out", required=True)
    r.add_argument("--runner", default="")
    r.add_argument("--prefix-root", required=True)
    r.add_argument("--runs", type=int, default=2)
    r.add_argument("--thread-variant", action="store_true",
                   default=os.environ.get("RL_APP_THREAD_VARIANT") == "1",
                   help="add a run with LP_NUM_THREADS=1 (llvmpipe single-threaded) to the comparison")
    v = sub.add_parser("validate")
    v.add_argument("--schema", required=True)
    v.add_argument("--app", required=True)
    v.add_argument("dir")
    c = sub.add_parser("compare")
    c.add_argument("--app", required=True)
    c.add_argument("--tolerances")
    c.add_argument("dir_a")
    c.add_argument("dir_b")
    s = sub.add_parser("selftest")
    s.add_argument("--schema", required=True)
    args = ap.parse_args()
    return {"run": cmd_run, "validate": cmd_validate, "compare": cmd_compare, "selftest": cmd_selftest}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
