#!/usr/bin/env python3
"""FUSE Relight RL-1.5: capture expectations for fixed-function translation, lights, fog and cameras.

  capture   runs an RL-0.4 test app under Wine through the Relight d3d9.dll / d3d8.dll with the recording
            tap (relight.tap.mode = record, the RL-1.1 driver rl_tap_run.py), replays the recorded event
            stream through TranslateTap (rl_translate_replay: RL-1.2's classifier + this package), and
            checks every draw and frame of the app's recorded frames against
              1. an independent reference computed here from the app's sidecar alone (its own record of
                 what it fed D3D9): setLegacyMaterialState, the texture stage, setFogState, the D3DLIGHT9
                 -> light conversion with Remix's stable light hash (float operations emulated step by
                 step, hashes with Tools/FUSE/Relight/remix_hash_ref.py), fog discovery, the FUSE additions
                 (viewport rectangle, emissive colour source), and the camera
                 (decomposed here in double precision: within 1e-5; and the app's own camera annotation);
              2. the hand-written semantics in expectations/<scene>.json.
  selftest  the expectation files parse, and the reference and matchers catch seeded mismatches.
  roundtrip the recording tap -> rl_translate_replay path on a synthetic session (clip planes, the light /
            clip-plane change counters, the alpha-swizzle mask): `fuse_relight_translate_tests record` writes
            the recorded stream and TranslateTap's in-process output; the replay of the stream must equal
            that output line for line, and must use a clip plane.

  rl_translate_capture.py capture --exe app.exe --app ff_lit --d3d9 d3d9.dll --d3d8 d3d8.dll --runner run.sh
      --prefix-root DIR --replay rl_translate_replay.exe [--emulator 'wine-run.sh|prefix'] --expect DIR
      --hash-ref DIR --out DIR
  rl_translate_capture.py capture --from-run DIR ...   (reuse a recorded run: relight_tap.jsonl + <app>.json)
  rl_translate_capture.py roundtrip --gen fuse_relight_translate_tests --replay rl_translate_replay
      [--emulator 'wine-run.sh|prefix'] --out DIR

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb).
"""
import argparse
import base64
import json
import math
import os
import shutil
import struct
import subprocess
import sys

# The helpers imported from tests/tap and tests/classify (other packages' directories) must not leave
# __pycache__ behind in them.
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP = 77
REF = None  # Tools/FUSE/Relight/remix_hash_ref.py


def load_hash_ref(path):
    global REF
    sys.path.insert(0, path)
    import remix_hash_ref  # noqa: E402
    REF = remix_hash_ref
    return remix_hash_ref


# ---------------------------------------------------------------------------------------------------
# float32 emulation: every IEEE operation computed in double and rounded once is the correctly rounded
# float result (double has more than 2 * 24 + 2 bits), so F(a op b) reproduces C++ float arithmetic.
# ---------------------------------------------------------------------------------------------------

def F(x):
    if isinstance(x, float) and (math.isinf(x) or math.isnan(x)):
        return x
    return struct.unpack("<f", struct.pack("<f", x))[0]


def fbits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def bitsf(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


def fdiv(a, b):
    if b == 0.0:
        if a == 0.0 or math.isnan(a):
            return float("nan")
        return math.copysign(float("inf"), a) * math.copysign(1.0, b)
    return F(a / b)


def fsqrt(x):
    return F(math.sqrt(x))


# ---------------------------------------------------------------------------------------------------
# D3D9 state as the sidecar records it (sparse: the states the app set) with D3D9's defaults
# ---------------------------------------------------------------------------------------------------

DEFAULT_RS = {
    "ZENABLE": 1, "ZWRITEENABLE": 1, "ALPHATESTENABLE": 0, "SRCBLEND": 2, "DESTBLEND": 1, "ALPHAREF": 0, "ALPHAFUNC": 8,
    "ALPHABLENDENABLE": 0, "FOGENABLE": 0, "FOGCOLOR": 0, "FOGTABLEMODE": 0, "FOGSTART": fbits(0.0),
    "FOGEND": fbits(1.0), "FOGDENSITY": fbits(1.0), "STENCILENABLE": 0, "TEXTUREFACTOR": 0xFFFFFFFF, "LIGHTING": 1,
    "FOGVERTEXMODE": 0, "COLORVERTEX": 1, "DIFFUSEMATERIALSOURCE": 1, "SPECULARMATERIALSOURCE": 2,
    "EMISSIVEMATERIALSOURCE": 0,
    "CLIPPLANEENABLE": 0, "COLORWRITEENABLE": 0xF, "BLENDOP": 1, "SEPARATEALPHABLENDENABLE": 0, "SRCBLENDALPHA": 2,
    "DESTBLENDALPHA": 1, "BLENDOPALPHA": 1, "ADAPTIVETESS_Y": 0, "POINTSIZE": fbits(1.0),
}


def tss_default(stage, name):
    return {"COLOROP": 4 if stage == 0 else 1, "COLORARG1": 2, "COLORARG2": 1, "ALPHAOP": 2 if stage == 0 else 1,
            "ALPHAARG1": 2, "ALPHAARG2": 1, "TEXCOORDINDEX": stage, "TEXTURETRANSFORMFLAGS": 0, "COLORARG0": 1,
            "ALPHAARG0": 1, "RESULTARG": 1}[name]


class State:
    def __init__(self, block, textures):
        self.rs = dict(DEFAULT_RS)
        self.rs.update(block.get("render_states", {}))
        self.tss = [{} for _ in range(8)]
        for st in block.get("texture_stages", []):
            self.tss[st["stage"]].update(st["states"])
        self.textures = {t["stage"]: t["texture"] for t in block.get("textures", [])}
        self.tex_info = textures  # sidecar texture id -> (type, hash)
        self.lights = block.get("lights", [])
        self.material = block.get("material") or {"diffuse": [0] * 4, "ambient": [0] * 4, "specular": [0] * 4,
                                                  "emissive": [0] * 4, "power": 0}
        self.viewport = block.get("viewport", {})
        self.vs = block.get("vertex_shader")
        self.ps = block.get("pixel_shader")

    def t(self, stage, name):
        return self.tss[stage].get(name, tss_default(stage, name))


# ---------------------------------------------------------------------------------------------------
# reference: material (d3d9_rtx_utils.cpp setLegacyMaterialState / setTextureStageState)
# ---------------------------------------------------------------------------------------------------

VK_CMP = {1: 0, 2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 6, 8: 7}                     # D3DCMPFUNC -> VkCompareOp
VK_BLEND = {1: 0, 2: 1, 3: 2, 4: 3, 5: 6, 6: 7, 7: 8, 8: 9, 9: 4, 10: 5, 11: 14, 12: 6, 13: 7, 16: 15, 17: 16}
VK_BLENDOP = {1: 0, 2: 1, 3: 2, 4: 3, 5: 4}
TEXOP = {1: "Disable", 2: "SelectArg1", 3: "SelectArg2", 4: "Modulate", 5: "Modulate2x", 6: "Modulate4x", 7: "Add"}


def blend_factor(b, alpha):
    if b == 14:
        return 12 if alpha else 10
    if b == 15:
        return 13 if alpha else 11
    return VK_BLEND.get(b, 0)


def color_source(src):
    return "VertexColor0" if src == 1 else "None"


def texture_arg(arg, c0, c1):
    return {0: c0, 1: c0, 4: c1, 2: "Texture", 3: "TFactor"}.get(arg, "None")


def ref_material(s, elements, alpha_swizzle, d3d8, multisampled):
    has_pt = any(e["usage"] == "POSITIONT" for e in elements)
    has_c0 = any(e["usage"] == "COLOR" and e["usage_index"] == 0 for e in elements)
    has_c1 = any(e["usage"] == "COLOR" and e["usage_index"] == 1 for e in elements)
    lighting = s.rs["LIGHTING"] != 0 and not has_pt
    diffuse = 1 if has_c0 else 0
    specular = 2 if has_c1 else 0
    if lighting:
        mask = (diffuse | specular) if s.rs["COLORVERTEX"] else 0
        diffuse = s.rs["DIFFUSEMATERIALSOURCE"] & mask
        specular = s.rs["SPECULARMATERIALSOURCE"] & mask
    # FUSE: the emissive colour source (D3D9 FFP semantics: lighting and COLORVERTEX on, the vertex has the colour).
    emissive = "Material"
    if lighting and s.rs["COLORVERTEX"]:
        src = s.rs["EMISSIVEMATERIALSOURCE"]
        emissive = "VertexColor0" if src == 1 and has_c0 else ("VertexColor1" if src == 2 and has_c1 else "Material")
    alpha_test = s.rs["ALPHATESTENABLE"] != 0
    if alpha_test and not d3d8 and multisampled and s.rs["ADAPTIVETESS_Y"] == REF.fourcc("ATOC"):
        alpha_test = False  # alpha to coverage
    m = {"alpha_test": alpha_test, "alpha_test_op": VK_CMP.get(s.rs["ALPHAFUNC"], 0) if alpha_test else 7,
         "alpha_ref": s.rs["ALPHAREF"] & 0xFF, "blend": s.rs["ALPHABLENDENABLE"] != 0,
         "diffuse_source": color_source(diffuse), "specular_source": color_source(specular),
         "tfactor": s.rs["TEXTUREFACTOR"], "write_mask": s.rs["COLORWRITEENABLE"]}

    def fixup(src, dst, op):
        if src == 12:
            return 5, 6, op
        if src == 13:
            return 6, 5, op
        return src, dst, op
    color = fixup(s.rs["SRCBLEND"], s.rs["DESTBLEND"], s.rs["BLENDOP"])
    alpha = color
    if s.rs["SEPARATEALPHABLENDENABLE"]:
        alpha = fixup(s.rs["SRCBLENDALPHA"], s.rs["DESTBLENDALPHA"], s.rs["BLENDOPALPHA"])

    def norm(f):
        if alpha_swizzle and f == 8:
            return 1
        if alpha_swizzle and f == 9:
            return 0
        return f
    m["color_src"] = norm(blend_factor(color[0], False))
    m["color_dst"] = norm(blend_factor(color[1], False))
    m["color_op"] = VK_BLENDOP.get(color[2], 0)
    m["alpha_src"] = norm(blend_factor(alpha[0], True))
    m["alpha_dst"] = norm(blend_factor(alpha[1], True))
    m["alpha_op"] = VK_BLENDOP.get(alpha[2], 0)
    m["vc_baked"] = True
    m["emissive_source"] = emissive
    m["d3d_material"] = {k: s.material[k] for k in ("diffuse", "ambient", "specular", "emissive", "power")}
    return m


def args_mask(op):
    return {1: 0b000, 2: 0b010, 17: 0b010, 3: 0b100, 25: 0b111, 26: 0b111}.get(op, 0b110)


def select_textures(s):
    """processTextures<FixedFunction>: (slots, first stage)."""
    bins = [None] * 16
    if not s.ps:
        for stage in range(8):
            if stage not in s.textures:
                continue
            if s.t(stage, "COLOROP") == 1:
                break
            used = args_mask(s.t(stage, "COLOROP")) | args_mask(s.t(stage, "ALPHAOP"))
            tex = 0
            for bit, (ca, aa) in enumerate((("COLORARG0", "ALPHAARG0"), ("COLORARG1", "ALPHAARG1"),
                                            ("COLORARG2", "ALPHAARG2"))):
                if (s.t(stage, ca) & 0xF) == 2 or (s.t(stage, aa) & 0xF) == 2:
                    tex |= 1 << bit
            if not used & tex:
                continue
            if s.tex_info.get(s.textures[stage], (0, 0))[0] != 3:
                continue
            c = (s.t(stage, "TEXCOORDINDEX") & 7) * 2
            sub = 0 if bins[c] is None else 1
            if bins[c + sub] is None:
                bins[c + sub] = stage
        order = bins
    else:
        order = [0, 1]
    slots, first = [], 0
    for stage in order:
        if len(slots) == 2:
            break
        if stage is None or stage not in s.textures:
            continue
        if not slots and not s.ps:
            first = stage
        slots.append(stage)
    return slots + [-1] * (2 - len(slots)), first


def tf_enabled(s, st):
    cop, aop = s.t(st, "COLOROP"), s.t(st, "ALPHAOP")
    if cop == 1 and aop == 1:
        return False
    prev = 1
    if st:
        prev = 5 if (s.t(st - 1, "RESULTARG") & 0xF) == 5 else 1
    a1c, a2c = s.t(st, "COLORARG1") & 0xF, s.t(st, "COLORARG2") & 0xF
    a1a, a2a = s.t(st, "ALPHAARG1") & 0xF, s.t(st, "ALPHAARG2") & 0xF
    mod = (4, 5, 6)
    return (cop in mod and ((a1c == 3 and a2c == prev) or (a2c == 3 and a1c == prev))) or \
           (aop in mod and ((a1a == 3 and a2a == prev) or (a2a == 3 and a1a == prev)))


def ref_texture_stage(s, m):
    """Texture binding, texture-factor blending and setTextureStageState of the first stage."""
    slots, first = select_textures(s)
    m["texture_slots"] = slots
    m["hash"] = "0x%016x" % (s.tex_info.get(s.textures.get(slots[0]), (0, 0))[1] if slots[0] >= 0 else 0)
    use_stage, use_multi = True, False
    if not s.ps:
        for stage in range(8):
            if use_stage and stage != 0 and tf_enabled(s, stage):
                use_multi = True
            if stage not in s.textures:
                continue
            if s.t(stage, "COLOROP") == 1:
                break
    c0, c1 = m["diffuse_source"], m["specular_source"]

    def arg(name):
        a = texture_arg(s.t(first, name), c0, c1)
        return "None" if (a == "TFactor" and not use_stage) else a
    m["tex_color_op"] = TEXOP.get(s.t(first, "COLOROP"), "Modulate")
    m["tex_color_arg1"] = arg("COLORARG1")
    m["tex_color_arg2"] = arg("COLORARG2")
    m["tex_alpha_op"] = TEXOP.get(s.t(first, "ALPHAOP"), "Modulate")
    m["tex_alpha_arg1"] = arg("ALPHAARG1")
    m["tex_alpha_arg2"] = arg("ALPHAARG2")
    m["tf_blend"] = use_multi
    tci = s.t(first, "TEXCOORDINDEX")
    texgen = {0x20000: "ViewPositions", 0x10000: "ViewNormals"}.get(tci, "None")
    return texgen


# ---------------------------------------------------------------------------------------------------
# reference: fog (setFogState, SceneManager fog discovery)
# ---------------------------------------------------------------------------------------------------

def ref_fog(s):
    if not s.rs["FOGENABLE"]:
        return {"mode": 0}
    c = s.rs["FOGCOLOR"]
    color = [F(((c >> 16) & 0xFF) / 255.0), F(((c >> 8) & 0xFF) / 255.0), F((c & 0xFF) / 255.0)]
    end, start = bitsf(s.rs["FOGEND"]), bitsf(s.rs["FOGSTART"])
    mode = s.rs["FOGTABLEMODE"] if s.rs["FOGTABLEMODE"] != 0 else s.rs["FOGVERTEXMODE"]
    fog = {"mode": mode, "color": color, "scale": fdiv(1.0, F(end - start)), "end": end,
           "density": bitsf(s.rs["FOGDENSITY"])}
    data = struct.pack("<I3f3f", mode, *color, fog["scale"], fog["end"], fog["density"])
    fog["hash"] = "0x%016x" % (REF.xxh3_64(data) if mode else 0)
    return fog


# ---------------------------------------------------------------------------------------------------
# reference: lights (rtx_lights_data.cpp / rtx_light_utils.cpp, float operations emulated)
# ---------------------------------------------------------------------------------------------------

K_LEGACY_END = F(1.0 / 255.0)
K_NEW_END = F(0.01)
K_PI = F(math.pi)
LIGHT_TYPES = {"POINT": 1, "SPOT": 2, "DIRECTIONAL": 3}


def least_square_intensity(intensity, a2, a1, a0, rng):
    eps = F(0.000001)
    low_range = 0.0
    low_threshold = F(0.1)
    if a2 < eps:
        if a1 > eps:
            low_range = fdiv(F(fdiv(1.0, low_threshold) - a0), a1)
    else:
        a, b, c = a2, a1, F(a0 - fdiv(1.0, low_threshold))
        disc = F(F(b * b) - 4.0 * a * c)
        if disc >= 0:
            sq = fsqrt(disc)
            r1 = fdiv(F(-b + sq), F(2 * a))
            r2 = fdiv(F(-b - sq), F(2 * a))
            if r1 > 0:
                low_range = r1
            if r2 > 0:
                low_range = r2
    if low_range > 0:
        rng = min(rng, low_range)
    num = den = 0.0
    for i in range(5):
        xi = F(F((i + 1) / 5.0) * rng)
        xi2 = F(xi * xi)
        xi4 = F(xi2 * xi2)
        ii = fdiv(intensity, F(F(F(a2 * xi2) + F(a1 * xi)) + a0))
        num = F(num + fdiv(ii, xi2))
        den = F(den + fdiv(1.0, xi4))
    return fdiv(num, den)


def calc_intensity(l, radius):
    a2, a1, a0 = l["attenuation"][2], l["attenuation"][1], l["attenuation"][0]
    ob = max(l["diffuse"][0], max(l["diffuse"][1], l["diffuse"][2]))
    end = l["range"]
    eps = F(0.000001)
    if a0 > 0 and fdiv(ob, a0) < K_LEGACY_END:
        end = 0.0
    elif a2 < eps:
        if a1 > eps:
            end = fsqrt(fdiv(least_square_intensity(ob, a2, a1, a0, l["range"]), K_LEGACY_END))
    else:
        end = fsqrt(fdiv(least_square_intensity(ob, a2, a1, a0, l["range"]), K_LEGACY_END))
    end_sq = F(end * end)
    k = fdiv(K_NEW_END, F(F(K_PI * radius) * radius))
    return F(F(k * end_sq) * 1.0)


def normalize3(v):
    ln = fsqrt(F(F(F(v[0] * v[0]) + F(v[1] * v[1])) + F(v[2] * v[2])))
    if ln == 0.0:
        return [0.0, 0.0, 1.0]
    inv = fdiv(1.0, ln)
    return [F(v[0] * inv), F(v[1] * inv), F(v[2] * inv)]


def f3(v):
    return struct.pack("<3f", *v)


def ref_light(l):
    t = LIGHT_TYPES.get(l["type"], 0)
    if not t:
        return None
    out = {"index": l["index"], "d3d_type": t}
    if t == 3:
        h = REF.xxh64(f3(l["direction"]), 1)                 # seeded with RtLightType::Rect
        h = REF.xxh64(struct.pack("<f", F(F(0.0349) / 2.0)), h)
        out.update(type="distant", hash="0x%016x" % h, direction=normalize3(l["direction"]),
                   half_angle=F(F(0.0349) / 2.0), intensity=1.0,
                   radiance=[F(c * 1.0) for c in l["diffuse"][:3]])
        return out
    shaping = 0
    cos_cone = F(math.cos(F(F(l["phi"]) / 2.0)))
    softness = F(F(math.cos(F(l["theta"] / 2.0))) - cos_cone)
    if t == 2:
        shaping = REF.xxh64(f3(l["direction"]), 0)
        for v in (cos_cone, softness, l["falloff"]):
            shaping = REF.xxh64(struct.pack("<f", v), shaping)
    h = REF.xxh64(f3(l["position"]), 0)
    h = REF.xxh64(struct.pack("<f", 4.0), h)
    h = REF.xxh64(struct.pack("<Q", h), shaping)
    radius = 4.0
    intensity = calc_intensity(l, radius)
    ob = max(l["diffuse"][0], max(l["diffuse"][1], l["diffuse"][2]))
    radiance = [F(fdiv(c, ob) * intensity) for c in l["diffuse"][:3]]
    out.update(type="sphere", hash="0x%016x" % h, position=list(l["position"]), radius=radius, intensity=intensity,
               radiance=radiance)
    if t == 2:
        out["shaping"] = {"enabled": True, "direction": normalize3(l["direction"]), "cos_cone": cos_cone,
                          "softness": softness, "focus": l["falloff"]}
    else:
        out["shaping"] = {"enabled": False}
    return out


def light_off(r):
    rad = r["radiance"]
    return any(c < 0 for c in rad) or all(c <= 0 for c in rad)


class LightRef:
    """processRenderState's light step: re-sent when the enabled lights changed or first in the frame."""

    def __init__(self):
        self.last = None
        self.frame = []

    def draw(self, lights):
        enabled = [l for l in lights if l["enabled"]]
        if self.last is not None and enabled == self.last:
            return []
        self.last = enabled
        added = []
        for l in enabled:
            r = ref_light(l)
            if r is None or light_off(r):
                continue
            if any(x["hash"] == r["hash"] for x in self.frame):
                continue
            self.frame.append(r)
            added.append(r)
        return added


# ---------------------------------------------------------------------------------------------------
# reference: camera decomposition in double precision
# ---------------------------------------------------------------------------------------------------

def ref_camera(proj, view):
    m = proj
    lh = m[10] > 0
    sign = 1.0 if m[11] > 0 else -1.0
    near = -m[14] / m[10] * sign
    far = m[14] / (m[11] - m[10]) * sign
    # Frustum half-extents at unit depth (no shear in the test apps: m[8] = m[9] = 0).
    x0, x1 = (1 + m[8] * sign) / m[0], -(1 - m[8] * sign) / m[0]
    y0, y1 = (1 + m[9] * sign) / m[5], -(1 - m[9] * sign) / m[5]
    fov = abs(math.atan(y1) - math.atan(y0))
    aspect = abs((x1 - x0) / (y1 - y0))
    # Camera position: the translation of inverse(VIEW) (rigid view: -t * R^T).
    t = view[12:15]
    pos = [-(t[0] * view[c * 4 + 0] + t[1] * view[c * 4 + 1] + t[2] * view[c * 4 + 2]) for c in range(3)]
    return {"fov": fov, "aspect": aspect, "near": near, "far": far, "lhs": lh, "position": pos}


# ---------------------------------------------------------------------------------------------------
# comparison helpers
# ---------------------------------------------------------------------------------------------------

def num(v):
    if isinstance(v, str) and v in ("inf", "-inf", "nan"):
        return float(v)
    return v


def close(a, b, tol):
    a, b = num(a), num(b)
    if isinstance(a, float) and math.isnan(a):
        return isinstance(b, float) and math.isnan(b)
    if isinstance(a, float) and math.isinf(a) or isinstance(b, float) and math.isinf(b):
        return a == b
    return abs(a - b) <= tol * max(1.0, abs(b))


def diff(expect, got, path, errors, tol=0.0):
    """Every key of `expect` must match `got` (floats within tol relative)."""
    if isinstance(expect, dict):
        if not isinstance(got, dict):
            errors.append(f"{path}: expected an object, got {got!r}")
            return
        for k, v in expect.items():
            if k not in got:
                errors.append(f"{path}.{k}: missing")
                continue
            diff(v, got[k], f"{path}.{k}", errors, tol)
    elif isinstance(expect, list):
        if not isinstance(got, list) or len(got) != len(expect):
            errors.append(f"{path}: expected {expect!r}, got {got!r}")
            return
        for i, (e, g) in enumerate(zip(expect, got)):
            diff(e, g, f"{path}[{i}]", errors, tol)
    elif isinstance(expect, bool) or isinstance(expect, str) and expect not in ("inf", "-inf", "nan"):
        if expect != got:
            errors.append(f"{path}: expected {expect!r}, got {got!r}")
    elif isinstance(expect, (int, float, str)):
        if isinstance(got, bool) or not isinstance(num(got), (int, float)) or not close(got, expect, tol):
            errors.append(f"{path}: expected {expect!r}, got {got!r}")
    elif expect != got:
        errors.append(f"{path}: expected {expect!r}, got {got!r}")


def covers(entry, index):
    i = entry["index"]
    return index == i if isinstance(i, int) else i[0] <= index <= i[1]


# ---------------------------------------------------------------------------------------------------
# the checks
# ---------------------------------------------------------------------------------------------------

def sidecar_textures(sidecar):
    """{sidecar texture id: (D3DRESOURCETYPE, Remix hash of the first level-0 upload)}."""
    blobs = sidecar.get("blobs", {})
    out = {}
    kinds = {"texture": 3, "cube": 5, "volume": 4, "surface": 1}
    for t in sidecar.get("textures", []):
        h = 0
        for u in t.get("uploads", []):
            if u.get("level") == 0 and u.get("blob") in blobs:
                h = REF.xxh3_64(base64.b64decode(blobs[u["blob"]]))
                break
        out[t["id"]] = (kinds.get(t.get("kind"), 0), h)
    return out


def check_reference(sidecar, translated, errors):
    """Replay output vs the reference computed from the sidecar. Returns the number of draws checked."""
    texinfo = sidecar_textures(sidecar)
    d3d8 = sidecar.get("api") == "d3d8"
    alpha_swizzle = sidecar.get("device", {}).get("backbuffer_format") in ("X8R8G8B8", "X1R5G5B5")
    draws = {}
    frames = {}
    for t in translated:
        if t["ev"] == "draw":
            draws.setdefault(t["frame"], []).append(t)
        elif t["ev"] == "frame":
            frames[t["frame"]] = t
    checked = 0
    for frame in sidecar["recorded_frames"]:
        side = [d for d in sidecar["draws"] if d["frame"] == frame]
        got = draws.get(frame, [])
        if len(side) != len(got):
            errors.append(f"frame {frame}: sidecar has {len(side)} draw(s), the replay has {len(got)}")
            continue
        lights = LightRef()
        fog_states, frame_fog = [], {"mode": 0}
        view_proj = None
        for i, (d, g) in enumerate(zip(side, got)):
            where = f"frame {frame} draw {i}"
            s = State(sidecar["state_blocks"][d["state"]], texinfo)
            if not g.get("translated"):
                errors.append(f"{where}: not translated ({g.get('status')} / {g.get('reason')})")
                continue
            elements = d["vertex_format"]["elements"]
            m = ref_material(s, elements, alpha_swizzle, d3d8, False)
            texgen = ref_texture_stage(s, m)
            diff(m, g["material"], where + " material", errors, 1e-7)
            vp = s.viewport
            want_vp = [vp.get("x", 0), vp.get("y", 0), vp.get("width", 0), vp.get("height", 0)]
            if g.get("viewport") != want_vp:
                errors.append(f"{where}: viewport {g.get('viewport')!r}, expected the sidecar's {want_vp!r}")
            if g.get("texgen") != texgen:
                errors.append(f"{where}: texgen {g.get('texgen')!r}, expected {texgen!r}")
            fog = ref_fog(s)
            diff(fog, g["fog"], where + " fog", errors, 1e-7)
            added = lights.draw(s.lights)
            diff(added, g["lights"], where + " lights", errors, 1e-6)
            if g.get("camera") != "Main":
                errors.append(f"{where}: camera {g.get('camera')!r}, expected 'Main'")
            if fog["mode"] and fog["hash"] not in [f["hash"] for f in fog_states]:
                fog_states.append(fog)
                if frame_fog["mode"] == 0:
                    frame_fog = fog
            if view_proj is None:
                xf = d.get("transforms", {})
                ident = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
                view_proj = (xf.get("PROJECTION", ident), xf.get("VIEW", ident))
            checked += 1
        fr = frames.get(frame)
        if fr is None:
            errors.append(f"frame {frame}: no frame line from the replay")
            continue
        diff(lights.frame, fr["lights"], f"frame {frame} lights", errors, 1e-6)
        diff(frame_fog, fr["fog"], f"frame {frame} fog", errors, 1e-7)
        diff(fog_states, fr["fog_states"], f"frame {frame} fog_states", errors, 1e-7)
        mains = [c for c in fr["cameras"] if c["type"] == "Main"]
        if len(mains) != 1 or view_proj is None:
            errors.append(f"frame {frame}: expected one Main camera, got {[c['type'] for c in fr['cameras']]}")
            continue
        cam = ref_camera(*view_proj)
        diff({k: cam[k] for k in ("fov", "aspect", "near", "far", "lhs", "position")}, mains[0],
             f"frame {frame} camera (1e-5 KAT)", errors, 1e-5)
        ann = sidecar.get("annotations", {}).get("camera")
        if ann:
            # The app's own camera. The float PROJECTION encodes z_far with ~1e-5 relative error when
            # z_far >> z_near (1 - m22 cancels), so z_far is compared at 1e-4.
            diff({"fov": ann["fov_y"], "aspect": ann["aspect"], "near": ann["z_near"], "position": ann["eye"]},
                 mains[0], f"frame {frame} camera vs app annotation", errors, 1e-5)
            diff({"far": ann["z_far"]}, mains[0], f"frame {frame} camera vs app annotation", errors, 1e-4)
    return checked


def check_expectations(expect, sidecar, translated, errors):
    api = sidecar.get("api", "d3d9")
    entries = [e for e in expect["draws"] if e.get("api", api) == api]
    draws = {}
    frames = {}
    for t in translated:
        if t["ev"] == "draw":
            draws.setdefault(t["frame"], []).append(t)
        elif t["ev"] == "frame":
            frames[t["frame"]] = t
    checked = 0
    for frame in sidecar["recorded_frames"]:
        got = draws.get(frame, [])
        if "draw_count" in expect and len(got) != expect["draw_count"]:
            errors.append(f"frame {frame}: {len(got)} draw(s), expected {expect['draw_count']}")
        for i, g in enumerate(got):
            for e in entries:
                if covers(e, i):
                    want = {k: v for k, v in e.items() if k not in ("index", "api", "why")}
                    diff(want, g, f"frame {frame} draw {i} ({e.get('why', '')})", errors, 1e-6)
                    checked += 1
        fr = frames.get(frame)
        fe = expect.get("frame", {})
        if fr is None:
            errors.append(f"frame {frame}: no frame line")
            continue
        if "light_count" in fe and len(fr["lights"]) != fe["light_count"]:
            errors.append(f"frame {frame}: {len(fr['lights'])} light(s), expected {fe['light_count']}")
        if "light_types" in fe:
            types = {}
            for l in fr["lights"]:
                types[l["type"]] = types.get(l["type"], 0) + 1
            if types != fe["light_types"]:
                errors.append(f"frame {frame}: light types {types}, expected {fe['light_types']}")
        if "shaped_lights" in fe:
            n = sum(1 for l in fr["lights"] if l.get("shaping", {}).get("enabled"))
            if n != fe["shaped_lights"]:
                errors.append(f"frame {frame}: {n} shaped light(s), expected {fe['shaped_lights']}")
        if "light_indices" in fe and sorted(l["index"] for l in fr["lights"]) != fe["light_indices"]:
            errors.append(f"frame {frame}: light indices {sorted(l['index'] for l in fr['lights'])}, "
                          f"expected {fe['light_indices']}")
        if "fog" in fe:
            diff(fe["fog"], fr["fog"], f"frame {frame} fog", errors, 1e-6)
        if "fog_states" in fe and len(fr["fog_states"]) != fe["fog_states"]:
            errors.append(f"frame {frame}: {len(fr['fog_states'])} fog state(s), expected {fe['fog_states']}")
        if "cameras" in fe and [c["type"] for c in fr["cameras"]] != fe["cameras"]:
            errors.append(f"frame {frame}: cameras {[c['type'] for c in fr['cameras']]}, expected {fe['cameras']}")
        if "camera" in fe and fr["cameras"]:
            diff(fe["camera"], fr["cameras"][0], f"frame {frame} camera", errors, 1e-5)
        if "camera_cut" in fe and fr["camera_cut"] != fe["camera_cut"]:
            errors.append(f"frame {frame}: camera_cut {fr['camera_cut']}, expected {fe['camera_cut']}")
    return checked


def read_stream(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def run_replay(args, stream, hashes_path, out_path):
    cmd = []
    if args.emulator:
        cmd += [p for p in args.emulator.split("|") if p]
    cmd += [args.replay, "--stream", stream, "--texture-hashes", hashes_path, "--out", out_path]
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
    load_hash_ref(args.hash_ref)
    # Remix texture hashes of the recorded textures: RL-1.2's helper (tap id -> XXH3 of mip 0 via the sidecar).
    sys.path.insert(0, os.path.join(HERE, "..", "classify"))
    import rl_classify_capture  # noqa: E402
    hashes, _, _ = rl_classify_capture.texture_hashes(read_stream(stream), sidecar, REF)
    os.makedirs(args.out, exist_ok=True)
    hashes_path = os.path.join(args.out, "texture_hashes.txt")
    with open(hashes_path, "w") as f:
        for tex, h in sorted(hashes.items()):
            f.write("%d 0x%016x\n" % (tex, h))
    out_path = os.path.join(args.out, "translated.jsonl")
    rc, text = run_replay(args, stream, hashes_path, out_path)
    if rc != 0 or not os.path.isfile(out_path):
        print(text.strip()[-4000:])
        print(f"FAIL: {args.app}: rl_translate_replay exited with {rc}")
        return 1
    translated = read_stream(out_path)
    errors = []
    n_ref = check_reference(sidecar, translated, errors)
    n_exp = check_expectations(expect, sidecar, translated, errors)
    if errors:
        print(f"FAIL: {args.app}: {len(errors)} mismatch(es):")
        for e in errors[:60]:
            print("  " + e)
        return 1
    frames = [t for t in translated if t["ev"] == "frame" and t["frame"] in sidecar["recorded_frames"]]
    nl = sum(len(f["lights"]) for f in frames)
    print(f"PASS: {args.app}: {n_ref} draw(s) match the sidecar reference (material, texture stage, fog, lights, "
          f"viewport, camera within 1e-5); {n_exp} expectation check(s); {nl} light(s) in the recorded frame(s)")
    return 0


# ---------------------------------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------------------------------

EXPECT_KEYS = {"index", "api", "why", "translated", "camera", "material", "fog", "texgen", "lights", "status",
               "alpha_swizzle", "z_write", "z_enable", "stencil", "viewport"}


def synthetic():
    """A one-draw sidecar and the matching replay output (written from the reference itself)."""
    light = {"index": 0, "enabled": True, "type": "SPOT", "diffuse": [F(0.2), F(0.4), 1.0, 1.0],
             "specular": [0, 0, 0, 0], "ambient": [0, 0, 0, 0], "position": [1.5, 2.0, -1.5],
             "direction": [F(-0.249136448), F(-0.830454767), F(0.498272896)], "range": 10.0, "falloff": 1.0,
             "attenuation": [1.0, 0.0, 0.0], "theta": F(0.35), "phi": F(0.9)}
    proj = [F(1.29903817), 0, 0, 0, 0, F(1.7320509), 0, 0, 0, 0, F(1.00502515), 1, 0, 0, F(-0.502512574), 0]
    view = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 4.0, 1.0]
    block = {"render_states": {"LIGHTING": 1, "FOGENABLE": 1, "FOGVERTEXMODE": 3, "FOGCOLOR": 0xFF8090A0,
                               "FOGSTART": fbits(2.0), "FOGEND": fbits(22.0)},
             "texture_stages": [], "textures": [], "lights": [light], "material": None,
             "viewport": {"x": 0, "y": 0, "width": 128, "height": 96, "min_z": 0, "max_z": 1}}
    elements = [{"usage": "POSITION", "usage_index": 0}, {"usage": "COLOR", "usage_index": 0}]
    sidecar = {"api": "d3d9", "device": {"backbuffer_format": "X8R8G8B8"}, "recorded_frames": [1],
               "state_blocks": [block], "textures": [], "blobs": {},
               "annotations": {"camera": {"fov_y": F(math.pi / 3), "aspect": F(4 / 3), "z_near": 0.5, "z_far": 100.0,
                                          "eye": [0.0, 0.0, -4.0]}},
               "draws": [{"frame": 1, "state": 0, "vertex_format": {"elements": elements},
                          "transforms": {"PROJECTION": proj, "VIEW": view}}]}
    s = State(block, {})
    m = ref_material(s, elements, True, False, False)
    texgen = ref_texture_stage(s, m)
    lights = LightRef().draw([light])
    fog = ref_fog(s)
    cam = ref_camera(proj, view)
    cam.update(type="Main")
    draw = {"ev": "draw", "frame": 1, "index": 0, "translated": True, "material": m, "fog": fog, "texgen": texgen,
            "lights": lights, "camera": "Main", "viewport": [0, 0, 128, 96]}
    frame = {"ev": "frame", "frame": 1, "lights": lights, "fog": fog, "fog_states": [fog], "cameras": [cam],
             "camera_cut": False}
    return sidecar, [draw, frame]


def cmd_selftest(args):
    problems = 0
    load_hash_ref(args.hash_ref)
    for name in sorted(os.listdir(args.expect)):
        with open(os.path.join(args.expect, name)) as f:
            e = json.load(f)
        if e.get("schema") != "fuse.relight.translate_expect/1" or "draws" not in e:
            print(f"FAIL: {name}: bad schema")
            problems += 1
            continue
        for d in e["draws"]:
            unknown = set(d) - EXPECT_KEYS
            if unknown or "index" not in d:
                print(f"FAIL: {name}: bad draw entry {d}")
                problems += 1
    sidecar, good = synthetic()
    errors = []
    check_reference(sidecar, good, errors)
    if errors:
        print("FAIL: the reference rejects its own output:")
        for e in errors:
            print("  " + e)
        problems += 1
    if sidecar["annotations"]["camera"]["z_far"] != 100.0 or not good[1]["lights"]:
        problems += 1
    # Seeded mismatches that must be caught.
    seeds = [
        ("light hash", lambda o: o[0]["lights"][0].__setitem__("hash", "0x0000000000000001")),
        ("light radiance", lambda o: o[0]["lights"][0]["radiance"].__setitem__(0, o[0]["lights"][0]["radiance"][0] * 1.001)),
        ("blend factor", lambda o: o[0]["material"].__setitem__("color_src", 6)),
        ("alpha swizzle", lambda o: o[0]["material"].__setitem__("color_dst", 9)),
        ("fog scale", lambda o: o[0]["fog"].__setitem__("scale", 0.051)),
        ("fog hash", lambda o: o[0]["fog"].__setitem__("hash", "0x0000000000000002")),
        ("camera fov 2e-5", lambda o: o[1]["cameras"][0].__setitem__("fov", o[1]["cameras"][0]["fov"] * (1 + 2e-5))),
        ("camera type", lambda o: o[0].__setitem__("camera", "Sky")),
        ("frame light list", lambda o: o[1].__setitem__("lights", [])),
        ("viewport rectangle", lambda o: o[0].__setitem__("viewport", [64, 0, 64, 48])),
        ("emissive source", lambda o: o[0]["material"].__setitem__("emissive_source", "VertexColor0")),
    ]
    for what, mutate in seeds:
        out = json.loads(json.dumps(good))
        mutate(out)
        errors = []
        check_reference(sidecar, out, errors)
        if not errors:
            print(f"FAIL: seeded mismatch not caught: {what}")
            problems += 1
    # The expectation matcher: ranges, api filter, frame fields.
    exp = {"draws": [{"index": [0, 3], "camera": "Main"}, {"index": 0, "api": "d3d8", "camera": "Sky"}],
           "frame": {"light_count": 1, "light_types": {"sphere": 1}, "shaped_lights": 1, "cameras": ["Main"]},
           "draw_count": 1}
    errors = []
    check_expectations(exp, sidecar, good, errors)
    if errors:
        print("FAIL: expectation matcher rejects a matching stream: " + "; ".join(errors))
        problems += 1
    exp["frame"]["light_count"] = 2
    errors = []
    check_expectations(exp, sidecar, good, errors)
    if len(errors) != 1:
        print("FAIL: expectation matcher misses a seeded light-count mismatch")
        problems += 1
    if problems:
        return 1
    print(f"PASS: expectation files parse; the reference reproduces itself; {len(seeds)} seeded mismatches caught")
    return 0


def cmd_roundtrip(args):
    os.makedirs(args.out, exist_ok=True)
    stream = os.path.join(args.out, "roundtrip_tap.jsonl")
    expected = os.path.join(args.out, "roundtrip_expected.jsonl")
    got_path = os.path.join(args.out, "roundtrip_replayed.jsonl")
    emu = [p for p in args.emulator.split("|") if p] if args.emulator else []
    proc = subprocess.run(emu + [args.gen, "record", stream, expected], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        print(proc.stdout.decode(errors="replace").strip()[-4000:])
        print("FAIL: roundtrip: the recording session exited with %d" % proc.returncode)
        return 1
    cmd = emu + [args.replay, "--stream", stream, "--out", got_path]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0 or not os.path.isfile(got_path):
        print(proc.stdout.decode(errors="replace").strip()[-4000:])
        print("FAIL: roundtrip: rl_translate_replay exited with %d" % proc.returncode)
        return 1
    want, got = read_stream(expected), read_stream(got_path)
    rec = read_stream(stream)
    errors = []
    blocks = [e["state"] for e in rec if e["ev"] == "state_block"]
    if not any(any(any(v != 0 for v in p) for p in b.get("clip_planes", [])) for b in blocks):
        errors.append("the recorded stream has no non-zero clip plane")
    if not all("lights_version" in e and "clip_planes_version" in e for e in rec if e["ev"] == "draw"):
        errors.append("recorded draws lack the change counters")
    if len(want) != len(got):
        errors.append("in-process output has %d line(s), the replay %d" % (len(want), len(got)))
    for i, (w, g) in enumerate(zip(want, got)):
        if w != g:
            keys = sorted(k for k in set(w) | set(g) if w.get(k) != g.get(k))
            errors.append("line %d (%s frame %s): differs in %s" % (i, w.get("ev"), w.get("frame"), keys))
    if not any(g.get("clip_plane") for g in got):
        errors.append("no replayed draw uses a clip plane")
    if errors:
        print("FAIL: roundtrip: %d problem(s):" % len(errors))
        for e in errors[:40]:
            print("  " + e)
        return 1
    clipped = sum(1 for g in got if g.get("clip_plane"))
    print("PASS: roundtrip: %d replayed line(s) equal TranslateTap in-process (%d draw(s) with a clip plane; "
          "recorded clip planes, change counters and alpha-swizzle mask)" % (len(got), clipped))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["capture", "selftest", "roundtrip"])
    p.add_argument("--gen")
    p.add_argument("--exe")
    p.add_argument("--app")
    p.add_argument("--d3d9")
    p.add_argument("--d3d8")
    p.add_argument("--runner")
    p.add_argument("--prefix-root")
    p.add_argument("--replay")
    p.add_argument("--emulator", default="")
    p.add_argument("--expect")
    p.add_argument("--hash-ref")
    p.add_argument("--from-run")
    p.add_argument("--out")
    args = p.parse_args()
    if args.mode == "roundtrip":
        if not (args.gen and args.replay and args.out):
            p.error("roundtrip needs --gen, --replay and --out")
        return cmd_roundtrip(args)
    if not (args.expect and args.hash_ref):
        p.error("--expect and --hash-ref are required")
    if args.mode == "selftest":
        return cmd_selftest(args)
    if not args.from_run and not shutil.which("wine") and not shutil.which("wine64"):
        print("SKIP: wine not installed")
        return SKIP
    return cmd_capture(args)


if __name__ == "__main__":
    sys.exit(main())
