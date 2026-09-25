#!/usr/bin/env python3
"""FUSE Relight RL-1.8: compare two capture directories (plan RL-1.8 `capture_diff`).

A capture directory is what Source/FUSE/Relight/capture/export writes (capture_writer.hpp):

  <stage>.usda, meshes/, materials/, lights/, skeletons/   the Remix-compatible capture (USDA, plan §1.9)
  textures/<HASH>.dds                                      captured albedo textures
  store/poco/**.poco.json, store/blobs/sha256/..,          the FUSE capture: POCO records, blobs and the
  store/db/remaster_db.json                                hash_key / original_asset tables

The comparison reports differences by category:

  files       a file present on one side only (outside the store's blobs)
  keys        hash_key rows (algo, value) present on one side only, or mapped to a different asset kind
  assets      original_asset rows (oaid, kind) present on one side only
  records     POCO records present on one side only, or with a different header / payload (blob refs by sha)
  blobs       store blobs present on one side only
  textures    DDS files with different bytes
  prims       USD prims (layer, path, type) present on one side only
  attributes  USD properties / metadata with different values; numbers within --tol (relative, default
              1e-5) are equal, so re-captures that differ in float noise compare equal
  transforms  the same for xformOp:transform values (reported separately: instance / camera / light motion)

  capture_diff.py A B [--tol 1e-5] [--json] [--max 50]   exit 0: same, 1: different, 2: unreadable input
  capture_diff.py selftest                                identical synthetic captures compare equal and
                                                          every seeded difference is reported in its
                                                          category (and only there)

The USDA reader is a small parser for the subset the capture layout uses (TinyUSDZ, Remaster W2.2, is not
vendored yet). Python standard library only.
"""
import argparse
import hashlib
import json
import math
import os
import shutil
import struct
import sys
import tempfile

sys.dont_write_bytecode = True

CATEGORIES = ["files", "keys", "assets", "records", "blobs", "textures", "prims", "attributes", "transforms"]


# ---------------------------------------------------------------------------------------------------
# USDA subset reader
# ---------------------------------------------------------------------------------------------------

class UsdaError(Exception):
    pass


def _tokenize(text):
    toks = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r":
            i += 1
        elif c == "#":
            while i < n and text[i] != "\n":
                i += 1
        elif c == '"':
            j = i + 1
            out = []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    out.append({"n": "\n", "t": "\t"}.get(text[j + 1], text[j + 1]))
                    j += 2
                    continue
                out.append(text[j])
                j += 1
            if j >= n:
                raise UsdaError("line %d: unterminated string" % line)
            toks.append(("str", "".join(out), line))
            i = j + 1
        elif c == "@":
            j = text.find("@", i + 1)
            if j < 0:
                raise UsdaError("line %d: unterminated asset path" % line)
            toks.append(("asset", text[i + 1:j], line))
            i = j + 1
        elif c == "<":
            j = text.find(">", i + 1)
            if j < 0:
                raise UsdaError("line %d: unterminated path" % line)
            toks.append(("path", text[i + 1:j], line))
            i = j + 1
        elif c.isdigit() or (c in "+-." and i + 1 < n and (text[i + 1].isdigit() or text[i + 1] in ".in")):
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] in "+-.") and not (text[j] in "+-" and text[j - 1] not in "eE"):
                j += 1
            lit = text[i:j]
            try:
                val = float(lit)
            except ValueError:
                raise UsdaError("line %d: bad number %r" % (line, lit))
            toks.append(("num", (val, lit), line))
            i = j
        elif c.isalpha() or c == "_":
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] in "_:." or (text[j] == "[" and text[j + 1:j + 2] == "]")
                             or (text[j] == "]" and text[j - 1] == "[")):
                j += 1
            while j > i + 1 and text[j - 1] == ":":
                j -= 1
            toks.append(("id", text[i:j], line))
            i = j
        elif c in "()[]{}=,:":
            toks.append(("p", c, line))
            i += 1
        else:
            raise UsdaError("line %d: unexpected character %r" % (line, c))
    toks.append(("end", None, line))
    return toks


class _Parser:
    def __init__(self, toks):
        self.t = toks
        self.i = 0

    def peek(self, k=0):
        return self.t[min(self.i + k, len(self.t) - 1)]

    def next(self):
        tok = self.peek()
        self.i += 1
        return tok

    def fail(self, why):
        tok = self.peek()
        raise UsdaError("line %d: %s (at %r)" % (tok[2], why, tok[1]))

    def punct(self, c):
        return self.peek()[0] == "p" and self.peek()[1] == c

    def expect(self, c):
        if not self.punct(c):
            self.fail("expected %r" % c)
        self.i += 1

    def value(self):
        kind, v, _ = self.peek()
        if kind == "num":
            self.i += 1
            return v[0]
        if kind == "str":
            self.i += 1
            return v
        if kind == "asset":
            self.i += 1
            if self.peek()[0] == "path":
                return ("@", v, self.next()[1])
            return ("@", v, "")
        if kind == "path":
            self.i += 1
            return ("<", v)
        if kind == "id":
            self.i += 1
            if v in ("nan", "inf"):
                return float(v)
            return ("token", v)
        if kind == "p" and v in "([":
            close = ")" if v == "(" else "]"
            self.i += 1
            items = []
            while not self.punct(close):
                items.append(self.value())
                if self.punct(","):
                    self.i += 1
                elif not self.punct(close):
                    self.fail("expected ',' or %r" % close)
            self.i += 1
            return items
        if kind == "p" and v == "{":
            return self.dictionary()
        self.fail("bad value")

    def dictionary(self):
        self.expect("{")
        out = {}
        while not self.punct("}"):
            if self.peek()[0] == "num":
                key = self.next()[1][0]
                self.expect(":")
                out[key] = self.value()
                if self.punct(","):
                    self.i += 1
                continue
            if self.peek()[0] != "id" or self.peek(1)[0] != "id":
                self.fail("expected 'type name' in a dictionary")
            self.next()
            name = self.next()[1]
            self.expect("=")
            out[name] = self.value()
        self.i += 1
        return out

    def metadata(self, required=False):
        out = {}
        if not self.punct("("):
            if required:
                self.fail("expected a metadata block")
            return out
        self.i += 1
        while not self.punct(")"):
            if self.peek()[0] == "str":
                out["doc"] = self.next()[1]
                continue
            if self.peek()[0] != "id":
                self.fail("expected a metadata key")
            key = self.next()[1]
            if key in ("prepend", "append", "add", "delete", "reorder") and self.peek()[0] == "id":
                key += " " + self.next()[1]
            self.expect("=")
            out[key] = self.value()
        self.i += 1
        return out

    def prim(self, parent, prims):
        spec = self.next()
        if spec[0] != "id" or spec[1] not in ("def", "over", "class"):
            self.fail("expected def / over / class")
        ptype = self.next()[1] if self.peek()[0] == "id" else ""
        if self.peek()[0] != "str":
            self.fail("expected a prim name")
        name = self.next()[1]
        path = parent + "/" + name
        entry = {"specifier": spec[1], "type": ptype, "metadata": self.metadata(), "properties": {}}
        prims[path] = entry
        self.expect("{")
        while not self.punct("}"):
            if self.peek()[0] != "id":
                self.fail("expected a property or a prim")
            if self.peek()[1] in ("def", "over", "class"):
                self.prim(path, prims)
                continue
            self.property(entry["properties"])
        self.i += 1

    def property(self, props):
        words = []
        if self.peek()[1] == "rel":
            self.i += 1
            words.append("rel")
        else:
            while self.peek()[0] == "id" and self.peek()[1] in ("custom", "uniform", "varying"):
                words.append(self.next()[1])
            if self.peek()[0] != "id":
                self.fail("expected a property type")
            words.append(self.next()[1])
        if self.peek()[0] != "id":
            self.fail("expected a property name")
        name = self.next()[1]
        field = "default"
        if name.endswith(".timeSamples"):
            name, field = name[:-12], "timeSamples"
        elif name.endswith(".connect"):
            name, field = name[:-8], "connect"
        p = props.setdefault(name, {"decl": " ".join(words)})
        if self.punct("="):
            self.i += 1
            p[field] = self.value()
        md = self.metadata()
        if md:
            p["metadata"] = md

    def layer(self):
        md = self.metadata(required=True)
        prims = {}
        while self.peek()[0] != "end":
            self.prim("", prims)
        return md, prims


def parse_usda(text):
    """Returns (layer metadata, {prim path: {specifier, type, metadata, properties}})."""
    if not text.startswith("#usda 1.0"):
        raise UsdaError("line 1: not a USDA 1.0 layer")
    return _Parser(_tokenize(text)).layer()


# ---------------------------------------------------------------------------------------------------
# capture loading and comparison
# ---------------------------------------------------------------------------------------------------

def _files(root):
    out = []
    for d, _, names in os.walk(root):
        for n in names:
            out.append(os.path.relpath(os.path.join(d, n), root).replace(os.sep, "/"))
    return sorted(out)


def load_capture(root):
    if not os.path.isdir(root):
        raise IOError("%s: not a directory" % root)
    cap = {"files": set(), "keys": {}, "assets": {}, "records": {}, "blobs": set(), "textures": {}, "usda": {}}
    for rel in _files(root):
        path = os.path.join(root, rel)
        if rel.startswith("store/blobs/"):
            cap["blobs"].add(os.path.basename(rel))
            continue
        cap["files"].add(rel)
        if rel == "store/db/remaster_db.json":
            with open(path) as f:
                db = json.load(f)
            for row in db.get("hash_key", []):
                cap["keys"][(row["algo"], row["value"])] = row.get("kind", "")
            for row in db.get("original_asset", []):
                cap["assets"][row["oaid"]] = row.get("kind", "")
        elif rel.endswith(".poco.json"):
            with open(path) as f:
                rec = json.load(f)
            cap["records"][rel] = rec
        elif rel.endswith(".dds"):
            with open(path, "rb") as f:
                cap["textures"][rel] = hashlib.sha256(f.read()).hexdigest()
        elif rel.endswith(".usda"):
            with open(path) as f:
                text = f.read()
            try:
                cap["usda"][rel] = parse_usda(text)
            except UsdaError as e:
                raise IOError("%s: %s" % (rel, e))
    return cap


def _flatten(v):
    if isinstance(v, (list, tuple)) and not (isinstance(v, tuple) and v and v[0] in ("@", "<", "token")):
        out = []
        for e in v:
            out.extend(_flatten(e))
        return out
    return [v]


def values_equal(a, b, tol):
    if isinstance(a, dict) and isinstance(b, dict):
        if set(a) != set(b):
            return False
        return all(values_equal(a[k], b[k], tol) for k in a)
    fa, fb = _flatten(a), _flatten(b)
    if len(fa) != len(fb):
        return False
    for x, y in zip(fa, fb):
        if isinstance(x, float) and isinstance(y, float):
            if math.isnan(x) and math.isnan(y):
                continue
            if x == y:
                continue
            if math.isinf(x) or math.isinf(y) or abs(x - y) > tol * max(1.0, abs(x), abs(y)):
                return False
        elif x != y:
            return False
    return True


def compare(a, b, tol=1e-5):
    """Returns {category: [messages]} (empty lists when equal)."""
    d = {c: [] for c in CATEGORIES}
    for rel in sorted(a["files"] ^ b["files"]):
        d["files"].append("%s only in %s" % (rel, "A" if rel in a["files"] else "B"))
    for k in sorted(set(a["keys"]) ^ set(b["keys"])):
        d["keys"].append("(%s, %s) only in %s" % (k[0], k[1], "A" if k in a["keys"] else "B"))
    for k in sorted(set(a["keys"]) & set(b["keys"])):
        if a["keys"][k] != b["keys"][k]:
            d["keys"].append("(%s, %s): kind %s vs %s" % (k[0], k[1], a["keys"][k], b["keys"][k]))
    for o in sorted(set(a["assets"]) ^ set(b["assets"])):
        d["assets"].append("oaid %s only in %s" % (o, "A" if o in a["assets"] else "B"))
    for o in sorted(set(a["assets"]) & set(b["assets"])):
        if a["assets"][o] != b["assets"][o]:
            d["assets"].append("oaid %s: kind %s vs %s" % (o, a["assets"][o], b["assets"][o]))
    for rel in sorted(set(a["records"]) & set(b["records"])):
        ra, rb = a["records"][rel], b["records"][rel]
        if ra != rb:
            keys = sorted(set(ra) | set(rb))
            diff = [k for k in keys if ra.get(k) != rb.get(k)]
            detail = diff
            if "payload" in diff:
                pa, pb = ra.get("payload") or {}, rb.get("payload") or {}
                detail = [k for k in diff if k != "payload"] + \
                         ["payload." + k for k in sorted(set(pa) | set(pb)) if pa.get(k) != pb.get(k)]
            d["records"].append("%s: %s differ" % (rel, ", ".join(detail)))
    for s in sorted(a["blobs"] ^ b["blobs"]):
        d["blobs"].append("blob %s only in %s" % (s, "A" if s in a["blobs"] else "B"))
    for rel in sorted(set(a["textures"]) & set(b["textures"])):
        if a["textures"][rel] != b["textures"][rel]:
            d["textures"].append("%s: different bytes" % rel)
    for rel in sorted(set(a["usda"]) & set(b["usda"])):
        (ma, pa), (mb, pb) = a["usda"][rel], b["usda"][rel]
        if not values_equal(ma, mb, tol):
            d["attributes"].append("%s: layer metadata differs" % rel)
        for path in sorted(set(pa) ^ set(pb)):
            d["prims"].append("%s %s only in %s" % (rel, path, "A" if path in pa else "B"))
        for path in sorted(set(pa) & set(pb)):
            xa, xb = pa[path], pb[path]
            if xa["type"] != xb["type"] or xa["specifier"] != xb["specifier"]:
                d["prims"].append("%s %s: %s %s vs %s %s" % (rel, path, xa["specifier"], xa["type"], xb["specifier"], xb["type"]))
            if not values_equal(xa["metadata"], xb["metadata"], tol):
                d["attributes"].append("%s %s: prim metadata differs" % (rel, path))
            props_a, props_b = xa["properties"], xb["properties"]
            for name in sorted(set(props_a) | set(props_b)):
                cat = "transforms" if name == "xformOp:transform" else "attributes"
                if name not in props_a or name not in props_b:
                    d[cat].append("%s %s.%s only in %s" % (rel, path, name, "A" if name in props_a else "B"))
                elif not values_equal(props_a[name], props_b[name], tol):
                    d[cat].append("%s %s.%s differs" % (rel, path, name))
    return d


def cmd_compare(args):
    try:
        a, b = load_capture(args.a), load_capture(args.b)
    except (IOError, OSError, ValueError) as e:
        print("capture_diff: %s" % e)
        return 2
    d = compare(a, b, args.tol)
    total = sum(len(v) for v in d.values())
    if args.json:
        print(json.dumps({"differences": total, "categories": d}, indent=2, sort_keys=True))
    else:
        for c in CATEGORIES:
            for m in d[c][:args.max]:
                print("%-10s %s" % (c, m))
            if len(d[c]) > args.max:
                print("%-10s ... %d more" % (c, len(d[c]) - args.max))
        print("capture_diff: %s" % ("identical" if total == 0 else "%d difference(s) in %s" %
                                    (total, ", ".join(c for c in CATEGORIES if d[c]))))
    return 0 if total == 0 else 1


# ---------------------------------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------------------------------

STAGE = """#usda 1.0
(
    customLayerData = {
        string lightspeed_geometry_hash_rules = "positions,indices,geometrydescriptor"
        string lightspeed_layer_type = "capture"
    }
    defaultPrim = "RootNode"
    endTimeCode = 1
    metersPerUnit = 1
    startTimeCode = 0
    timeCodesPerSecond = 24
    upAxis = "Y"
)

def "RootNode"
{
    def Xform "lights"
    {
        def DistantLight "light_000000000000D1D1"
        {
            float inputs:angle = 2
            color3f inputs:color = (1, 1, 1)
            float inputs:intensity = 0.5
        }
    }

    def "meshes"
    {
        def Xform "mesh_00000000000000AA" (
            prepend references = @./meshes/mesh_00000000000000AA.usda@</mesh_00000000000000AA>
        )
        {
            token visibility = "invisible"
        }
    }

    def "Looks"
    {
    }

    def Xform "instances"
    {
        def Xform "inst_00000000000000AA_0" (
            prepend references = </RootNode/meshes/mesh_00000000000000AA>
        )
        {
            token visibility = "inherited"
            matrix4d xformOp:transform.timeSamples = {
                0: ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (5, 0, 0, 1) ),
                1: ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (6, 0, 0, 1) ),
            }
            uniform token[] xformOpOrder = ["xformOp:transform"]
        }
    }

    def Xform "cameras"
    {
    }
}
"""

MESH = """#usda 1.0
(
    customLayerData = {
        uint64 positions = 170
    }
    defaultPrim = "mesh_00000000000000AA"
)

def Xform "mesh_00000000000000AA"
{
    def Mesh "mesh"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        custom uniform bool remix_category:sky = 0
    }
}
"""


def _write(root, rel, data):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data.encode() if isinstance(data, str) else data)


def synthetic_capture(root):
    """A small capture in the writer's layout: stage, mesh layer, DDS, POCO record, blob and DB."""
    _write(root, "capture_selftest.usda", STAGE)
    _write(root, "meshes/mesh_00000000000000AA.usda", MESH)
    dds = b"DDS " + struct.pack("<31I", 124, 0x1007 | 0x8, 1, 1, 4, 0, 1, *([0] * 11), 32, 0x41, 0, 32,
                                0xff0000, 0xff00, 0xff, 0xff000000, 0x1000, 0, 0, 0, 0) + b"\x10\x20\x30\x40"
    _write(root, "textures/00000000000000CC.dds", dds)
    blob = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    sha = hashlib.sha256(blob).hexdigest()
    _write(root, "store/blobs/sha256/%s/%s" % (sha[:2], sha), blob)
    rec = {"schema": "fuse.poco/1", "kind": "mesh", "id": "cap/selftest/mesh_00000000000000AA",
           "payload": {"streams": [{"semantic": "Position", "format": "F32x3",
                                    "data": {"sha256": sha, "size": len(blob), "media": "fuse/mesh-stream"}}],
                       "bounds": [0, 0, 0, 1, 1, 0]}}
    _write(root, "store/poco/mesh/cap/selftest/mesh_00000000000000AA.poco.json", json.dumps(rec, indent=2))
    db = {"schema": "fuse.remaster.db-shim/1", "game_id": "selftest",
          "original_asset": [{"oaid": "11111111-1111-5111-8111-111111111111", "kind": "mesh"},
                             {"oaid": "22222222-2222-5222-8222-222222222222", "kind": "light"}],
          "hash_key": [{"algo": "remix.geom.asset", "value": "00000000000000AA", "kind": "mesh"},
                       {"algo": "remix.light", "value": "000000000000D1D1", "kind": "light"},
                       {"algo": "fuse.capture.sha256", "value": sha, "kind": "mesh"}],
          "replacement": []}
    _write(root, "store/db/remaster_db.json", json.dumps(db, indent=2))


def _edit(root, rel, fn):
    path = os.path.join(root, rel)
    with open(path, "rb") as f:
        data = f.read()
    with open(path, "wb") as f:
        f.write(fn(data))


def _edit_json(root, rel, fn):
    path = os.path.join(root, rel)
    with open(path) as f:
        doc = json.load(f)
    fn(doc)
    with open(path, "w") as f:
        json.dump(doc, f, indent=2)


def seeded_faults():
    """(name, expected category, mutation of a capture directory)."""
    db = "store/db/remaster_db.json"

    def drop_key(root):
        _edit_json(root, db, lambda d: d["hash_key"].pop(1))

    def change_key(root):
        def f(d):
            d["hash_key"][0]["value"] = "00000000000000AB"
        _edit_json(root, db, f)

    def drop_asset(root):
        _edit_json(root, db, lambda d: d["original_asset"].pop())

    def move_instance(root):
        _edit(root, "capture_selftest.usda", lambda b: b.replace(b"(6, 0, 0, 1)", b"(7, 0, 0, 1)"))

    def extra_prim(root):
        _edit(root, "capture_selftest.usda", lambda b: b.replace(
            b'    def "Looks"\n    {\n    }', b'    def "Looks"\n    {\n        def Material "mat_00000000000000CC"\n        {\n        }\n    }'))

    def change_attribute(root):
        _edit(root, "capture_selftest.usda", lambda b: b.replace(b"float inputs:angle = 2", b"float inputs:angle = 3"))

    def change_points(root):
        _edit(root, "meshes/mesh_00000000000000AA.usda", lambda b: b.replace(b"(0, 1, 0)]", b"(0, 2, 0)]"))

    def flip_dds(root):
        _edit(root, "textures/00000000000000CC.dds", lambda b: b[:-1] + bytes([b[-1] ^ 1]))

    def drop_blob(root):
        for d, _, names in os.walk(os.path.join(root, "store", "blobs")):
            for n in names:
                os.remove(os.path.join(d, n))

    def change_record(root):
        rel = "store/poco/mesh/cap/selftest/mesh_00000000000000AA.poco.json"

        def f(d):
            d["payload"]["bounds"][4] = 2
        _edit_json(root, rel, f)

    def drop_file(root):
        os.remove(os.path.join(root, "meshes", "mesh_00000000000000AA.usda"))

    return [("a dropped hash_key row", "keys", drop_key), ("a changed key value", "keys", change_key),
            ("a dropped original_asset row", "assets", drop_asset), ("a moved instance", "transforms", move_instance),
            ("an extra prim", "prims", extra_prim), ("a changed light attribute", "attributes", change_attribute),
            ("changed mesh points", "attributes", change_points), ("a flipped DDS byte", "textures", flip_dds),
            ("a deleted blob", "blobs", drop_blob), ("a changed POCO payload", "records", change_record),
            ("a deleted layer file", "files", drop_file)]


def cmd_selftest(args):
    del args
    tmp = tempfile.mkdtemp(prefix="rl_capture_diff_")
    try:
        base = os.path.join(tmp, "base")
        synthetic_capture(base)
        same = os.path.join(tmp, "same")
        shutil.copytree(base, same)
        d = compare(load_capture(base), load_capture(same))
        if any(d.values()):
            print("FAIL: identical captures differ: %s" % {k: v for k, v in d.items() if v})
            return 1
        # Float noise below the tolerance is not a difference; above it is.
        noise = os.path.join(tmp, "noise")
        shutil.copytree(base, noise)
        _edit(noise, "capture_selftest.usda", lambda b: b.replace(b"(6, 0, 0, 1)", b"(6.00000001, 0, 0, 1)"))
        d = compare(load_capture(base), load_capture(noise))
        if any(d.values()):
            print("FAIL: a 1e-8 transform change is reported: %s" % {k: v for k, v in d.items() if v})
            return 1
        if not compare(load_capture(base), load_capture(noise), tol=1e-12)["transforms"]:
            print("FAIL: a 1e-8 transform change is not reported with --tol 1e-12")
            return 1
        caught = 0
        for name, category, mutate in seeded_faults():
            case = os.path.join(tmp, "case_%d" % caught)
            shutil.copytree(base, case)
            mutate(case)
            d = compare(load_capture(base), load_capture(case))
            if not d[category]:
                print("FAIL: %s is not reported under '%s': %s" % (name, category, {k: v for k, v in d.items() if v}))
                return 1
            # A missing layer also removes its prims from the comparison, nothing else may light up.
            others = [c for c in CATEGORIES if c != category and d[c]]
            if others:
                print("FAIL: %s is also reported under %s: %s" % (name, others, {k: d[k] for k in others}))
                return 1
            caught += 1
        # The command line: exit 0 on identical, 1 on different, 2 on unreadable input.
        ns = argparse.Namespace(a=base, b=same, tol=1e-5, json=False, max=5)
        if cmd_compare(ns) != 0:
            print("FAIL: compare of identical captures does not exit 0")
            return 1
        ns.b = os.path.join(tmp, "case_0")
        if cmd_compare(ns) != 1:
            print("FAIL: compare of different captures does not exit 1")
            return 1
        broken = os.path.join(tmp, "broken")
        shutil.copytree(base, broken)
        _edit(broken, "capture_selftest.usda", lambda b: b.replace(b'def "Looks"', b'def "Looks" {'))
        ns.b = broken
        if cmd_compare(ns) != 2:
            print("FAIL: an unparsable USDA layer does not exit 2")
            return 1
        print("PASS: identical captures compare equal (float noise within --tol), %d seeded difference(s) each "
              "reported in its own category, exit codes 0 / 1 / 2" % caught)
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "selftest":
        return cmd_selftest(None)
    if argv and argv[0] == "compare":
        argv = argv[1:]
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--tol", type=float, default=1e-5)
    p.add_argument("--json", action="store_true")
    p.add_argument("--max", type=int, default=50)
    return cmd_compare(p.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
