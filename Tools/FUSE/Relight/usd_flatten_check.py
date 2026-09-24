#!/usr/bin/env python3
"""FUSE Relight RL-3.1: cross-check the Remix-profile USD reader against usd-core (plan §4.3, RL-3.1).

The reader (Source/FUSE/Relight/mods/usd) composes a mod with FUSE's own "Remix profile" composition on top of
TinyUSDZ. This script composes the same root layer with usd-core (the `pxr` wheel, TOST / Apache-2.0; an optional
offline tool, never a runtime dependency) and compares the flattened result with the reader's canonical dump
(`fuse_relight_usd_tool dump`, one JSON object per prim).

usd-core side: Usd.Stage.Open(root, LoadAll), every prim reachable through instance proxies (so instanceable
prims are flattened like the reader flattens them), inactive prims' subtrees pruned (as USD populates them).

Compared per prim: existence, specifier, type name, active, instanceable, kind, applied API schemas (as a set:
TinyUSDZ does not keep the relative order of schemas it knows and schemas it does not), variant selections,
child names (as a set), and every authored attribute (type name, uniform, custom, default value, time samples,
connections, interpolation / elementSize / colorSpace) and relationship (targets, in order). Numbers compare with
a relative tolerance (--tol, default 1e-5: usd-core reports float32 values widened to double, the reader keeps
the authored decimal text); asset paths compare by authored path and by resolved path relative to the root
layer's directory. Stage: defaultPrim and the root layer's customLayerData.

Fixtures whose reader dump reports profile gaps (inherits, specializes, value clips, relocates, ignored layer
offsets) are skipped by `fixtures`: usd-core composes those arcs and the reader deliberately does not.

  usd_flatten_check.py compare <root layer> --tool <fuse_relight_usd_tool> [--emulator a|b] [--tol 1e-5]
  usd_flatten_check.py flatten <root layer>          usd-core's side only, as JSON lines (needs pxr)
  usd_flatten_check.py fixtures --fixtures <Tests/relight/fixtures/mods> --tool <...> [--emulator a|b]
  usd_flatten_check.py selftest                      the comparison logic on synthetic dumps (no pxr needed)

Exit codes: 0 equal, 1 different, 2 usage / tool error, 77 usd-core (pxr) not installed.
Python standard library only, apart from pxr for compare / flatten / fixtures.
"""
import argparse
import json
import math
import os
import subprocess
import sys

SKIP_CODES = {"inherits", "specializes", "value_clips", "relocates", "layer_offset_ignored"}
ATTR_META = ("interpolation", "elementSize", "colorSpace")


# ---- usd-core side --------------------------------------------------------------------------------------------

def _import_pxr():
    try:
        from pxr import Gf, Sdf, Usd, Vt  # noqa: F401
        return True
    except ImportError:
        return False


def _rel_to(path, base):
    if not path or not base:
        return path or ""
    path = os.path.normpath(path).replace("\\", "/")
    base = os.path.normpath(base).replace("\\", "/")
    try:
        return os.path.relpath(path, base).replace("\\", "/")
    except ValueError:
        return path


def _to_json(v, base):
    """pxr value -> the reader's canonical JSON shape."""
    from pxr import Gf, Sdf, Vt
    if v is None:
        return None
    if isinstance(v, bool):
        return v
    if isinstance(v, (int, float)):
        if isinstance(v, float) and (math.isinf(v) or math.isnan(v)):
            return "nan" if math.isnan(v) else ("inf" if v > 0 else "-inf")
        return v
    if isinstance(v, str):
        return v
    if isinstance(v, Sdf.AssetPath):
        return {"asset": v.path, "resolved": _rel_to(v.resolvedPath, base) if v.resolvedPath else v.path}
    if isinstance(v, Sdf.Path):
        return {"path": str(v)}
    if isinstance(v, (Gf.Quatf, Gf.Quatd, Gf.Quath)):
        im = v.GetImaginary()
        return [float(v.GetReal()), float(im[0]), float(im[1]), float(im[2])]
    if isinstance(v, (Gf.Matrix2d, Gf.Matrix3d, Gf.Matrix4d, Gf.Matrix2f, Gf.Matrix3f, Gf.Matrix4f)):
        n = v.dimension[0]
        return [[float(v[r][c]) for c in range(n)] for r in range(n)]
    if isinstance(v, dict):
        return {k: _to_json(x, base) for k, x in v.items()}
    try:
        return [_to_json(x, base) for x in v]  # Vt arrays, Gf vectors, lists
    except TypeError:
        return str(v)


def usd_core_dump(root):
    """Composes `root` with usd-core; returns (stage dict, {path: prim dict})."""
    from pxr import Sdf, Usd
    stage = Usd.Stage.Open(root, Usd.Stage.LoadAll)
    if stage is None:
        raise RuntimeError("usd-core could not open " + root)
    base = os.path.dirname(os.path.abspath(root))
    layer = stage.GetRootLayer()
    stage_info = {"defaultPrim": layer.defaultPrim or "", "customLayerData": _to_json(dict(layer.customLayerData), base)}
    predicate = Usd.TraverseInstanceProxies(Usd.PrimAllPrimsPredicate)
    prims = {}
    spec_names = {Sdf.SpecifierDef: "def", Sdf.SpecifierOver: "over", Sdf.SpecifierClass: "class"}
    for prim in Usd.PrimRange(stage.GetPseudoRoot(), predicate):
        if prim.IsPseudoRoot():
            continue
        path = str(prim.GetPath())
        d = {
            "path": path,
            "specifier": spec_names.get(prim.GetSpecifier(), "def"),
            "type": str(prim.GetTypeName()),
            "active": bool(prim.IsActive()),
            "instanceable": bool(prim.IsInstanceable()),
            "kind": str(Usd.ModelAPI(prim).GetKind() or ""),
            "apiSchemas": [str(s) for s in prim.GetPrimTypeInfo().GetAppliedAPISchemas()],
            "variantSelections": {k: v for k, v in prim.GetVariantSets().GetAllVariantSelections().items() if v},
            "children": [c.GetName() for c in prim.GetFilteredChildren(predicate)] if prim.IsActive() else [],
            "attributes": {},
            "relationships": {},
        }
        for a in prim.GetAuthoredAttributes():
            ad = {
                "type": str(a.GetTypeName()),
                "custom": bool(a.IsCustom()),
                "uniform": a.GetVariability() == Sdf.VariabilityUniform,
            }
            dv = a.Get(Usd.TimeCode.Default())
            if dv is not None:
                ad["default"] = _to_json(dv, base)
            samples = a.GetTimeSamples()
            if samples:
                ad["timeSamples"] = [[t, _to_json(a.Get(t), base)] for t in samples]
            conns = a.GetConnections()
            if conns:
                ad["connections"] = [str(c) for c in conns]
            md = {}
            for key in ATTR_META:
                if a.HasAuthoredMetadata(key):
                    md[key] = _to_json(a.GetMetadata(key), base)
            if md:
                ad["metadata"] = md
            d["attributes"][a.GetName()] = ad
        for r in prim.GetAuthoredRelationships():
            d["relationships"][r.GetName()] = [str(t) for t in r.GetTargets()]
        prims[path] = d
    return stage_info, prims


# ---- reader side ----------------------------------------------------------------------------------------------

def reader_dump(root, tool, emulator):
    cmd = ([c for c in emulator.split("|") if c] if emulator else []) + [tool, "dump", root]
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if out.returncode != 0:
        raise RuntimeError("reader dump failed (%d): %s" % (out.returncode, out.stderr.decode(errors="replace")[:2000]))
    return parse_dump(out.stdout.decode("utf-8"))


def parse_dump(text):
    stage_info, prims, diags = {}, {}, []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        obj = json.loads(line)
        if "stage" in obj:
            s = obj["stage"]
            cld = s.get("customLayerData")
            stage_info = {"defaultPrim": s.get("defaultPrim", ""), "customLayerData": _strip_dict_types(cld) if cld else {}}
        elif "diagnostic" in obj:
            diags.append(obj["diagnostic"])
        else:
            prims[obj["path"]] = obj
    return stage_info, prims, diags


def _strip_dict_types(d):
    """The reader keys dictionaries "type name" as authored; usd-core by name."""
    if not isinstance(d, dict):
        return d
    return {k.split(" ", 1)[-1]: _strip_dict_types(v) for k, v in d.items()}


# ---- comparison -----------------------------------------------------------------------------------------------

def _num_eq(a, b, tol):
    if a == b:
        return True
    if isinstance(a, str) or isinstance(b, str):  # "inf" / "nan"
        return str(a) == str(b)
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))


def values_equal(a, b, tol):
    if isinstance(a, bool) or isinstance(b, bool):
        # USDA lets bools be authored as 0 / 1: only those numbers stand for a bool.
        if isinstance(a, (bool, int, float)) and isinstance(b, (bool, int, float)) and a in (0, 1) and b in (0, 1):
            return bool(a) == bool(b)
        return False
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return _num_eq(a, b, tol)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(values_equal(x, y, tol) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return set(a) == set(b) and all(values_equal(a[k], b[k], tol) for k in a)
    return a == b


def compare(reader, reference, tol=1e-5, limit=50):
    """Returns a list of difference strings (empty when equal). Both sides: (stage dict, {path: prim dict})."""
    rs, rp = reader[0], reader[1]
    us, up = reference[0], reference[1]
    diffs = []

    def add(msg):
        if len(diffs) < limit:
            diffs.append(msg)

    if rs.get("defaultPrim", "") != us.get("defaultPrim", ""):
        add("stage defaultPrim: reader %r, usd-core %r" % (rs.get("defaultPrim"), us.get("defaultPrim")))
    if not values_equal(rs.get("customLayerData") or {}, us.get("customLayerData") or {}, tol):
        add("stage customLayerData differs")
    for path in sorted(set(rp) | set(up)):
        if path not in up:
            add("%s: only in the reader" % path)
            continue
        if path not in rp:
            add("%s: only in usd-core" % path)
            continue
        a, b = rp[path], up[path]
        for key in ("specifier", "type", "active", "instanceable", "kind"):
            if a.get(key) != b.get(key):
                add("%s: %s reader %r, usd-core %r" % (path, key, a.get(key), b.get(key)))
        for key in ("apiSchemas", "children"):
            if sorted(a.get(key, [])) != sorted(b.get(key, [])):
                add("%s: %s reader %r, usd-core %r" % (path, key, a.get(key), b.get(key)))
        if a.get("variantSelections", {}) != b.get("variantSelections", {}):
            add("%s: variantSelections reader %r, usd-core %r" % (path, a.get("variantSelections"), b.get("variantSelections")))
        ra, ua = a.get("attributes", {}), b.get("attributes", {})
        for name in sorted(set(ra) | set(ua)):
            if name not in ua or name not in ra:
                add("%s.%s: only in %s" % (path, name, "the reader" if name in ra else "usd-core"))
                continue
            x, y = ra[name], ua[name]
            for key in ("type", "uniform", "custom"):
                if x.get(key) != y.get(key):
                    add("%s.%s: %s reader %r, usd-core %r" % (path, name, key, x.get(key), y.get(key)))
            if not values_equal(x.get("default"), y.get("default"), tol):
                add("%s.%s: default reader %s, usd-core %s" % (path, name, json.dumps(x.get("default"))[:200],
                                                               json.dumps(y.get("default"))[:200]))
            if not values_equal(x.get("timeSamples", []), y.get("timeSamples", []), tol):
                add("%s.%s: timeSamples differ" % (path, name))
            if x.get("connections", []) != y.get("connections", []):
                add("%s.%s: connections reader %r, usd-core %r" % (path, name, x.get("connections"), y.get("connections")))
            xm = {k: v for k, v in (x.get("metadata") or {}).items() if k in ATTR_META}
            ym = y.get("metadata") or {}
            if not values_equal(xm, ym, tol):
                add("%s.%s: metadata reader %r, usd-core %r" % (path, name, xm, ym))
        rr, ur = a.get("relationships", {}), b.get("relationships", {})
        for name in sorted(set(rr) | set(ur)):
            if rr.get(name) != ur.get(name):
                add("%s.%s: targets reader %r, usd-core %r" % (path, name, rr.get(name), ur.get(name)))
    return diffs


# ---- commands -------------------------------------------------------------------------------------------------

def cmd_compare(root, tool, emulator, tol):
    if not _import_pxr():
        print("usd_flatten_check: usd-core (pxr) is not installed; skipping")
        return 77
    rs, rp, _ = reader_dump(root, tool, emulator)
    diffs = compare((rs, rp), usd_core_dump(root), tol)
    for d in diffs:
        print("  " + d)
    print("%s: %s (%d prims)" % (root, "equal" if not diffs else "%d difference(s)" % len(diffs), len(rp)))
    return 0 if not diffs else 1


def cmd_fixtures(fixtures, tool, emulator, tol):
    if not _import_pxr():
        print("usd_flatten_check: usd-core (pxr) is not installed; skipping the cross-check")
        return 77
    failed, checked, skipped = 0, 0, 0
    for name in sorted(os.listdir(fixtures)):
        root = os.path.join(fixtures, name, "mod.usda")
        if not os.path.isfile(root):
            continue
        rs, rp, diags = reader_dump(root, tool, emulator)
        gaps = sorted({d["code"] for d in diags} & SKIP_CODES)
        if gaps:
            print("%s: skipped (outside the Remix profile: %s)" % (name, ", ".join(gaps)))
            skipped += 1
            continue
        diffs = compare((rs, rp), usd_core_dump(root), tol)
        checked += 1
        if diffs:
            failed += 1
            print("%s: %d difference(s)" % (name, len(diffs)))
            for d in diffs:
                print("  " + d)
        else:
            print("%s: equal (%d prims)" % (name, len(rp)))
    print("usd_flatten_check: %d fixtures compared, %d different, %d skipped" % (checked, failed, skipped))
    return 0 if failed == 0 and checked > 0 else 1


def cmd_flatten(root):
    if not _import_pxr():
        print("usd_flatten_check: usd-core (pxr) is not installed")
        return 77
    stage_info, prims = usd_core_dump(root)
    print(json.dumps({"stage": stage_info}))
    for path in sorted(prims):
        print(json.dumps(prims[path]))
    return 0


def selftest():
    dump = "\n".join([
        json.dumps({"stage": {"defaultPrim": "RootNode", "metadata": {}, "customLayerData": {"string lightspeed_layer_type": "replacement"}}}),
        json.dumps({"path": "/RootNode", "specifier": "def", "type": "Xform", "active": True, "instanceable": False, "kind": "",
                    "apiSchemas": ["ShapingAPI", "MaterialBindingAPI"], "variantSelections": {}, "children": ["a", "b"],
                    "attributes": {"x": {"type": "float", "custom": False, "uniform": False, "default": 0.1},
                                   "t": {"type": "asset", "custom": False, "uniform": False,
                                         "default": {"asset": "./t.dds", "resolved": "t.dds"}},
                                   "m": {"type": "float3", "custom": False, "uniform": False, "timeSamples": [[0, [1, 2, 3]]],
                                         "metadata": {"interpolation": "vertex", "displayName": "ignored"}}},
                    "relationships": {"r": ["/RootNode/a"]}}),
        json.dumps({"diagnostic": {"severity": "warning", "code": "inherits", "prim": "/RootNode", "layer": "mod.usda"}}),
    ])
    rs, rp, diags = parse_dump(dump)
    ok = True

    def expect(cond, what):
        nonlocal ok
        if not cond:
            ok = False
            print("selftest FAIL: " + what)

    expect(rs["customLayerData"] == {"lightspeed_layer_type": "replacement"}, "dictionary keys lose their type")
    expect([d["code"] for d in diags] == ["inherits"], "diagnostics parsed")
    ref_prims = json.loads(json.dumps(rp))
    ref_prims["/RootNode"]["apiSchemas"].reverse()                       # order-insensitive
    ref_prims["/RootNode"]["children"].reverse()                         # order-insensitive
    ref_prims["/RootNode"]["attributes"]["x"]["default"] = 0.10000000149011612  # float32 widened
    ref_prims["/RootNode"]["attributes"]["m"]["metadata"] = {"interpolation": "vertex"}
    ref = ({"defaultPrim": "RootNode", "customLayerData": {"lightspeed_layer_type": "replacement"}}, ref_prims)
    expect(compare((rs, rp), ref) == [], "equivalent dumps compare equal: %r" % compare((rs, rp), ref))

    seeded = {
        "value": lambda p: p["/RootNode"]["attributes"]["x"].__setitem__("default", 0.2),
        "type": lambda p: p["/RootNode"].__setitem__("type", "Scope"),
        "missing prim": lambda p: p.pop("/RootNode"),
        "attribute": lambda p: p["/RootNode"]["attributes"].pop("t"),
        "asset": lambda p: p["/RootNode"]["attributes"]["t"]["default"].__setitem__("resolved", "other/t.dds"),
        "targets": lambda p: p["/RootNode"]["relationships"].__setitem__("r", ["/RootNode/b"]),
        "samples": lambda p: p["/RootNode"]["attributes"]["m"]["timeSamples"][0].__setitem__(1, [1, 2, 4]),
        "variants": lambda p: p["/RootNode"].__setitem__("variantSelections", {"lod": "high"}),
        "uniform": lambda p: p["/RootNode"]["attributes"]["x"].__setitem__("uniform", True),
        "bool vs number": lambda p: p["/RootNode"]["attributes"]["x"].__setitem__("default", True),
    }
    for what, mutate in seeded.items():
        p = json.loads(json.dumps(ref_prims))
        mutate(p)
        expect(len(compare((rs, rp), (ref[0], p))) >= 1, "seeded difference reported: " + what)
    expect(len(compare((rs, rp), ({"defaultPrim": "Other", "customLayerData": {}}, ref_prims))) == 2, "stage differences reported")
    expect(values_equal(1, True, 1e-5) and not values_equal(0, True, 1e-5), "USDA 0/1 bools compare with true/false")
    expect(values_equal("inf", "inf", 1e-5) and not values_equal("inf", 1.0, 1e-5), "inf spelled as strings")
    print("usd_flatten_check selftest: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compare")
    c.add_argument("root")
    c.add_argument("--tool", required=True)
    c.add_argument("--emulator", default="")
    c.add_argument("--tol", type=float, default=1e-5)
    f = sub.add_parser("fixtures")
    f.add_argument("--fixtures", required=True)
    f.add_argument("--tool", required=True)
    f.add_argument("--emulator", default="")
    f.add_argument("--tol", type=float, default=1e-5)
    fl = sub.add_parser("flatten")
    fl.add_argument("root")
    sub.add_parser("selftest")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "compare":
            return cmd_compare(args.root, args.tool, args.emulator, args.tol)
        if args.cmd == "fixtures":
            return cmd_fixtures(args.fixtures, args.tool, args.emulator, args.tol)
        if args.cmd == "flatten":
            return cmd_flatten(args.root)
        return selftest()
    except (RuntimeError, OSError, ValueError) as e:
        print("usd_flatten_check: " + str(e))
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
