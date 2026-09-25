#!/usr/bin/env python3
"""FUSE Relight RL-1.6: vertex capture test driver (see CMakeLists.txt).

  spirv-val  the SPIR-V pass (fuse_relight_vertex_capture_tests --emit) on the glslang-compiled test
             shaders and the built-in variants; spirv-val must accept every output (SPIR-V 1.0 inputs
             against the Vulkan 1.0 rules, the rest against Vulkan 1.3).
  twin       2000 random D3D9 vertex shaders: the C++ constant-range analysis (--analyze) equals the
             Python twin rl_vs_analysis.py.
  run        an RL-0.4 app (vs_sm2 / vs_sm3) under Wine through the Relight d3d9.dll:
               1. FUSE_RELIGHT=0 (reference) and relight.tap.mode = capture (vertex capture on, every
                  SPIR-V module DXVK hands to the pass dumped, FUSE_RELIGHT_VC_DUMP): the back buffer
                  dumps are bit-identical;
               2. every dumped module (the dxbc-spirv input and the pass's output) passes spirv-val;
               3. every draw of the sidecar's recorded frames that has a vertex shader has a
                  "vertex_capture" in the capture record, and every vertex the draw references was
                  written; per vertex: the CPU back-transform (pos) is within 1e-4 of the vertex
                  buffer's object-space position, the captured clip position is within 1e-4 of the
                  CPU transform by the app's VS constants (m4x4 oPos, v0, c0), an independent double
                  precision back-transform of that clip position (Remix's math, the draw's
                  WORLD / VIEW / PROJECTION) is within 1e-4 of pos, and texcoord, normal and colour
                  match what the shader computes;
               4. the vertexshader hash component of those draws equals remix_hash_ref.py's
                  vertex_shader with rl_vs_analysis.py's constant ranges (non-zero), and 0 for the
                  fixed-function draws.

Exit codes: 0 pass, 1 fail, 77 skip (no spirv-val for spirv-val / run, no Wine / Xvfb for run).
"""
import argparse
import base64
import glob
import hashlib
import importlib.util
import json
import os
import random
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rl_vs_analysis  # noqa: E402

SKIP = 77
TOL = 1e-4


def tool_cmd(args):
    return list(args.emulator or []) + [args.tool]


# ---------------------------------------------------------------------------------------------------
# spirv-val
# ---------------------------------------------------------------------------------------------------

def spirv_version(path):
    with open(path, "rb") as f:
        head = f.read(8)
    return struct.unpack("<2I", head)[1] if len(head) == 8 else 0


def validate(spirv_val, path):
    env = "vulkan1.0" if spirv_version(path) < 0x00010300 else "vulkan1.3"
    proc = subprocess.run([spirv_val, "--target-env", env, path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode == 0, env, proc.stdout.decode(errors="replace").strip()


def cmd_spirv_val(args):
    if not args.spirv_val or not os.path.isfile(args.spirv_val):
        print("SKIP: spirv-val not found")
        return SKIP
    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out)
    proc = subprocess.run(tool_cmd(args) + ["--emit", args.out] + list(args.inputs), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    print(proc.stdout.decode(errors="replace").strip())
    if proc.returncode != 0:
        print("FAIL: the pass rejected a module")
        return 1
    outputs = sorted(glob.glob(os.path.join(args.out, "*.spv")))
    expected = 7 + len(args.inputs)
    failed = 0
    if len(outputs) != expected:
        print("FAIL: %d outputs, expected %d" % (len(outputs), expected))
        failed += 1
    for path in outputs:
        ok, env, text = validate(args.spirv_val, path)
        print("%s %s (%s)%s" % ("ok  " if ok else "FAIL", os.path.basename(path), env, "" if ok else "\n" + text))
        failed += 0 if ok else 1
    if not args.inputs:
        print("note: no glslang test shaders (glslangValidator not found at configure time)")
    print("%s: %d modules validated" % ("FAIL" if failed else "PASS", len(outputs)))
    return 1 if failed else 0


# ---------------------------------------------------------------------------------------------------
# twin
# ---------------------------------------------------------------------------------------------------

def reg(t, num):
    return 0x80000000 | ((t & 7) << 28) | ((t & 0x18) << 8) | (num & 0x7FF)


def random_shader(rnd):
    """A random (mostly valid) D3D9 vertex shader token stream exercising every rule."""
    major = rnd.choice([1, 2, 2, 3, 3])
    minor = 1 if major == 1 else 0
    toks = [0xFFFE0000 | (major << 8) | minor]
    const_types = [2, 2, 2, 7, 14, 11] if major >= 2 else [2, 2, 2]
    other_types = [0, 1, 3, 15, 19]
    O = rl_vs_analysis.OPS
    alu = ["MOV", "ADD", "SUB", "MAD", "MUL", "RCP", "RSQ", "DP3", "DP4", "MIN", "MAX", "SLT", "SGE", "EXP", "LOG", "LIT",
           "DST", "LRP", "FRC", "M4x4", "M4x3", "M3x4", "M3x3", "M3x2", "POW", "CRS", "SGN", "ABS", "NRM", "SINCOS",
           "MOVA", "EXPP", "LOGP", "CMP", "DP2ADD", "SETP", "TEXLDL"]
    flow = ["LOOP", "ENDLOOP", "REP", "ENDREP", "IF", "IFC", "ELSE", "ENDIF", "BREAK", "BREAKC", "CALL", "CALLNZ",
            "LABEL", "RET", "NOP"]
    for _ in range(rnd.randint(0, 40)):
        kind = rnd.random()
        if kind < 0.12:
            t = rnd.choice([2, 7, 14]) if major >= 2 else 2
            op = {2: "DEF", 7: "DEFI", 14: "DEFB"}[t]
            params = [reg(t, rnd.randint(0, 20)) | (0xF << 16)]
            nlit = {2: 4, 7: 4, 14: 1}[t]
            params += [rnd.getrandbits(32) for _ in range(nlit)]
        elif kind < 0.17:
            body = [rnd.getrandbits(32) for _ in range(rnd.randint(0, 4))]
            toks.append(0xFFFE | (len(body) << 16))
            toks += body
            continue
        elif kind < 0.22:
            op = "DCL"
            params = [0x80000000 | rnd.randint(0, 13), reg(1, rnd.randint(0, 15)) | (0xF << 16)]
        else:
            op = rnd.choice(alu + flow if major >= 2 else alu[:-4])
            nsrc = rnd.randint(0, 4)
            params = []
            if op not in ("LOOP", "ENDLOOP", "REP", "ENDREP", "IF", "IFC", "ELSE", "ENDIF", "BREAK", "BREAKC", "CALL",
                          "CALLNZ", "LABEL", "RET", "NOP"):
                d = reg(rnd.choice([0, 6]), rnd.randint(0, 11)) | (0xF << 16)
                rel_dst = major >= 3 and rnd.random() < 0.1
                params.append(d | ((1 << 13) if rel_dst else 0))
                if rel_dst:
                    params.append(reg(15, 0))
                if major >= 2 and rnd.random() < 0.1:
                    params.append(reg(19, 0))
                    op_pred = True
                else:
                    op_pred = False
            else:
                op_pred = False
            for _s in range(nsrc):
                t = rnd.choice(const_types + other_types)
                num = rnd.randint(0, 300 if rnd.random() < 0.1 else 30)
                rel = rnd.random() < 0.08
                params.append(reg(t, num) | (0xE4 << 16) | ((1 << 13) if rel else 0))
                if rel and major >= 2:
                    params.append(reg(3, 0))
            token = O[op] | ((1 << 28) if op_pred else 0)
            if major >= 2:
                token |= min(len(params), 15) << 24
                params = params[:15]
            toks.append(token)
            toks += params
            continue
        token = O[op]
        if major >= 2:
            token |= len(params) << 24
        toks.append(token)
        toks += params
    if rnd.random() < 0.97:
        toks.append(0x0000FFFF)
    return toks


def cmd_twin(args):
    os.makedirs(args.out, exist_ok=True)
    rnd = random.Random(1616)
    cases = [(rnd.random() < 0.3, random_shader(rnd)) for _ in range(args.cases)]
    path = os.path.join(args.out, "shaders.txt")
    with open(path, "w") as f:
        for swvp, toks in cases:
            f.write("%d %s\n" % (1 if swvp else 0, " ".join("%08x" % t for t in toks)))
    proc = subprocess.run(tool_cmd(args) + ["--analyze", path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        print("FAIL: --analyze exited with %d: %s" % (proc.returncode, proc.stderr.decode(errors="replace")[-2000:]))
        return 1
    lines = [l.split() for l in proc.stdout.decode().splitlines() if l.strip()]
    if len(lines) != len(cases):
        print("FAIL: %d results for %d shaders" % (len(lines), len(cases)))
        return 1
    bad = 0
    stats = {"valid": 0, "F": 0, "I": 0, "B": 0, "relative": 0}
    for (swvp, toks), got in zip(cases, lines):
        valid, mf, mi, mb = rl_vs_analysis.analyze(toks, swvp)
        exp = [str(int(valid)), str(mf), str(mi), str(mb)]
        if valid:
            stats["valid"] += 1
            stats["F"] += mf > 0
            stats["I"] += mi > 0
            stats["B"] += mb > 0
            stats["relative"] += mf in (256, 8192)
        if got != exp:
            bad += 1
            if bad <= 10:
                print("FAIL: swvp=%d %s: C++ %s, Python %s" % (swvp, " ".join("%08x" % t for t in toks), got, exp))
    print("%s: %d shaders (%s), %d mismatches" % ("FAIL" if bad else "PASS", len(cases),
                                                    ", ".join("%s %d" % kv for kv in stats.items()), bad))
    return 1 if bad else 0


# ---------------------------------------------------------------------------------------------------
# run (Wine)
# ---------------------------------------------------------------------------------------------------

def load_ref(path):
    spec = importlib.util.spec_from_file_location("remix_hash_ref", path)
    ref = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ref)
    ref.set_impl("auto")
    return ref


def run_app(args, run_dir, env_extra, dump_dir=None):
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    if dump_dir:
        os.makedirs(os.path.join(run_dir, dump_dir))
    for src in (args.exe, args.d3d9, args.d3d8):
        dst = os.path.join(run_dir, os.path.basename(src))
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy2(src, dst)
    env = dict(os.environ)
    env.setdefault("DXVK_LOG_LEVEL", "warn")
    env["DXVK_LOG_PATH"] = "none"
    env["DXVK_SHADER_CACHE"] = "0"
    env.update(env_extra)
    local_exe = os.path.join(run_dir, os.path.basename(args.exe))
    cmd = ["bash", args.runner, "--native-d3d", args.prefix_root, local_exe, "--out", ".", "--quiet"]
    proc = subprocess.run(cmd, cwd=run_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode(errors="replace")
    with open(os.path.join(run_dir, "run.log"), "w") as f:
        f.write(text)
    return proc.returncode, text


def sha_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def f32(hexbits):
    return struct.unpack("<f", struct.pack("<I", int(hexbits, 16)))[0]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def mat_inv(m):
    """Gauss-Jordan inverse in double."""
    a = [list(map(float, row)) + [1.0 if i == j else 0.0 for j in range(4)] for i, row in enumerate(m)]
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        piv = a[c][c]
        a[c] = [x / piv for x in a[c]]
        for r in range(4):
            if r != c:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [row[4:] for row in a]


def row_mul(v, m):
    return [sum(v[k] * m[k][j] for k in range(4)) for j in range(4)]


def as_mat(flat):
    return [list(flat[r * 4:r * 4 + 4]) for r in range(4)]


IDENTITY = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]


def remix_back_transform(clip, world, view, proj):
    """Remix's emitVertexCaptureOp in D3D row-vector form, double precision."""
    view_h = row_mul(clip, mat_inv(as_mat(proj)))
    world_h = row_mul(view_h[:3] + [1.0], mat_inv(as_mat(view)))
    obj_h = row_mul(world_h[:3] + [1.0], mat_inv(as_mat(world)))
    return obj_h[:3]


def unpack_color(c):
    return [((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0, ((c >> 24) & 0xFF) / 255.0]


def pack_color(rgba):
    def u8(x):
        x = min(max(x, 0.0), 1.0)
        return int(x * 255.0 + 0.5)
    r, g, b, a = (u8(x) for x in rgba)
    return (a << 24) | (r << 16) | (g << 8) | b


class Sidecar:
    def __init__(self, path):
        with open(path) as f:
            self.sc = json.load(f)
        self.blobs = {}

    def blob(self, key):
        if key not in self.blobs:
            data = base64.b64decode(self.sc["blobs"][key])
            if hashlib.sha256(data).hexdigest() != key:
                raise ValueError("sidecar blob %s does not hash to its key" % key[:16])
            self.blobs[key] = data
        return self.blobs[key]

    def buffer_bytes(self, buf_id, version):
        b = [x for x in self.sc["buffers"] if x["id"] == buf_id][0]
        return self.blob(b["versions"][version]["blob"])

    def shader(self, shader_id):
        s = [x for x in self.sc["shaders"] if x["id"] == shader_id][0]
        return self.blob(s["blob"])


DECL_FMT = {"FLOAT1": "<f", "FLOAT2": "<2f", "FLOAT3": "<3f", "FLOAT4": "<4f", "D3DCOLOR": "<I"}


def element(draw, usage, index=0):
    for e in draw["vertex_format"]["elements"]:
        if e["usage"] == usage and e["usage_index"] == index:
            return e
    return None


def read_element(sc, draw, e, vertex):
    s = [x for x in draw["streams"] if x["stream"] == e["stream"]][0]
    data = sc.buffer_bytes(s["buffer"], s["version"])
    off = s["offset"] + s["stride"] * vertex + e["offset"]
    return struct.unpack_from(DECL_FMT[e["type"]], data, off)


def referenced_vertices(sc, draw):
    """gl_VertexIndex values (= stream vertex numbers) the draw references."""
    call = draw["call"]
    if call == "DrawIndexedPrimitive":
        ib = draw["index_buffer"]
        data = sc.buffer_bytes(ib["buffer"], ib["version"])
        size = 4 if ib["format"] == "INDEX32" else 2
        count = draw["index_count"]
        idx = struct.unpack_from("<%d%s" % (count, "I" if size == 4 else "H"), data, draw["start_index"] * size)
        return sorted({draw["base_vertex"] + i for i in idx})
    if call == "DrawPrimitive":
        return list(range(draw["start_vertex"], draw["start_vertex"] + draw["vertex_count"]))
    raise ValueError("unsupported draw call %s in the vertex capture check" % call)


def const_f(state, reg):
    for c in state.get("vs_const_f", []):
        if c["register"] == reg:
            return [float(x) for x in c["value"]]
    return [0.0, 0.0, 0.0, 0.0]


def const_i(state, reg):
    for c in state.get("vs_const_i", []):
        if c["register"] == reg:
            return [int(x) for x in c["value"]]
    return [0, 0, 0, 0]


def const_b(state, reg):
    for c in state.get("vs_const_b", []):
        if c["register"] == reg:
            return bool(c["value"])
    return False


def expected_color(app, state, draw, sc, vertex):
    """What the app's vertex shader writes to COLOR0 (Tests/relight/apps/scenes/<app>.cpp)."""
    col = unpack_color(read_element(sc, draw, element(draw, "COLOR"), vertex)[0])
    if app == "vs_sm2":  # mad oD0, v3, max(dp3(v1, c4), c7.x), c5
        n = read_element(sc, draw, element(draw, "NORMAL"), vertex)
        c4, c5 = const_f(state, 4), const_f(state, 5)
        d = max(n[0] * c4[0] + n[1] * c4[1] + n[2] * c4[2], 0.0)
        return [col[k] * d + c5[k] for k in range(4)]
    if app == "vs_sm3":  # v1 + i1.x * c8 + (b0 ? c9 : 0) + i0.x * c10 (c10 = def 0)
        c8, c9 = const_f(state, 8), const_f(state, 9)
        loops = const_i(state, 1)[0]
        return [col[k] + loops * c8[k] + (c9[k] if const_b(state, 0) else 0.0) for k in range(4)]
    return None


def vs_consts_bytes(state):
    f = bytearray(256 * 16)
    i = bytearray(16 * 16)
    b = bytearray(4)
    for c in state.get("vs_const_f", []):
        struct.pack_into("<4f", f, c["register"] * 16, *c["value"])
    for c in state.get("vs_const_i", []):
        struct.pack_into("<4i", i, c["register"] * 16, *c["value"])
    for c in state.get("vs_const_b", []):
        if c["value"]:
            b[c["register"] // 8] |= 1 << (c["register"] % 8)
    return bytes(f), bytes(i), bytes(b)


def check_capture(args, ref, sc, record_lines):
    errors = []
    notes = {"vs_draws": 0, "vertices": 0, "ff_draws": 0, "worst_pos": 0.0, "worst_clip": 0.0, "worst_twin": 0.0}
    header = json.loads(record_lines[0]) if record_lines else {}
    if header.get("schema") != "fuse.relight.capture/1":
        return ["capture record: missing header"], notes
    draws = [json.loads(l) for l in record_lines if '"ev":"draw"' in l]
    for fr in sc.sc.get("recorded_frames", []):
        sdraws = sorted((d for d in sc.sc["draws"] if d["frame"] == fr), key=lambda d: d["seq"])
        cdraws = sorted((d for d in draws if d["frame"] == fr), key=lambda d: d["di"])
        if len(sdraws) != len(cdraws):
            errors.append("frame %d: %d sidecar draws, %d captured draws" % (fr, len(sdraws), len(cdraws)))
            continue
        for sd, cd in zip(sdraws, cdraws):
            where = "frame %d seq %d" % (fr, sd["seq"])
            state = sc.sc["state_blocks"][sd["state"]]
            geometry = cd.get("geometry") or {}
            fields = geometry.get("f", "").split(",")
            vs_id = state.get("vertex_shader")
            if vs_id is None:
                notes["ff_draws"] += 1
                if "vertex_capture" in cd:
                    errors.append("%s: a fixed-function draw has vertex_capture" % where)
                if len(fields) == 9 and fields[8] != "0" * 16:
                    errors.append("%s: fixed-function draw with a vertexshader component %s" % (where, fields[8]))
                continue
            notes["vs_draws"] += 1
            # 4. the vertexshader component
            fb, ib, bb = vs_consts_bytes(state)
            expected_hash = rl_vs_analysis.vertex_shader_hash(ref, sc.shader(vs_id), fb, ib, bb)
            if expected_hash in (None, 0):
                errors.append("%s: the reference has no vertexshader hash" % where)
            elif len(fields) != 9 or int(fields[8], 16) != expected_hash:
                errors.append("%s: vertexshader component %s, reference %016x" % (where, fields[8:9], expected_hash))
            # 3. the captured vertices
            vc = cd.get("vertex_capture")
            if not vc:
                errors.append("%s: programmable-VS draw without vertex_capture" % where)
                continue
            slots = {s["k"]: s for s in vc["slots"]}
            base = int(vc["base"])
            referenced = referenced_vertices(sc, sd)
            if vc["truncated"]:
                errors.append("%s: truncated vertex capture" % where)
            written = {base + k for k in slots}
            missing = [v for v in referenced if v not in written]
            if missing:
                errors.append("%s: vertices %s were not captured (base %d, count %d)" % (where, missing, base, vc["count"]))
            xf = sd.get("transforms", {})
            world, view, proj = (xf.get(n, IDENTITY) for n in ("WORLD", "VIEW", "PROJECTION"))
            c = [const_f(state, r) for r in range(4)]
            pos_e, tc_e, n_e = element(sd, "POSITION"), element(sd, "TEXCOORD"), element(sd, "NORMAL")
            for v in referenced:
                s = slots.get(v - base)
                if s is None:
                    continue
                notes["vertices"] += 1
                obj = read_element(sc, sd, pos_e, v)
                pos = [f32(x) for x in s["pos"]]
                clip = [f32(x) for x in s["clip"]]
                cpu_clip = [obj[0] * c[j][0] + obj[1] * c[j][1] + obj[2] * c[j][2] + c[j][3] for j in range(4)]
                twin = remix_back_transform(clip, world, view, proj)
                e_pos = max(abs(a - b) for a, b in zip(pos, obj))
                e_clip = max(abs(a - b) for a, b in zip(clip, cpu_clip))
                e_twin = max(abs(a - b) for a, b in zip(pos, twin))
                notes["worst_pos"] = max(notes["worst_pos"], e_pos)
                notes["worst_clip"] = max(notes["worst_clip"], e_clip)
                notes["worst_twin"] = max(notes["worst_twin"], e_twin)
                vw = "%s vertex %d" % (where, v)
                if e_pos > TOL:
                    errors.append("%s: object-space position %s, vertex buffer %s" % (vw, pos, list(obj)))
                if e_clip > TOL:
                    errors.append("%s: clip %s, CPU transform %s" % (vw, clip, cpu_clip))
                if e_twin > TOL:
                    errors.append("%s: back-transform %s, double-precision twin %s" % (vw, pos, twin))
                flags = s["f"]
                if tc_e is not None:
                    tex = [f32(x) for x in s["tex"]]
                    want = read_element(sc, sd, tc_e, v)
                    if not flags & 2 or max(abs(a - b) for a, b in zip(tex, want)) > 1e-6:
                        errors.append("%s: texcoord %s (fields %#x), shader output %s" % (vw, tex, flags, list(want)))
                if n_e is not None:  # vs_sm2 reads NORMAL0 (an input): Remix captures it, times objectToWorld
                    nrm = [f32(x) for x in s["nrm"]]
                    n = read_element(sc, sd, n_e, v)
                    w = world
                    want = [w[r * 4 + 0] * n[0] + w[r * 4 + 1] * n[1] + w[r * 4 + 2] * n[2] for r in range(3)]
                    if not flags & 8 or max(abs(a - b) for a, b in zip(nrm, want)) > 1e-6:
                        errors.append("%s: normal %s (fields %#x), expected %s" % (vw, nrm, flags, want))
                elif flags & 0xC:
                    errors.append("%s: a normal was captured from a shader without one (fields %#x)" % (vw, flags))
                want_c = expected_color(args.app, state, sd, sc, v)
                if want_c is not None:
                    col = int(s["col"], 16)
                    exp = pack_color(want_c)
                    close = all(abs(((col >> sh) & 0xFF) - ((exp >> sh) & 0xFF)) <= 1 for sh in (0, 8, 16, 24))
                    if not flags & 16 or not close:
                        errors.append("%s: colour %08X (fields %#x), shader output %08X" % (vw, col, flags, exp))
    if notes["vs_draws"] == 0:
        errors.append("the recorded frames have no programmable-VS draw")
    return errors, notes


def cmd_run(args):
    if not args.spirv_val or not os.path.isfile(args.spirv_val):
        print("SKIP: spirv-val not found")
        return SKIP
    ref = load_ref(args.ref)
    ref_dir = os.path.join(args.out, "reference")
    rc, text = run_app(args, ref_dir, {"FUSE_RELIGHT": "0"})
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(text.strip()[-4000:])
        print("FAIL: %s: reference run exited with %d" % (args.app, rc))
        return 1
    cap_dir = os.path.join(args.out, "capture")
    rc, text = run_app(args, cap_dir, {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "capture",
                                       "FUSE_RELIGHT_TAP_CAPTURE_PATH": "relight_capture.jsonl",
                                       "FUSE_RELIGHT_VC_DUMP": "vc_dump"}, dump_dir="vc_dump")
    if rc != 0:
        print(text.strip()[-4000:])
        print("FAIL: %s: capture run exited with %d" % (args.app, rc))
        return 1
    errors = []
    # 1. rendering unchanged
    ref_dumps = sorted(os.path.basename(p) for p in glob.glob(os.path.join(ref_dir, "*.rgba")))
    if not ref_dumps:
        errors.append("the reference run wrote no .rgba dump")
    for name in ref_dumps:
        other = os.path.join(cap_dir, name)
        if not os.path.isfile(other) or sha_file(other) != sha_file(os.path.join(ref_dir, name)):
            errors.append("%s differs from the FUSE_RELIGHT=0 reference" % name)
    # 2. spirv-val on what DXVK compiled
    dumped = sorted(glob.glob(os.path.join(cap_dir, "vc_dump", "*.spv")))
    outs = [p for p in dumped if p.endswith(".out.spv")]
    if not outs:
        errors.append("no vertex shader went through the SPIR-V pass (vc_dump is empty)")
    for path in dumped:
        ok, env, msg = validate(args.spirv_val, path)
        if not ok:
            errors.append("spirv-val (%s) %s: %s" % (env, os.path.basename(path), msg[-1500:]))
    # 3. / 4. the capture record
    record = os.path.join(cap_dir, "relight_capture.jsonl")
    sidecar = os.path.join(cap_dir, args.app + ".json")
    if not os.path.isfile(record) or not os.path.isfile(sidecar):
        errors.append("missing %s or %s" % (record, sidecar))
        notes = {}
    else:
        with open(record) as f:
            lines = [l for l in f if l.strip()]
        errs, notes = check_capture(args, ref, Sidecar(sidecar), lines)
        errors += errs
    for e in errors[:60]:
        print("FAIL: %s: %s" % (args.app, e))
    print("%s: %s: %d SPIR-V modules validated (%d transformed), %s" % (
        "FAIL" if errors else "PASS", args.app, len(dumped), len(outs),
        ", ".join("%s %s" % (k, ("%.3g" % v) if isinstance(v, float) else v) for k, v in notes.items())))
    if not errors:
        shutil.rmtree(ref_dir, ignore_errors=True)
    return 1 if errors else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", required=True, help="fuse_relight_vertex_capture_tests")
    ap.add_argument("--emulator", action="append", help="emulator command word (repeat)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("spirv-val")
    s.add_argument("--spirv-val", default="")
    s.add_argument("--out", required=True)
    s.add_argument("inputs", nargs="*")
    t = sub.add_parser("twin")
    t.add_argument("--out", required=True)
    t.add_argument("--cases", type=int, default=2000)
    r = sub.add_parser("run")
    for name in ("--exe", "--app", "--d3d9", "--d3d8", "--runner", "--prefix-root", "--ref", "--out"):
        r.add_argument(name, required=True)
    r.add_argument("--spirv-val", default="")
    args = ap.parse_args()
    if args.cmd == "spirv-val":
        return cmd_spirv_val(args)
    if args.cmd == "twin":
        return cmd_twin(args)
    return cmd_run(args)


if __name__ == "__main__":
    sys.exit(main())
