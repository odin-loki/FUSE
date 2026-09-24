#!/usr/bin/env python3
"""FUSE Relight RL-1.3: geometry capture vs the Python Remix hash reference, per RL-0.4 app.

  run       runs a test app under Wine through the Relight d3d9.dll / d3d8.dll with the recording tap
            (relight.tap.mode = record; rl_tap_run.run_app from Source/FUSE/Relight/tests/tap), then
            does `check` on the app's sidecar and the recorded event stream.
  check     1. converts the tap's JSON Lines into geometry_replay's input, restoring the buffer and UP
               bytes the tap only hashed from the sidecar blobs (matched by SHA-256; every bound
               stream's bytes are re-checked against the tap's SHA-256 at each draw);
            2. runs geometry_replay (the C++ GeometryCapture fed with IRelightTap events) twice: every
               hash component on JobScheduler workers, and the default generation rule inline with no
               CPU mappings (buffer shadows) and no index memoization;
            3. compares every draw of the stream with Tools/FUSE/Relight/remix_hash_ref.py computed
               from the same bytes (status, the 9 components - vertexshader (RL-1.6) from the stream's
               shader bytecode and constants with tests/vertex_capture/rl_vs_analysis.py's constant
               ranges -, index/vertex counts, min/max, topology,
               index type, position stride, the asset key and both legacy keys, texcoordIndex,
               bounding box, skinning bone count / range / hash);
            4. compares every sidecar draw of the recorded frames (the app's ground truth) with the
               tap draw at the same position of the frame: reference from the sidecar alone ==
               reference from the stream == GeometryCapture.
  selftest  the same pipeline on a synthetic sidecar + stream pair (u16/u32/non-indexed, negative base
            vertex, UP and indexed UP, dynamic DISCARD rewrites, skinning, texcoord index 1), plus
            seeded failures that must be caught. Needs no Wine (the replay tool may be native).

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb for `run`).
"""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys

# RL-1.6: the vertexshader component's constant-range analysis (Python twin of vs_hash.cpp).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "vertex_capture"))
import rl_vs_analysis  # noqa: E402

SKIP = 77
M64 = (1 << 64) - 1

CALLS = {"DrawPrimitive": 0, "DrawIndexedPrimitive": 1, "DrawPrimitiveUP": 2, "DrawIndexedPrimitiveUP": 3}
PRIMS = {"POINTLIST": 1, "LINELIST": 2, "LINESTRIP": 3, "TRIANGLELIST": 4, "TRIANGLESTRIP": 5, "TRIANGLEFAN": 6}
DECLTYPES = {"FLOAT1": 0, "FLOAT2": 1, "FLOAT3": 2, "FLOAT4": 3, "D3DCOLOR": 4, "UBYTE4": 5, "SHORT2": 6, "SHORT4": 7,
             "UBYTE4N": 8, "SHORT2N": 9, "SHORT4N": 10, "USHORT2N": 11, "USHORT4N": 12, "UDEC3": 13, "DEC3N": 14,
             "FLOAT16_2": 15, "FLOAT16_4": 16, "UNUSED": 17}
D3D_DECL_SIZE = [4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8, 0]
USAGES = {"POSITION": 0, "BLENDWEIGHT": 1, "BLENDINDICES": 2, "NORMAL": 3, "PSIZE": 4, "TEXCOORD": 5, "TANGENT": 6,
          "BINORMAL": 7, "TESSFACTOR": 8, "POSITIONT": 9, "COLOR": 10, "FOG": 11, "DEPTH": 12, "SAMPLE": 13}
TSS = {"COLOROP": 1, "COLORARG1": 2, "COLORARG2": 3, "ALPHAOP": 4, "ALPHAARG1": 5, "ALPHAARG2": 6, "BUMPENVMAT00": 7,
       "BUMPENVMAT01": 8, "BUMPENVMAT10": 9, "BUMPENVMAT11": 10, "TEXCOORDINDEX": 11, "BUMPENVLSCALE": 22,
       "BUMPENVLOFFSET": 23, "TEXTURETRANSFORMFLAGS": 24, "COLORARG0": 26, "ALPHAARG0": 27, "RESULTARG": 28,
       "CONSTANT": 32}
RS_VERTEXBLEND, RS_INDEXEDVB = 151, 167
IFMT = {"INDEX16": 101, "INDEX32": 102}
IDENTITY = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
ALL_RULE = "positions,legacypositions0,legacypositions1,texcoords,indices,legacyindices,geometrydescriptor,vertexlayout,vertexshader"
DEFAULT_RULE = "positions,indices,texcoords,geometrydescriptor,vertexlayout,vertexshader"
ASSET_RULE = "positions,indices,geometrydescriptor"
PAD = 64  # zero bytes after every stream copy (GeometryCapture pads its copies the same way)

REF = None  # remix_hash_ref module


def load_ref(path):
    global REF
    spec = importlib.util.spec_from_file_location("remix_hash_ref", path)
    REF = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(REF)
    REF.set_impl("auto")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def f32(x):
    return struct.unpack("<I", struct.pack("<f", float(x)))[0]


def tss_defaults(stage):
    d = {"COLOROP": 4 if stage == 0 else 1, "COLORARG1": 2, "COLORARG2": 1, "ALPHAOP": 2 if stage == 0 else 1,
         "ALPHAARG1": 2, "ALPHAARG2": 1, "TEXCOORDINDEX": stage, "COLORARG0": 1, "ALPHAARG0": 1, "RESULTARG": 1}
    return d


# ---------------------------------------------------------------------------------------------------
# The reference for one draw (independent of the C++ port: written from the upstream behaviour)
# ---------------------------------------------------------------------------------------------------

def args_mask(op):
    if op == 1:
        return 0
    if op in (2, 17):
        return 0b010
    if op == 3:
        return 0b100
    if op in (25, 26):
        return 0b111
    return 0b110


def select_texcoord(stages, textures, tex_types, pixel_shader):
    """processTextures: (firstStage, raw texcoord index). stages[s] = {name: value} (complete)."""
    def st(s, n):
        return stages[s].get(n, tss_defaults(s).get(n, 0))
    first = 0
    if not pixel_shader:
        bins = [None] * 16
        for s in range(8):
            if not textures.get(s):
                continue
            if st(s, "COLOROP") == 1:
                break
            used = args_mask(st(s, "COLOROP")) | args_mask(st(s, "ALPHAOP"))
            tex = 0
            for bit, (ca, aa) in enumerate((("COLORARG0", "ALPHAARG0"), ("COLORARG1", "ALPHAARG1"),
                                            ("COLORARG2", "ALPHAARG2"))):
                if (st(s, ca) & 0xF) == 2 or (st(s, aa) & 0xF) == 2:
                    tex |= 1 << bit
            if not used & tex:
                continue
            if tex_types.get(textures[s], 0) != 3:
                continue
            c = (st(s, "TEXCOORDINDEX") & 7) * 2
            sub = 0 if bins[c] is None else 1
            if bins[c + sub] is None:
                bins[c + sub] = s
        for s in bins:
            if s is not None and textures.get(s):
                first = s
                break
    return first, st(first, "TEXCOORDINDEX")


def expect_draw(d, rule_bits):
    """d: call, prim, pc, sv, bv, si, mi, nv, elements [(stream, off, type, usage, ui)],
    streams {s: (bytes, offset, stride)}, ib (bytes, fmt) | None, upv, ups, upi, upf, stages, textures,
    tex_types, vs, ps, rs {index: value}, xf {slot: [16 floats]}. Returns the replay line's fields."""
    first, tci = select_texcoord(d["stages"], d["textures"], d["tex_types"], d["ps"])
    out = {"tci": str(tci), "stage": str(first)}
    call, prim, pc = d["call"], d["prim"], d["pc"]
    indexed = call in (1, 3)
    up = call in (2, 3)
    topo, count = REF.primitive_info(prim, pc)
    if pc == 0:
        out["status"] = "no_vertices"  # makeDrawCallType skips PrimitiveCount 0
        return out

    # Vertex sources (whole buffers) and the base vertex of the draw context.
    streams = dict(d["streams"])
    if up:
        stride = d["ups"]
        nverts = d["mi"] + d["nv"] if indexed else count
        data_size = nverts * stride
        decl0 = max([e[1] + D3D_DECL_SIZE[e[2]] for e in d["elements"] if e[0] == 0] or [0])
        buf_size = (nverts - 1) * stride + max(decl0, stride) if nverts else data_size
        streams = {0: (d["upv"][:data_size], 0, stride)}
        base_vertex = 0
    else:
        base_vertex = d["sv"] if call == 0 else d["bv"]

    # Indices (rebasing happens in REF.draw_hashes; bounds and the UP read offset here).
    idx = b""
    itype = 1000165000
    if indexed:
        fmt = d["upf"] if up else d["ib"][1]
        isize = 4 if fmt == 102 else 2
        itype = 1 if isize == 4 else 0
        if up:
            scratch = d["upv"][:data_size] + bytes(buf_size - data_size) + d["upi"]
            idx = scratch[data_size:data_size + count * isize]
        else:
            ibytes = d["ib"][0]
            start = d["si"] * isize
            if start + count * isize > len(ibytes):
                out["status"] = "out_of_bounds"
                return out
            idx = ibytes[start:start + count * isize]
        if count == 0:
            out["status"] = "no_vertices"
            return out
        values = REF.read_indices(idx, isize, count)
        lo, hi = min(values), max(values)
        if lo == hi:
            out["status"] = "degenerate_indices"
            return out
        vc = hi - lo + 1
    else:
        lo = 0
        vc = count
    if vc == 0:
        out["status"] = "no_vertices"
        return out

    # processVertices: the consumed elements; the later element wins.
    targets = {}
    for (s, off, typ, usage, ui) in d["elements"]:
        if s not in streams:
            continue
        key = None
        if usage in (0, 9) and ui == 0:
            key = "pos"
        elif usage == 1 and ui == 0:
            key = "bw"
        elif usage == 2 and ui == 0:
            key = "bi"
        elif usage == 3 and ui == 0:
            key = "n"
        elif usage == 5 and tci <= 15 and ui == tci:
            key = "tc"
        elif usage == 10 and ui == 0:
            key = "c"
        if key:
            targets[key] = (s, off, typ)
    # Bounds of every stream a consumed element reads, then one padded copy per stream laid out
    # back to back in `vb` (window at vertex base_vertex + lo).
    vb = b""
    placed = {}
    for s in sorted({t[0] for t in targets.values()}):
        data, soff, stride = streams[s]
        start = soff + stride * (base_vertex + lo)
        if start < 0 or start + stride * vc > len(data):
            out["status"] = "out_of_bounds"
            return out
        window = data[start:start + stride * vc]
        tail = data[start + stride * vc:start + stride * vc + PAD]
        placed[s] = len(vb)
        vb += window + tail + bytes(PAD - len(tail))
    if "pos" not in targets:
        out["status"] = "no_position"
        return out

    def elem(key):
        t = targets.get(key)
        if t is None:
            return None
        s, off, typ = t
        # draw_hashes adds lo * stride itself: hand it vertex 0 of the draw context.
        return (placed[s] + off - lo * streams[s][2], streams[s][2], typ, s)

    elements = {k: elem(k) for k in ("pos", "tc", "n", "c")}
    r = REF.draw_hashes(prim, pc, itype if indexed else 0x7fffffff, idx, vb, elements, vs_component(d), rule_bits,
                        0x3F800000)
    if r is None:
        out["status"] = "skipped_by_reference"
        return out
    out["status"] = "captured"
    fields = r["fields"]
    out["fields"] = fields
    out["ic"], out["vc"], out["min"], out["max"] = str(r["ic"]), str(r["vc"]), str(r["min"]), str(r["max"])
    out["topo"], out["it"], out["ps"] = str(r["topo"]), str(r["it"]), str(r["ps"])
    out["raw"] = r

    # Bounding box over vc positions (MINPS / MAXPS).
    s, off, _ = targets["pos"]
    stride = streams[s][2]
    fmax = struct.unpack("<f", struct.pack("<I", 0x7F7FFFFF))[0]
    mn, mx = [fmax] * 3, [-fmax] * 3
    for v in range(vc):
        p = struct.unpack_from("<3f", vb, placed[s] + off + v * stride)
        for k in range(3):
            mn[k] = mn[k] if mn[k] < p[k] else p[k]
            mx[k] = mx[k] if mx[k] > p[k] else p[k]
    out["aabb"] = ",".join("%08x" % f32(x) for x in mn + mx)

    # processSkinning.
    out["skin"] = "-"
    vbl = d["rs"].get(RS_VERTEXBLEND, 0)
    has_bi_decl = any(e[3] == 2 for e in d["elements"])
    indexed_vb = has_bi_decl and d["rs"].get(RS_INDEXEDVB, 0) != 0
    if not d["vs"] and vbl != 0 and ((vbl != 256 and "bw" in targets) or (vbl == 256 and indexed_vb)):
        per = {256: 1, 1: 2, 2: 3, 3: 4}.get(vbl, 0)
        nb, mnb = per, 0
        if indexed_vb and "bi" in targets:
            bs, boff, _ = targets["bi"]
            bstride = streams[bs][2]
            vals = [vb[placed[bs] + boff + v * bstride + j] for v in range(vc) for j in range(per)]
            mnb, mxb = (min(vals), max(vals)) if vals else (256, -1)
            nb = mxb + 1
        mats = b"".join(struct.pack("<16f", *d["xf"].get(10 + n, IDENTITY)) for n in range(nb))
        bone_hash = REF.xxh3_64(mats[mnb * 64:]) if nb > 0 and mnb < nb else 0
        out["skin"] = "%d:%d:%d:%016x" % (nb, per, mnb, bone_hash)
    return out


def vs_constants(vs_const_f, vs_const_i, vs_const_b, swvp):
    """(f, i, b) constant bytes as D3D9 keeps them: 16 bytes per float / int register, u32 bool words."""
    nf, no = (8192, 2048) if swvp else (256, 16)
    f = bytearray(nf * 16)
    i = bytearray(no * 16)
    b = bytearray((no + 31) // 32 * 4)
    for c in vs_const_f:
        struct.pack_into("<4f", f, c["register"] * 16, *c["value"])
    for c in vs_const_i:
        struct.pack_into("<4i", i, c["register"] * 16, *c["value"])
    for c in vs_const_b:
        if c["value"]:
            b[c["register"] // 8] |= 1 << (c["register"] % 8)
    return bytes(f), bytes(i), bytes(b)


def vs_data(bytecode, state, swvp):
    """The vertexshader component's inputs of a draw: (bytecode, f, i, b, swvp), or None."""
    if not bytecode:
        return None
    f, i, b = vs_constants(state.get("vs_const_f", []), state.get("vs_const_i", []), state.get("vs_const_b", []), swvp)
    return (bytecode, f, i, b, swvp)


def vs_component(d):
    """REF.draw_hashes' vs argument (RL-1.6): a programmable VS (no POSITIONT declaration) with its
    bytecode and constants; the constant ranges come from rl_vs_analysis."""
    data = d.get("vs_data")
    if not d["vs"] or data is None or any(e[3] == 9 for e in d["elements"]):
        return None
    bc, f, i, b, swvp = data
    tokens = list(struct.unpack("<%dI" % (len(bc) // 4), bc[:len(bc) // 4 * 4]))
    valid, mf, mi, mb = rl_vs_analysis.analyze(tokens, swvp)
    if not valid:
        return None
    return (bc, f, mf, i, mi, b, mb)


def vs_tokens(data):
    """geometry_replay's vsb / vcf / vci / vcb / swvp tokens."""
    if data is None:
        return ["vsb=-"]
    bc, f, i, b, swvp = data
    vcf = ";".join("%d:%s" % (r, f[r * 16:r * 16 + 16].hex()) for r in range(len(f) // 16) if any(f[r * 16:r * 16 + 16]))
    vci = ";".join("%d:%s" % (r, ":".join(str(v) for v in struct.unpack_from("<4i", i, r * 16)))
                   for r in range(len(i) // 16) if any(i[r * 16:r * 16 + 16]))
    vcb = ",".join(str(r) for r in range(len(b) * 8) if b[r // 8] >> (r % 8) & 1)
    return ["vsb=%s" % bc.hex(), "vcf=%s" % (vcf or "-"), "vci=%s" % (vci or "-"), "vcb=%s" % (vcb or "-"),
            "swvp=%d" % (1 if swvp else 0)]


def finish_fields(exp, rule_bits):
    """The replay line fields for one generation rule (components outside the rule are 0)."""
    if exp.get("status") != "captured":
        return exp
    e = dict(exp)
    fields = [f if rule_bits >> i & 1 else 0 for i, f in enumerate(exp["fields"])]
    raw = dict(exp["raw"], fields=fields)
    e["f"] = ",".join("%016x" % x for x in fields)
    e["key"] = "%016x" % REF.combine(fields, REF.parse_rule(ASSET_RULE))
    e["leg0"] = "%016x" % REF.legacy_key(raw, REF.RULE_LEGACY0, 0)
    e["leg1"] = "%016x" % REF.legacy_key(raw, REF.RULE_LEGACY1, 0)
    return e


COMPARED = ("status", "tci", "stage", "f", "ic", "vc", "min", "max", "topo", "it", "ps", "key", "leg0", "leg1", "aabb",
            "skin")


def diff(exp, got):
    errs = []
    for k in COMPARED:
        if k not in exp and exp.get("status") != "captured" and k not in ("status", "tci", "stage"):
            continue
        if exp.get(k) != got.get(k):
            errs.append("%s: expected %s, got %s" % (k, exp.get(k), got.get(k)))
    return errs


# ---------------------------------------------------------------------------------------------------
# The tap stream -> replay script (+ the reference per draw)
# ---------------------------------------------------------------------------------------------------

class Blobs:
    def __init__(self, sidecar):
        self.raw = sidecar.get("blobs", {})
        self.cache = {}

    def get(self, key):
        if key is None:
            return None
        if key not in self.cache:
            b64 = self.raw.get(key)
            if b64 is None:
                self.cache[key] = None
            else:
                data = base64.b64decode(b64)
                if sha(data) != key:
                    raise ValueError("sidecar blob %s does not hash to its key" % key[:16])
                self.cache[key] = data
        return self.cache[key]


def hexs(data):
    return data.hex() if data else "-"


def convert_stream(lines, blobs):
    """Returns (script lines, draws); each draw = dict(n, frame, di, inputs, resolved)."""
    script, draws = [], []
    buffers, resolved = {}, {}
    tex_types, states = {}, {}
    shaders = {}  # RL-1.6: shader id -> bytecode (the tap's "shader" events)
    frame, di = 0, 0
    for line in lines:
        ev = json.loads(line)
        kind = ev.get("ev")
        if kind == "buffer_create":
            size = ev["size"]
            buffers[ev["id"]] = bytes(size)
            resolved[ev["id"]] = True
            fmt = IFMT.get(ev.get("format"), 0)
            script.append("bcreate id=%d kind=%d size=%d format=%d" % (ev["id"], 1 if ev["kind"] == "index" else 0,
                                                                        size, fmt))
        elif kind == "buffer_write":
            data = blobs.get(ev["blob"])
            bid = ev["buffer"]
            if data is None or len(data) != len(buffers.get(bid, b"")):
                resolved[bid] = False
                data = buffers.get(bid, b"")
            else:
                resolved[bid] = True
            buffers[bid] = data
            script.append("bwrite id=%d offset=%d size=%d flags=%d data=%s" % (bid, ev["offset"], ev["size"],
                                                                               ev["lock_flags"], hexs(data)))
        elif kind == "buffer_destroy":
            script.append("bdestroy id=%d" % ev["buffer"])
            buffers.pop(ev["buffer"], None)
        elif kind == "texture_create":
            tex_types[ev["id"]] = ev["type"]
            script.append("tcreate id=%d type=%d" % (ev["id"], ev["type"]))
        elif kind == "texture_upload":
            script.append("tupload id=%d face=%d level=%d" % (ev["texture"], ev["face"], ev["level"]))
        elif kind == "texture_copy":
            script.append("tcopy src=%d dst=%d method=%d" % (ev["source"], ev["destination"],
                                                              0 if ev["method"] == "UpdateTexture" else 1))
        elif kind == "state_block":
            states[ev["index"]] = ev["state"]
        elif kind == "shader":
            shaders[ev["id"]] = bytes.fromhex(ev["data"])
        elif kind == "present":
            script.append("present")
            frame += 1
            di = 0
        elif kind == "draw":
            n = len(draws)
            st = states[ev["state"]]
            ok = True
            streams = {}
            st_tokens = []
            for s in ev["streams"]:
                data = buffers.get(s["buffer"])
                if data is None or not resolved.get(s["buffer"]) or sha(data) != s["blob"]:
                    ok = False
                    data = data or b""
                streams[s["stream"]] = (data, s["offset"], s["stride"])
                st_tokens.append("%d:%d:%d:%d" % (s["stream"], s["buffer"], s["offset"], s["stride"]))
            ib = None
            if ev["index_buffer"]:
                b = ev["index_buffer"]
                data = buffers.get(b["buffer"])
                if data is None or not resolved.get(b["buffer"]) or sha(data) != b["blob"]:
                    ok = False
                    data = data or b""
                ib = (data, IFMT[b["format"]], b["buffer"])
            upv = upi = b""
            ups = upf = 0
            if ev.get("up"):
                u = ev["up"]
                upv = blobs.get(u["vertex_blob"])
                upi = blobs.get(u["index_blob"]) if u["index_blob"] else b""
                if upv is None or upi is None:
                    ok = False
                upv, upi = upv or b"", upi or b""
                ups = u["vertex_stride"]
                upf = IFMT.get(u["index_format"], 0) if u["index_format"] else 0
            elements = [(e["stream"], e["offset"], DECLTYPES[e["type"]], USAGES[e["usage"]], e["usage_index"])
                        for e in ev["elements"]]
            stages = {}
            for t in st["texture_stages"]:
                stages[t["stage"]] = dict(tss_defaults(t["stage"]), **t["states"])
            for s in range(8):
                stages.setdefault(s, tss_defaults(s))
            textures = {t["stage"]: t["texture"] for t in st["textures"]}
            rs = {RS_VERTEXBLEND: st["render_states"].get("VERTEXBLEND", 0),
                  RS_INDEXEDVB: st["render_states"].get("INDEXEDVERTEXBLENDENABLE", 0)}
            xf = {}
            for name, m in ev.get("transforms", {}).items():
                xf[transform_slot(name)] = m
            inputs = dict(call=CALLS[ev["call"]], prim=PRIMS[ev["primitive"]], pc=ev["prim_count"],
                          sv=ev["start_vertex"], bv=ev["base_vertex"], si=ev["start_index"], mi=ev["min_index"],
                          nv=ev["num_vertices"], elements=elements, streams=streams,
                          ib=(ib[0], ib[1]) if ib else None, upv=upv, ups=ups, upi=upi, upf=upf, stages=stages,
                          textures=textures, tex_types=dict(tex_types), vs=bool(ev["vertex_shader"]),
                          ps=bool(ev["pixel_shader"]), rs=rs, xf=xf, vs_data=None)
            if ev["vertex_shader"]:
                vsref = ev["vertex_shader"]
                bc = shaders.get(vsref["id"]) or blobs.get(vsref.get("blob"))
                if bc is not None and hashlib.sha256(bc).hexdigest() != vsref.get("blob"):
                    bc = None
                inputs["vs_data"] = vs_data(bc, st, bool(st.get("software_vp")))
            tokens = ["draw", "n=%d" % n, "call=%d" % inputs["call"], "prim=%d" % inputs["prim"], "pc=%d" % inputs["pc"],
                      "sv=%d" % inputs["sv"], "bv=%d" % inputs["bv"], "mi=%d" % inputs["mi"], "nv=%d" % inputs["nv"],
                      "si=%d" % inputs["si"], "ib=%s" % ("%d:%d" % (ib[2], ib[1]) if ib else "-"),
                      "st=%s" % (";".join(st_tokens) or "-"),
                      "el=%s" % (";".join("%d:%d:%d:0:%d:%d" % (s, o, t, u, i) for (s, o, t, u, i) in elements) or "-"),
                      "fvf=%d" % (ev.get("fvf") or 0),
                      "rs=%s" % ",".join("%d:%d" % kv for kv in sorted(rs.items())),
                      "tss=%s" % (",".join("%d:%d:%d" % (s, TSS[name], v) for s, d in sorted(stages.items())
                                           for name, v in sorted(d.items()) if name in TSS) or "-"),
                      "tex=%s" % (",".join("%d:%d" % kv for kv in sorted(textures.items())) or "-"),
                      "vs=%d" % (ev["vertex_shader"]["id"] if ev["vertex_shader"] else 0),
                      "ps=%d" % (ev["pixel_shader"]["id"] if ev["pixel_shader"] else 0),
                      "xf=%s" % (";".join("%d:%s" % (slot, struct.pack("<16f", *m).hex()) for slot, m in sorted(xf.items()))
                                 or "-"),
                      "upv=%s" % hexs(upv), "ups=%d" % ups, "upi=%s" % hexs(upi), "upf=%d" % upf] + \
                vs_tokens(inputs["vs_data"])
            script.append(" ".join(tokens))
            draws.append(dict(n=n, frame=frame, di=di, inputs=inputs, resolved=ok))
            di += 1
    return script, draws


def transform_slot(name):
    if name == "VIEW":
        return 0
    if name == "PROJECTION":
        return 1
    if name.startswith("TEXTURE"):
        return 2 + int(name[7:])
    if name == "WORLD":
        return 10
    if name.startswith("WORLDMATRIX_"):
        return 10 + int(name[12:])
    raise ValueError(name)


def sidecar_inputs(sc, draw, blobs, tap_elements=None, tap_vs=None):
    """The same inputs as convert_stream, from the sidecar alone (the app's ground truth).
    D3D8 programmable draws: d3d8 turns the declaration's input registers (v0, v1, ...) into D3D9
    usages by register number (D3D8_VERTEX_INPUT_REGISTERS), so the sidecar's usages are not what
    D3D9 / Remix see; the layout must match the tap's and the usages are taken from the tap."""
    bufs = {b["id"]: b for b in sc["buffers"]}
    # Sidecar texture ids start at 0; shift them so 0 keeps meaning "nothing bound".
    tex_types = {t["id"] + 1: 3 for t in sc.get("textures", []) if t.get("kind") == "texture"}
    st = sc["state_blocks"][draw["state"]]

    def version_bytes(buf_id, version):
        return blobs.get(bufs[buf_id]["versions"][version]["blob"])

    streams = {s["stream"]: (version_bytes(s["buffer"], s["version"]), s["offset"], s["stride"])
               for s in draw.get("streams", [])}
    ib = None
    if draw.get("index_buffer"):
        b = draw["index_buffer"]
        ib = (version_bytes(b["buffer"], b["version"]), IFMT[b["format"]])
    upv = upi = b""
    ups = upf = 0
    if draw.get("up"):
        u = draw["up"]
        upv = blobs.get(u["vertex_blob"]) or b""
        upi = blobs.get(u["index_blob"]) if u.get("index_blob") else b""
        ups = u["vertex_stride"]
        upf = IFMT.get(u.get("index_format"), 0) if u.get("index_format") else 0
    elements = [(e["stream"], e["offset"], DECLTYPES[e["type"]], USAGES[e["usage"]], e["usage_index"])
                for e in draw["vertex_format"]["elements"]]
    if sc.get("api") == "d3d8" and st.get("vertex_shader") is not None and tap_elements is not None:
        if [e[:3] for e in elements] != [e[:3] for e in tap_elements]:
            raise ValueError("d3d8 programmable draw: sidecar layout %s != tap layout %s" % (elements, tap_elements))
        elements = list(tap_elements)
    stages = {s: tss_defaults(s) for s in range(8)}
    for t in st.get("texture_stages", []):
        stages[t["stage"]] = dict(stages[t["stage"]], **t["states"])
    textures = {t["stage"]: t["texture"] + 1 for t in st.get("textures", []) if t.get("texture") is not None}
    rs = {RS_VERTEXBLEND: st["render_states"].get("VERTEXBLEND", 0),
          RS_INDEXEDVB: st["render_states"].get("INDEXEDVERTEXBLENDENABLE", 0)}
    xf = {transform_slot(k): v for k, v in draw.get("transforms", {}).items()}
    return dict(call=CALLS[draw["call"]], prim=PRIMS[draw["primitive"]], pc=draw["prim_count"],
                sv=draw.get("start_vertex", 0), bv=draw.get("base_vertex", 0), si=draw.get("start_index", 0),
                mi=draw.get("min_index", 0), nv=draw.get("num_vertices", 0), elements=elements, streams=streams, ib=ib,
                upv=upv, ups=ups, upi=upi, upf=upf, stages=stages, textures=textures, tex_types=tex_types,
                vs=st.get("vertex_shader") is not None, ps=st.get("pixel_shader") is not None, rs=rs, xf=xf,
                vs_data=sidecar_vs_data(sc, st, blobs, tap_vs))


def sidecar_vs_data(sc, st, blobs, tap_vs):
    """RL-1.6: the sidecar's vertex shader and constants. D3D8 twins: d3d8 translates the shader, so
    the bytecode D3D9 hashes is the tap's (tap_vs); the constants are the app's."""
    vs_id = st.get("vertex_shader")
    if vs_id is None:
        return None
    swvp = sc.get("device", {}).get("vertex_processing") == "software"
    if sc.get("api") == "d3d8":
        return vs_data(tap_vs[0], st, swvp) if tap_vs else None
    shader = next((x for x in sc.get("shaders", []) if x["id"] == vs_id), None)
    return vs_data(blobs.get(shader["blob"]) if shader else None, st, swvp)


# ---------------------------------------------------------------------------------------------------
# Replay + comparison
# ---------------------------------------------------------------------------------------------------

def run_replay(replay_cmd, script_path, extra):
    proc = subprocess.run(replay_cmd + [script_path] + extra, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = proc.stdout.decode(errors="replace")
    if proc.returncode != 0:
        raise RuntimeError("geometry_replay failed (%d): %s" % (proc.returncode, proc.stderr.decode(errors="replace")[-2000:]))
    draws, stats = {}, ""
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("draw "):
            kv = dict(tok.split("=", 1) for tok in line.split()[1:])
            draws[int(kv["n"])] = kv
        elif line.startswith("stats "):
            stats = line
    return draws, stats


def check(sidecar_path, stream_path, replay_cmd, work, corrupt=None):
    """Returns (errors, summary)."""
    with open(sidecar_path) as f:
        sc = json.load(f)
    with open(stream_path) as f:
        lines = [l for l in f if l.strip()]
    blobs = Blobs(sc)
    script, draws = convert_stream(lines, blobs)
    if corrupt:
        script = corrupt(script)
    os.makedirs(work, exist_ok=True)
    script_path = os.path.join(work, "replay.txt")
    with open(script_path, "w") as f:
        f.write("\n".join(script) + "\n")

    all_bits = REF.parse_rule(ALL_RULE)
    def_bits = REF.parse_rule(DEFAULT_RULE)
    runs = [("all components, JobScheduler", ["--rule", ALL_RULE], all_bits),
            ("default rule, inline, shadows, no memoization", ["--rule", DEFAULT_RULE, "--sync", "--no-mapping", "--no-memo"],
             def_bits)]
    errors = []
    outputs = []
    stats = []
    for name, extra, _ in runs:
        got, st = run_replay(replay_cmd, script_path, extra)
        outputs.append(got)
        stats.append(st)

    compared = unresolved = captured = 0
    kinds = set()
    expected_by_n = {}
    for d in draws:
        if not d["resolved"]:
            unresolved += 1
            continue
        exp = expect_draw(d["inputs"], all_bits)
        expected_by_n[d["n"]] = exp
        compared += 1
        if exp.get("status") == "captured":
            captured += 1
            i = d["inputs"]
            kinds.add({0: "non-indexed", 1: "u16" if i["ib"] and i["ib"][1] == 101 else "u32", 2: "UP",
                       3: "indexed UP"}[i["call"]])
            if i["call"] == 1 and i["bv"] < 0:
                kinds.add("negative base vertex")
            if exp["skin"] != "-":
                kinds.add("skinned")
        for (name, _, bits), got in zip(runs, outputs):
            g = got.get(d["n"])
            if g is None:
                errors.append("draw %d: missing from the replay output (%s)" % (d["n"], name))
                continue
            e = finish_fields(exp, bits)
            e = dict(e, frame=str(d["frame"]), di=str(d["di"]))
            errs = diff(e, g)
            for k in ("frame", "di"):
                if e[k] != g.get(k):
                    errs.append("%s: expected %s, got %s" % (k, e[k], g.get(k)))
            for err in errs:
                errors.append("draw %d (frame %d, #%d, %s): %s" % (d["n"], d["frame"], d["di"], name, err))

    # Ground truth: the sidecar's recorded frames.
    truth = 0
    for fr in sc.get("recorded_frames", []):
        sc_draws = sorted((x for x in sc["draws"] if x["frame"] == fr), key=lambda x: x["seq"])
        tap_draws = [d for d in draws if d["frame"] == fr]
        if len(sc_draws) != len(tap_draws):
            errors.append("frame %d: %d sidecar draws, %d tap draws" % (fr, len(sc_draws), len(tap_draws)))
            continue
        for sd, td in zip(sc_draws, tap_draws):
            try:
                exp = expect_draw(sidecar_inputs(sc, sd, blobs, td["inputs"]["elements"], td["inputs"]["vs_data"]),
                                  all_bits)
            except ValueError as e:
                errors.append("sidecar frame %d seq %d: %s" % (fr, sd["seq"], e))
                continue
            e = finish_fields(exp, all_bits)
            g = outputs[0].get(td["n"])
            where = "sidecar frame %d seq %d (tap draw %d)" % (fr, sd["seq"], td["n"])
            if td["n"] in expected_by_n:
                for err in diff(e, finish_fields(expected_by_n[td["n"]], all_bits)):
                    errors.append("%s: sidecar vs stream reference: %s" % (where, err))
            if g is None:
                errors.append("%s: missing from the replay output" % where)
                continue
            for err in diff(e, g):
                errors.append("%s: %s" % (where, err))
            truth += 1
    summary = ("%d draws (%d captured: %s), %d unresolved, %d sidecar draws of frames %s match; replay %s"
               % (compared, captured, ", ".join(sorted(kinds)) or "-", unresolved, truth,
                  sc.get("recorded_frames", []), stats[0][6:] if stats and stats[0] else "-"))
    if compared == 0:
        errors.append("no draw could be compared")
    return errors, summary


# ---------------------------------------------------------------------------------------------------
# Synthetic self-test
# ---------------------------------------------------------------------------------------------------

def synthetic_pair():
    """A sidecar + tap stream pair exercising every draw kind, built like RL-0.4 / RL-1.1 write them."""
    import random
    rnd = random.Random(1234)
    blobs, sc_buffers, stream, sc_draws, sc_states = {}, [], [], [], []

    def blob(data):
        k = sha(data)
        blobs[k] = base64.b64encode(data).decode()
        return k

    def floats(n):
        return struct.pack("<%df" % n, *[rnd.uniform(-9, 9) for _ in range(n)])

    stream.append({"ev": "header", "schema": "fuse.relight.tap_events/1", "interface_version": 1})
    tap_ids = {}

    def buffer(sc_id, kind, size, fmt=None):
        tap_ids[sc_id] = len(tap_ids) + 1
        b = {"id": sc_id, "kind": kind, "size": size, "versions": [], "shadow": bytearray(size)}
        if fmt:
            b["format"] = fmt
        sc_buffers.append(b)
        ev = {"ev": "buffer_create", "frame": 0, "id": tap_ids[sc_id], "kind": kind, "size": size, "usage": 0,
              "pool": "MANAGED"}
        if fmt:
            ev["format"] = fmt
        stream.append(ev)
        return b

    def write(b, offset, data, flags=0, frame=0):
        if flags & 0x2000:
            b["shadow"][:] = bytes(len(b["shadow"]))
        b["shadow"][offset:offset + len(data)] = data
        k = blob(bytes(b["shadow"]))
        b["versions"].append({"frame": frame, "offset": offset, "size": len(data), "lock_flags": flags, "blob": k})
        stream.append({"ev": "buffer_write", "frame": frame, "buffer": tap_ids[b["id"]], "offset": offset,
                       "size": len(data), "lock_flags": flags, "blob": k})
        return len(b["versions"]) - 1

    stream.append({"ev": "texture_create", "frame": 0, "id": 1, "type": 3})
    # Vertex formats.
    pos_col = [("POSITION", "FLOAT3", 0, 0), ("COLOR", "D3DCOLOR", 12, 0)]  # stride 16
    two_tc = [("POSITION", "FLOAT3", 0, 0), ("TEXCOORD", "FLOAT2", 12, 0), ("TEXCOORD", "FLOAT2", 20, 1)]  # 28
    skinned = [("POSITION", "FLOAT3", 0, 0), ("BLENDWEIGHT", "FLOAT2", 12, 0), ("BLENDINDICES", "UBYTE4", 20, 0),
               ("NORMAL", "FLOAT3", 24, 0)]  # 36

    def elems(fmt):
        return [{"stream": 0, "offset": o, "type": t, "size": D3D_DECL_SIZE[DECLTYPES[t]], "method": 0, "usage": u,
                 "usage_index": i} for (u, t, o, i) in fmt]

    vb_static = buffer(0, "vertex", 28 * 24)
    write(vb_static, 0, floats(7 * 24))
    ib16 = buffer(1, "index", 2 * 48, "INDEX16")
    write(ib16, 0, struct.pack("<48H", *[rnd.randrange(8, 20) for _ in range(48)]))
    ib32 = buffer(2, "index", 4 * 12, "INDEX32")
    write(ib32, 0, struct.pack("<12I", *[rnd.randrange(0, 16) for _ in range(12)]))
    vb_dyn = buffer(3, "vertex", 16 * 16)
    vb_skin = buffer(4, "vertex", 36 * 8)
    sk = bytearray(floats(9 * 8))
    for v in range(8):
        struct.pack_into("<4B", sk, v * 36 + 20, 3 + v % 3, 4 + v % 2, 7, 0)
    write(vb_skin, 0, bytes(sk))

    def state(textures=(), stages=(), rs=None):
        sc_states.append({"render_states": dict(rs or {}), "texture_stages": [{"stage": s, "states": d} for s, d in stages],
                          "textures": [{"stage": s, "texture": t} for s, t in textures], "vertex_shader": None,
                          "pixel_shader": None})
        full = {"render_states": {"VERTEXBLEND": (rs or {}).get("VERTEXBLEND", 0),
                                  "INDEXEDVERTEXBLENDENABLE": (rs or {}).get("INDEXEDVERTEXBLENDENABLE", 0)},
                "texture_stages": [], "textures": [{"stage": s, "texture": 1} for s, _ in textures]}
        for s in range(8):
            d = tss_defaults(s)
            d.update(dict(stages).get(s, {}))
            full["texture_stages"].append({"stage": s, "states": d})
        stream.append({"ev": "state_block", "index": len(sc_states) - 1, "state": full})
        return len(sc_states) - 1

    plain = state()
    tc1 = state(textures=[(0, 7)], stages=[(0, {"TEXCOORDINDEX": 1})])
    skin_state = state(rs={"VERTEXBLEND": 2, "INDEXEDVERTEXBLENDENABLE": 1})

    frame = 0
    seq = [0]

    def draw(call, prim, pc, fmt, st, streams=(), ib=None, up=None, xf=None, **kw):
        seq[0] += 1
        sd = {"frame": frame, "seq": seq[0], "call": call, "primitive": prim, "prim_count": pc, "state": st,
              "vertex_format": {"fvf": 0, "declaration": None, "elements": elems(fmt)},
              "streams": [{"stream": 0, "buffer": b["id"], "version": len(b["versions"]) - 1, "offset": o,
                           "stride": s} for (b, o, s) in streams],
              "index_buffer": {"buffer": ib["id"], "version": len(ib["versions"]) - 1, "format": ib["format"]}
              if ib else None, "transforms": xf or {}}
        sd.update(kw)
        if up:
            sd["up"] = up
        sc_draws.append(sd)
        ev = {"ev": "draw", "frame": frame, "call": call, "primitive": prim, "prim_count": pc,
              "start_vertex": kw.get("start_vertex", 0), "vertex_count": 0, "base_vertex": kw.get("base_vertex", 0),
              "min_index": kw.get("min_index", 0), "num_vertices": kw.get("num_vertices", 0),
              "start_index": kw.get("start_index", 0), "index_count": 0, "instance_count": 1,
              "index_buffer": {"buffer": tap_ids[ib["id"]], "format": ib["format"], "blob": sha(bytes(ib["shadow"]))}
              if ib else None, "elements": elems(fmt), "fvf": 0,
              "streams": [{"stream": 0, "buffer": tap_ids[b["id"]], "offset": o, "stride": s, "frequency": 1,
                           "blob": sha(bytes(b["shadow"]))} for (b, o, s) in streams],
              "transforms": xf or {}, "vertex_shader": None, "pixel_shader": None, "state": st, "decision": "raster"}
        if up:
            ev["up"] = up
        stream.append(ev)

    for frame in range(2):
        # Dynamic buffer rewritten with DISCARD each frame, then drawn at a negative base vertex.
        write(vb_dyn, 0, floats(4 * 16), 0x2000, frame)
        draw("DrawIndexedPrimitive", "TRIANGLELIST", 16, two_tc, tc1, [(vb_static, 28 * 4, 28)], ib16,
             base_vertex=-4, min_index=8, num_vertices=12, start_index=0, index_count=48)
        draw("DrawIndexedPrimitive", "TRIANGLELIST", 4, pos_col, plain, [(vb_dyn, 0, 16)], ib32,
             base_vertex=0, min_index=0, num_vertices=16, start_index=0, index_count=12)
        draw("DrawPrimitive", "TRIANGLESTRIP", 6, pos_col, plain, [(vb_dyn, 16, 16)], start_vertex=2, vertex_count=8)
        draw("DrawIndexedPrimitive", "TRIANGLELIST", 2, pos_col, plain, [(vb_dyn, 0, 16)], ib16,
             base_vertex=-8, min_index=8, num_vertices=8, start_index=42, index_count=6)
        upv = floats(4 * 6)
        draw("DrawPrimitiveUP", "TRIANGLEFAN", 4, pos_col, plain,
             up={"vertex_stride": 16, "vertex_blob": blob(upv), "index_blob": None, "index_format": None}, vertex_count=6)
        upv2 = floats(4 * 7)
        upi = struct.pack("<6H", 3, 4, 5, 5, 6, 3)
        draw("DrawIndexedPrimitiveUP", "TRIANGLELIST", 2, pos_col, plain,
             up={"vertex_stride": 16, "vertex_blob": blob(upv2), "index_blob": blob(upi), "index_format": "INDEX16"},
             min_index=3, num_vertices=4, index_count=6)
        mats = {"WORLD": [float(v) for v in range(16)], "WORLDMATRIX_3": [0.5] * 16, "WORLDMATRIX_7": [-0.25] * 16}
        draw("DrawPrimitive", "TRIANGLESTRIP", 6, skinned, skin_state, [(vb_skin, 0, 36)], xf=mats, start_vertex=0,
             vertex_count=8)
        stream.append({"ev": "present", "frame": frame})
    for b in sc_buffers:
        del b["shadow"]
    sidecar = {"schema": "fuse.relight.app_sidecar/1", "app": "synthetic", "recorded_frames": [1],
               "buffers": sc_buffers, "textures": [{"id": 7, "kind": "texture"}], "state_blocks": sc_states,
               "draws": [d for d in sc_draws if d["frame"] == 1], "blobs": blobs}
    return sidecar, [json.dumps(e) for e in stream]


def cmd_selftest(args):
    work = args.out
    os.makedirs(work, exist_ok=True)
    sidecar, lines = synthetic_pair()
    sc_path, st_path = os.path.join(work, "synthetic.json"), os.path.join(work, "synthetic.jsonl")
    with open(sc_path, "w") as f:
        json.dump(sidecar, f)
    with open(st_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    replay = args.replay_cmd
    failed = False
    errors, summary = check(sc_path, st_path, replay, os.path.join(work, "good"))
    if errors:
        failed = True
        print("FAIL: synthetic pair:")
        for e in errors[:40]:
            print("  " + e)
    else:
        print("PASS: synthetic pair: " + summary)
        for need in ("u16", "u32", "non-indexed", "UP", "indexed UP", "negative base vertex", "skinned"):
            if need not in summary:
                print("FAIL: the synthetic pair does not cover " + need)
                failed = True

    # Seeded failures: a flipped vertex byte, a changed index, a wrong texcoord stage state.
    def flip_vertex(script):
        out = list(script)
        for i, l in enumerate(out):
            if l.startswith("bwrite id=5 "):  # the skinned buffer (tap id 5)
                head, data = l.rsplit("data=", 1)
                out[i] = head + "data=" + ("%02x" % (int(data[:2], 16) ^ 1)) + data[2:]
                break
        return out

    def change_tci(script):
        return [l.replace("0:11:1,", "0:11:0,") if l.startswith("draw ") else l for l in script]

    def drop_draw(script):
        out, dropped = [], False
        for l in script:
            if l.startswith("draw ") and not dropped:
                dropped = True
                continue
            out.append(l)
        return out

    for name, corrupt in (("flipped vertex byte", flip_vertex), ("texcoord index", change_tci),
                          ("missing draw", drop_draw)):
        try:
            errors, _ = check(sc_path, st_path, replay, os.path.join(work, "bad"), corrupt=corrupt)
        except RuntimeError as e:
            errors = [str(e)]
        if not errors:
            print("FAIL: seeded difference not caught: " + name)
            failed = True
        else:
            print("PASS: seeded difference caught: %s (%s)" % (name, errors[0][:120]))
    return 1 if failed else 0


def cmd_check(args):
    errors, summary = check(args.sidecar, args.stream, args.replay_cmd, args.out)
    return report(args.app or os.path.basename(args.sidecar), errors, summary)


def report(app, errors, summary):
    if errors:
        print("FAIL: %s: %d difference(s)" % (app, len(errors)))
        for e in errors[:60]:
            print("  " + e)
        return 1
    print("PASS: %s: %s" % (app, summary))
    return 0


def cmd_run(args):
    sys.path.insert(0, args.tap_tools)
    sys.dont_write_bytecode = True  # no __pycache__ in RL-1.1's source directory
    import rl_tap_run  # noqa: E402  (RL-1.1's Wine driver: run directory, DLLs, environment)
    if not __import__("shutil").which("wine") and not __import__("shutil").which("wine64"):
        print("SKIP: wine not installed")
        return SKIP
    ns = argparse.Namespace(d3d9=args.d3d9, d3d8=args.d3d8, runner=args.runner, prefix_root=args.prefix_root)
    run_dir = os.path.join(args.out, "run")
    rc, text = rl_tap_run.run_app(ns, args.exe, run_dir, {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "record",
                                                          "FUSE_RELIGHT_TAP_RECORD_PATH": "relight_tap.jsonl"})
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(text.strip()[-4000:])
        print("FAIL: %s: exited with %d" % (args.app, rc))
        return 1
    sidecar = os.path.join(run_dir, args.app + ".json")
    stream = os.path.join(run_dir, "relight_tap.jsonl")
    for p in (sidecar, stream):
        if not os.path.isfile(p):
            print("FAIL: %s: missing %s" % (args.app, os.path.basename(p)))
            return 1
    errors, summary = check(sidecar, stream, args.replay_cmd, os.path.join(args.out, "check"))
    return report(args.app, errors, summary)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ref", required=True, help="Tools/FUSE/Relight/remix_hash_ref.py")
    p.add_argument("--replay", required=True, help="geometry_replay executable")
    p.add_argument("--emulator", action="append", default=[],
                   help="command prefix to run the replay, one argument per option (Wine runner)")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", required=True)
    r.add_argument("--app", required=True)
    r.add_argument("--d3d9", required=True)
    r.add_argument("--d3d8", required=True)
    r.add_argument("--runner", required=True)
    r.add_argument("--prefix-root", required=True)
    r.add_argument("--tap-tools", required=True)
    r.add_argument("--out", required=True)
    c = sub.add_parser("check")
    c.add_argument("--sidecar", required=True)
    c.add_argument("--stream", required=True)
    c.add_argument("--app")
    c.add_argument("--out", required=True)
    s = sub.add_parser("selftest")
    s.add_argument("--out", required=True)
    args = p.parse_args()
    load_ref(args.ref)
    args.replay_cmd = list(args.emulator) + [args.replay]
    return {"run": cmd_run, "check": cmd_check, "selftest": cmd_selftest}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
