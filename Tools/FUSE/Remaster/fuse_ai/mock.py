"""Deterministic CPU mock providers for every Task (plan §3.1, the ``mock`` row; §3.5 "CI stays CPU-only").

Like ``fuse_nvplugin_mock``, these test plumbing, not quality. Every handler is a pure function of the
request (task, params, seed, input bytes / content hashes) using integer arithmetic only, so outputs are
byte-identical across runs, Python versions and platforms (the tests pin sha256s of a few outputs).

==============  ==========================================================================================
Task            Mock behaviour
==============  ==========================================================================================
UPSCALE         Catmull-Rom bicubic x``scale`` (default 4), fixed-point weights, clamped edges -> PNG
PBR_MAPS        Sobel-from-luma tangent-space normal map (wrapping), roughness = 255 - luma, height = luma
TAG_IMAGE       keyword table on the input name / ``hint`` + colour / brightness / contrast tags -> JSON
INFER_MATERIAL  base colour = mean, roughness from luma spread, metallic from the keyword table -> JSON
EDIT_IMAGE      25 % tint towards a colour derived from sha256(prompt) -> PNG
GEN_IMAGE       synthetic gradient/checker from (prompt, seed) -> PNG
SEGMENT         binary mask, luma >= mean luma -> PNG
SUMMARISE_CODE  line / function counts and the function list -> Markdown
PORT_SCRIPT     canned Lua module with one TODO stub per detected TorqueScript function
GEN_VIDEO       ``frames`` PNG frames tinted by (prompt, seed) + a JSON manifest
WORLD_ROLLOUT   a fixed 8-frame sequence (moving bar, 32x32) + a JSON manifest
TRANSCRIBE      canned transcript with one segment per 500 ms of audio -> JSON
TTS_SYNTH       one 60 ms triangle tone per character of the text -> 16 kHz mono WAV
IMAGE_TO_3D     9x9 heightfield from the luma of the input -> Wavefront OBJ
SEPARATE_STEMS  bass = 8-tap box low-pass, vocals/other split the remainder, drums silent (sum == input)
AUDIO_SR        2x linear-interpolation upsample -> WAV
==============  ==========================================================================================
"""

from __future__ import annotations

import hashlib
import json
import math
import re
from fractions import Fraction
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

from . import imaging
from .errors import RecipeError
from .imaging import Image
from .providers import ALL_TASKS, Cost, Output, Request, Result, Task

MOCK_MODEL_ID = "fuse-mock/1"
MAX_UPSCALE_INPUT_PIXELS = 128 * 128  # pure Python: keep CI fast

PNG = "image/png"
JSON = "application/json"
WAV = "audio/wav"


def _json_bytes(obj: Any) -> bytes:
    return (json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode("ascii")


def _first_input(req: Request) -> Tuple[Optional[bytes], str, str]:
    """(bytes or None, content key, name hint) of input 0 (or empty placeholders)."""
    if not req.inputs:
        return None, "no-input", ""
    ref = req.inputs[0]
    data = req.input_data[0] if req.input_data else None
    hint = " ".join(x for x in (ref.name, ref.oaid) if x)  # hashed fields only (path is not)
    return data, ref.content_key or "no-hash", hint


def _input_image(req: Request) -> Image:
    data, key, _ = _first_input(req)
    return imaging.image_from_input(data, key)


def _input_text(req: Request) -> str:
    if "text" in req.params:
        return str(req.params["text"])
    data, key, _ = _first_input(req)
    if data is None:
        return f"-- no input bytes (content key {key})\n"
    return data.decode("utf-8", errors="replace")


def _input_audio(req: Request) -> Tuple[List[int], int, int]:
    data, key, _ = _first_input(req)
    if data is not None and data[:4] == b"RIFF":
        return imaging.decode_wav(data)
    return imaging.synthetic_audio(data if data is not None else key.encode()), 16000, 1


def _digest(*parts: Any) -> bytes:
    h = hashlib.sha256()
    for p in parts:
        h.update(str(p).encode("utf-8"))
        h.update(b"\0")
    return h.digest()


# ---- UPSCALE: fixed-point Catmull-Rom ---------------------------------------------------------------------------

_W_ONE = 1 << 12


def _catmull_rom(t: Fraction) -> List[Fraction]:
    """Weights for taps -1, 0, +1, +2 at fractional offset t (a = -0.5)."""
    t2, t3 = t * t, t * t * t
    return [(-t3 + 2 * t2 - t) / 2, (3 * t3 - 5 * t2 + 2) / 2, (-3 * t3 + 4 * t2 + t) / 2, (t3 - t2) / 2]


def _taps(src_len: int, scale: int) -> List[Tuple[List[int], List[int]]]:
    out = []
    for d in range(src_len * scale):
        pos = Fraction(2 * d + 1 - scale, 2 * scale)  # (d + 0.5) / scale - 0.5
        base = math.floor(pos)
        weights = _catmull_rom(pos - base)
        iw = [round(w * _W_ONE) for w in weights]
        iw[1] += _W_ONE - sum(iw)  # exact unit gain
        idx = [min(max(base + k, 0), src_len - 1) for k in (-1, 0, 1, 2)]
        out.append((idx, iw))
    return out


def bicubic_upscale(img: Image, scale: int) -> Image:
    if scale < 1 or scale > 8:
        raise RecipeError(f"mock UPSCALE supports scale 1..8, got {scale}")
    if img.width * img.height > MAX_UPSCALE_INPUT_PIXELS:
        raise RecipeError(f"mock UPSCALE input {img.width}x{img.height} is larger than {MAX_UPSCALE_INPUT_PIXELS} px")
    w, h = img.width, img.height
    src = img.rgba
    xt, yt = _taps(w, scale), _taps(h, scale)
    ow, oh = w * scale, h * scale
    # horizontal pass -> rows of ints scaled by _W_ONE
    tmp: List[List[int]] = []
    for y in range(h):
        row_off = y * w * 4
        row = [0] * (ow * 4)
        for x, (idx, iw) in enumerate(xt):
            for c in range(4):
                row[x * 4 + c] = sum(src[row_off + i * 4 + c] * wt for i, wt in zip(idx, iw))
        tmp.append(row)
    out = bytearray(ow * oh * 4)
    denom = _W_ONE * _W_ONE
    half = denom // 2
    for y, (idx, iw) in enumerate(yt):
        rows = [tmp[i] for i in idx]
        base = y * ow * 4
        for j in range(ow * 4):
            v = (rows[0][j] * iw[0] + rows[1][j] * iw[1] + rows[2][j] * iw[2] + rows[3][j] * iw[3] + half) // denom
            out[base + j] = 0 if v < 0 else (255 if v > 255 else v)
    return Image(ow, oh, bytes(out))


def _upscale(req: Request):
    img = _input_image(req)
    scale = int(req.params.get("scale", 4))
    up = bicubic_upscale(img, scale)
    return [Output("upscaled.png", PNG, imaging.encode_png(up))], {"width": up.width, "height": up.height,
                                                                     "filter": "catmull-rom", "scale": scale}


# ---- PBR_MAPS ---------------------------------------------------------------------------------------------------


def sobel_normal_map(img: Image, strength: int = 2) -> Image:
    w, h = img.width, img.height
    L = img.luma()

    def at(x: int, y: int) -> int:
        return L[(y % h) * w + (x % w)]

    out = bytearray()
    for y in range(h):
        for x in range(w):
            gx = (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1))
            gy = (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1))
            nx, ny, nz = -gx * strength, -gy * strength, 1020
            ln = math.isqrt(nx * nx + ny * ny + nz * nz)
            out += bytes((128 + (nx * 127) // ln, 128 + (ny * 127) // ln, 128 + (nz * 127) // ln, 255))
    return Image(w, h, bytes(out))


def _grey(img: Image, values: List[int]) -> Image:
    return Image(img.width, img.height, b"".join(bytes((v, v, v, 255)) for v in values))


def _pbr(req: Request):
    img = _input_image(req)
    strength = int(req.params.get("normal_strength", 2))
    luma = img.luma()
    outs = [
        Output("normal.png", PNG, imaging.encode_png(sobel_normal_map(img, strength))),
        Output("roughness.png", PNG, imaging.encode_png(_grey(img, [255 - v for v in luma]))),
        Output("height.png", PNG, imaging.encode_png(_grey(img, luma))),
    ]
    return outs, {"width": img.width, "height": img.height, "normal": "sobel-luma", "normal_strength": strength}


# ---- image analysis: TAG_IMAGE / INFER_MATERIAL / SEGMENT -------------------------------------------------------

KEYWORD_TAGS: Dict[str, List[str]] = {
    "brick": ["brick", "masonry", "wall"],
    "wall": ["wall"],
    "stone": ["stone", "rock"],
    "rock": ["stone", "rock"],
    "wood": ["wood", "organic"],
    "plank": ["wood", "planks"],
    "metal": ["metal"],
    "rust": ["metal", "rust"],
    "steel": ["metal"],
    "grass": ["grass", "foliage", "organic"],
    "leaf": ["foliage", "organic"],
    "water": ["water", "liquid"],
    "sky": ["sky"],
    "cloth": ["fabric"],
    "fabric": ["fabric"],
    "glass": ["glass", "transparent"],
    "tile": ["tile", "ceramic"],
    "concrete": ["concrete"],
    "dirt": ["dirt", "ground"],
    "sand": ["sand", "ground"],
    "skin": ["skin", "character"],
    "face": ["character"],
    "ui": ["ui"],
    "font": ["ui", "font"],
}
METAL_TAGS = {"metal"}


def _stats(img: Image) -> Dict[str, int]:
    n = img.width * img.height
    px = img.rgba
    sr = sum(px[0::4]); sg = sum(px[1::4]); sb = sum(px[2::4]); sa = sum(px[3::4])
    luma = img.luma()
    mean_l = sum(luma) // n
    spread = math.isqrt(sum((v - mean_l) ** 2 for v in luma) // n)
    return {"r": sr // n, "g": sg // n, "b": sb // n, "a": sa // n, "luma": mean_l, "spread": spread}


def _keyword_tags(text: str) -> List[str]:
    words = re.findall(r"[a-z]+", text.lower())
    tags: List[str] = []
    for w in words:
        for key, vals in KEYWORD_TAGS.items():
            if w == key or w.startswith(key):
                tags.extend(vals)
    return tags


def _colour_tags(s: Dict[str, int]) -> List[str]:
    tags = []
    tags.append("dark" if s["luma"] < 64 else ("bright" if s["luma"] > 192 else "midtone"))
    r, g, b = s["r"], s["g"], s["b"]
    if max(r, g, b) - min(r, g, b) < 24:
        tags.append("grey")
    elif r >= g and r >= b:
        tags.append("red")
    elif g >= r and g >= b:
        tags.append("green")
    else:
        tags.append("blue")
    tags.append("noisy" if s["spread"] > 48 else "smooth")
    if s["a"] < 250:
        tags.append("alpha")
    return tags


def _tag(req: Request):
    img = _input_image(req)
    _, _, hint = _first_input(req)
    s = _stats(img)
    tags = sorted(set(_keyword_tags(hint + " " + str(req.params.get("hint", ""))) + _colour_tags(s)))
    return [Output("tags.json", JSON, _json_bytes({"tags": tags, "stats": s}))], {"n_tags": len(tags)}


def _material(req: Request):
    img = _input_image(req)
    _, _, hint = _first_input(req)
    s = _stats(img)
    kw = set(_keyword_tags(hint + " " + str(req.params.get("hint", ""))))
    mat = {
        "base_color": [s["r"], s["g"], s["b"]],
        "metallic": 1000 if kw & METAL_TAGS else 0,
        "roughness": min(1000, 350 + s["spread"] * 8),
        "normal_strength": 1000 if s["spread"] > 32 else 500,
        "units": "milli",  # integer thousandths, so the JSON never depends on float formatting
        "keywords": sorted(kw),
    }
    return [Output("material.json", JSON, _json_bytes(mat))], {}


def _segment(req: Request):
    img = _input_image(req)
    luma = img.luma()
    mean_l = sum(luma) // len(luma)
    mask = _grey(img, [255 if v >= mean_l else 0 for v in luma])
    return [Output("mask.png", PNG, imaging.encode_png(mask))], {"threshold": mean_l}


# ---- generative image / video -----------------------------------------------------------------------------------


def _tint(img: Image, colour: Tuple[int, int, int], weight: int = 64) -> Image:
    out = bytearray(img.rgba)
    for i in range(0, len(out), 4):
        for c in range(3):
            out[i + c] = (out[i + c] * (256 - weight) + colour[c] * weight) >> 8
    return Image(img.width, img.height, bytes(out))


def _edit(req: Request):
    img = _input_image(req)
    d = _digest("edit", req.params.get("prompt", ""))
    edited = _tint(img, (d[0], d[1], d[2]))
    return [Output("edited.png", PNG, imaging.encode_png(edited))], {"tint": [d[0], d[1], d[2]]}


def _gen_image(req: Request):
    size = min(256, max(4, int(req.params.get("size_px", 64))))
    seed = _digest("gen_image", req.params.get("prompt", ""), req.seed)
    img = imaging.synthetic_image(seed, size, size)
    return [Output("image.png", PNG, imaging.encode_png(img))], {"width": size, "height": size}


def _frames_manifest(frames: List[bytes], fps: int, kind: str) -> Output:
    return Output("manifest.json", JSON, _json_bytes({
        "kind": kind, "fps": fps, "frames": [hashlib.sha256(f).hexdigest() for f in frames]}))


def _gen_video(req: Request):
    n = min(64, max(1, int(req.params.get("frames", 8))))
    base = imaging.synthetic_image(_digest("gen_video", req.params.get("prompt", ""), req.seed), 32, 18)
    frames = []
    for i in range(n):
        frames.append(imaging.encode_png(_tint(base, ((i * 37) & 255, (i * 91) & 255, (i * 13) & 255), 96)))
    outs = [_frames_manifest(frames, 24, "video")] + [Output(f"frame_{i:03d}.png", PNG, f) for i, f in enumerate(frames)]
    return outs, {"frames": n, "fps": 24}


def _world_rollout(req: Request):
    size, n = 32, 8
    frames = []
    for i in range(n):
        bar = i * size // n
        px = bytearray()
        for y in range(size):
            for x in range(size):
                on = bar <= x < bar + size // n
                px += bytes((240, 200, 40, 255)) if on else bytes((20, 30 + y * 4, 60, 255))
        frames.append(imaging.encode_png(Image(size, size, bytes(px))))
    outs = [_frames_manifest(frames, 25, "world_rollout")] + [Output(f"frame_{i:03d}.png", PNG, f)
                                                               for i, f in enumerate(frames)]
    return outs, {"frames": n, "fps": 25, "distribution": "never"}


# ---- code -------------------------------------------------------------------------------------------------------

_FN_RE = re.compile(r"^\s*function\s+([A-Za-z_][\w]*(?:::[A-Za-z_]\w*)?)\s*\(([^)]*)\)", re.MULTILINE)


def _functions(text: str) -> List[Tuple[str, str]]:
    return [(m.group(1), m.group(2).strip()) for m in _FN_RE.finditer(text)]


def _summarise(req: Request):
    text = _input_text(req)
    fns = _functions(text)
    lines = text.splitlines()
    md = ["# Mock summary (fuse_ai mock SUMMARISE_CODE)", "",
          f"- source sha256: `{hashlib.sha256(text.encode('utf-8')).hexdigest()}`",
          f"- lines: {len(lines)}", f"- functions: {len(fns)}", ""]
    md += [f"- `{name}({args})`" for name, args in fns]
    return [Output("summary.md", "text/markdown", ("\n".join(md) + "\n").encode("utf-8"))], {"functions": len(fns)}


def _lua_name(name: str) -> str:
    return name.replace("::", "_")


def _port(req: Request):
    text = _input_text(req)
    fns = _functions(text)
    lua = ["-- fuse_ai mock PORT_SCRIPT output (canned; tests plumbing, not quality)",
           f"-- source sha256: {hashlib.sha256(text.encode('utf-8')).hexdigest()}",
           "local M = {}", ""]
    for name, args in fns:
        params = ", ".join(_lua_name(a.strip().lstrip("%")) for a in args.split(",") if a.strip())
        lua += [f"function M.{_lua_name(name)}({params})",
                f"  -- TODO(port): body of {name} is not ported by the mock provider",
                "end", ""]
    lua.append("return M")
    return [Output("port.lua", "text/x-lua", ("\n".join(lua) + "\n").encode("utf-8"))], {"functions": len(fns)}


# ---- audio ------------------------------------------------------------------------------------------------------


def _transcribe(req: Request):
    samples, rate, ch = _input_audio(req)
    dur_ms = (len(samples) // max(1, ch)) * 1000 // max(1, rate)
    segs = [{"start_ms": t, "end_ms": min(dur_ms, t + 500), "text": f"[mock segment {i}]"}
            for i, t in enumerate(range(0, max(dur_ms, 1), 500))]
    out = {"language": "en", "text": " ".join(s["text"] for s in segs), "segments": segs, "duration_ms": dur_ms}
    return [Output("transcript.json", JSON, _json_bytes(out))], {"duration_ms": dur_ms}


def _tts(req: Request):
    text = str(req.params.get("text", "")) or _input_text(req)
    rate = 16000
    seg = rate * 60 // 1000
    samples: List[int] = []
    for chv in text[:200]:
        period = 16 + (ord(chv) % 64)
        half = period // 2
        amp = 0 if chv.isspace() else 6000
        for i in range(seg):
            ph = i % period
            samples.append((ph * 2 * amp) // half - amp if ph < half else amp - ((ph - half) * 2 * amp) // (period - half))
    return [Output("speech.wav", WAV, imaging.encode_wav(samples, rate))], {"chars": min(len(text), 200),
                                                                              "voice": "mock-synthetic"}


def _box_lowpass(x: List[int], taps: int = 8) -> List[int]:
    out, acc = [], 0
    for i, v in enumerate(x):
        acc += v
        if i >= taps:
            acc -= x[i - taps]
        out.append(acc // taps)
    return out


def _stems(req: Request):
    samples, rate, ch = _input_audio(req)
    chans = [samples[c::ch] for c in range(ch)]
    stems: Dict[str, List[List[int]]] = {"vocals": [], "drums": [], "bass": [], "other": []}
    for x in chans:
        bass = _box_lowpass(x)
        hp = [a - b for a, b in zip(x, bass)]
        vocals = [v // 2 for v in hp]
        stems["bass"].append(bass)
        stems["vocals"].append(vocals)
        stems["other"].append([h - v for h, v in zip(hp, vocals)])
        stems["drums"].append([0] * len(x))
    outs = []
    for name in ("vocals", "drums", "bass", "other"):
        inter = [stems[name][c][i] for i in range(len(chans[0])) for c in range(ch)]
        outs.append(Output(f"{name}.wav", WAV, imaging.encode_wav(inter, rate, ch)))
    return outs, {"stems": ["vocals", "drums", "bass", "other"], "rate": rate}


def _audio_sr(req: Request):
    samples, rate, ch = _input_audio(req)
    chans = [samples[c::ch] for c in range(ch)]
    up = []
    for x in chans:
        y = []
        for i, v in enumerate(x):
            nxt = x[i + 1] if i + 1 < len(x) else v
            y += [v, (v + nxt) >> 1]
        up.append(y)
    inter = [up[c][i] for i in range(len(up[0])) for c in range(ch)]
    return [Output("upsampled.wav", WAV, imaging.encode_wav(inter, rate * 2, ch))], {"rate": rate * 2}


# ---- 3D ---------------------------------------------------------------------------------------------------------


def _image_to_3d(req: Request):
    img = _input_image(req)
    L = img.luma()
    n = 9
    lines = ["# fuse_ai mock IMAGE_TO_3D heightfield (Wavefront OBJ)", "o mock_blockout"]
    for j in range(n):
        for i in range(n):
            x = i * (img.width - 1) // (n - 1)
            y = j * (img.height - 1) // (n - 1)
            hgt = L[y * img.width + x]
            lines.append(f"v {i * 125 - 500} {hgt * 2} {j * 125 - 500}")
    for j in range(n - 1):
        for i in range(n - 1):
            a = j * n + i + 1
            lines.append(f"f {a} {a + n} {a + n + 1} {a + 1}")
    text = "\n".join(lines) + "\n"
    return [Output("blockout.obj", "model/obj", text.encode("ascii"))], {"vertices": n * n, "units": "mm"}


# ---- the provider -----------------------------------------------------------------------------------------------

HANDLERS: Dict[Task, Callable[[Request], Tuple[List[Output], Dict[str, Any]]]] = {
    Task.UPSCALE: _upscale,
    Task.PBR_MAPS: _pbr,
    Task.TAG_IMAGE: _tag,
    Task.INFER_MATERIAL: _material,
    Task.EDIT_IMAGE: _edit,
    Task.GEN_IMAGE: _gen_image,
    Task.SEGMENT: _segment,
    Task.SUMMARISE_CODE: _summarise,
    Task.PORT_SCRIPT: _port,
    Task.GEN_VIDEO: _gen_video,
    Task.WORLD_ROLLOUT: _world_rollout,
    Task.TRANSCRIBE: _transcribe,
    Task.TTS_SYNTH: _tts,
    Task.IMAGE_TO_3D: _image_to_3d,
    Task.SEPARATE_STEMS: _stems,
    Task.AUDIO_SR: _audio_sr,
}
assert set(HANDLERS) == set(ALL_TASKS)


class MockProvider:
    """The ``mock`` provider: deterministic, CPU-only, never touches the network."""

    id = "mock"
    cloud = False

    def capabilities(self) -> Set[Task]:
        return set(ALL_TASKS)

    def vram_mb(self, req: Request) -> int:
        return 0

    def estimate(self, req: Request) -> Cost:
        return Cost()

    def run(self, req: Request) -> Result:
        outputs, meta = HANDLERS[req.task](req)
        meta = dict(meta, mock=True, task=req.task.value)
        return Result(outputs=outputs, meta=meta, cost=Cost(), provider_id=self.id, model_id=MOCK_MODEL_ID)
