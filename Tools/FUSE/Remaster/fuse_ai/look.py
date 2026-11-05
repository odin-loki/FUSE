"""W5.4: look presets Classic / Remastered / Stylised and a 3D LUT fitted to captures (plan §4.4 step 2).

Output is the renderer's own format: a ``.fuselook`` JSON (``kind: fuse.look``, ``schemaVersion: 1``, see
``Source/FUSE/Renderer/include/fuse/renderer/look/look_schema.hpp`` and ``Samples/Looks/``) whose
``colorGrade.lut`` names a ``.cube`` file (Adobe Cube LUT 1.0, red fastest, written like ``lut_write_cube``).
The renderer applies the grade LUT to display-referred, sRGB-encoded colour after tone mapping with
**tetrahedral** interpolation (``GradeLutKernel`` / ``lut_sample_tetrahedral``); :func:`sample_tetrahedral` is a
line-by-line port so the fit is evaluated exactly the way the engine will sample it.

* :func:`fit_lut` fits an N³ LUT (default 17) to (render colour, capture colour) pairs - e.g. a FUSE render
  of a level vs the original game's golden capture of the same view, or a colour chart - by iterative
  residual splatting: predict, splat each residual onto its 8 lattice nodes with trilinear weights, add the
  weighted mean, repeat; nodes no sample touches stay at identity.
* Presets live in ``Content/recipes/ai/looks/<name>.look-preset.json``: ``classic`` (match the original
  game's grade with a fitted LUT, ``reinhard`` tone map), ``remastered`` (neutral ``filmic``, no
  LUT), ``stylised`` (a creative grade). :func:`emit` writes ``<out>/<name>.fuselook`` and
  ``<out>/luts/<name>.cube``.
* :func:`synthetic_grade` is the W5.4 gate's known grade (lift/gamma/gain, saturation, warm shift, S-curve);
  the fitted LUT must reproduce it on held-out colours with mean ΔE*ab (CIE76) < 2.
"""

from __future__ import annotations

import json
import math
import random
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .errors import RecipeError
from .fimage import delta_e76

Vec3 = Tuple[float, float, float]
LOOK_KIND = "fuse.look"
_REPO_ROOT = Path(__file__).resolve().parents[4]


def presets_dir() -> Path:
    return _REPO_ROOT / "Content" / "recipes" / "ai" / "looks"


def _sat(v: float) -> float:
    return 0.0 if v < 0.0 else 1.0 if v > 1.0 else v


class Lut3D:
    def __init__(self, size: int, data: Optional[List[List[float]]] = None, title: str = ""):
        if size < 2 or size > 256:
            raise RecipeError("LUT size must be 2..256")
        self.size = size
        self.title = title
        if data is None:
            inv = 1.0 / (size - 1)
            data = [[r * inv, g * inv, b * inv] for b in range(size) for g in range(size) for r in range(size)]
        if len(data) != size ** 3:
            raise RecipeError("LUT data has the wrong length")
        self.data = data

    def index(self, r: int, g: int, b: int) -> int:
        return r + g * self.size + b * self.size * self.size  # lut_index(): red fastest


def sample_tetrahedral(lut: Lut3D, c: Sequence[float]) -> Vec3:
    """Port of ``kernels::lut_sample_tetrahedral`` (look_kernels.hpp)."""
    n = lut.size
    scale = float(n - 1)
    fr, fg, fb = _sat(c[0]) * scale, _sat(c[1]) * scale, _sat(c[2]) * scale
    r0, g0, b0 = min(int(fr), n - 2), min(int(fg), n - 2), min(int(fb), n - 2)
    dr, dg, db = fr - r0, fg - g0, fb - b0
    L = lut.data
    ix = lut.index
    c000, c111 = L[ix(r0, g0, b0)], L[ix(r0 + 1, g0 + 1, b0 + 1)]
    if dr > dg:
        if dg > db:  # r > g > b
            c100, c110 = L[ix(r0 + 1, g0, b0)], L[ix(r0 + 1, g0 + 1, b0)]
            return tuple(c000[i] + (c100[i] - c000[i]) * dr + (c110[i] - c100[i]) * dg + (c111[i] - c110[i]) * db
                         for i in range(3))
        if dr > db:
            c100, c101 = L[ix(r0 + 1, g0, b0)], L[ix(r0 + 1, g0, b0 + 1)]
            return tuple(c000[i] + (c100[i] - c000[i]) * dr + (c101[i] - c100[i]) * db + (c111[i] - c101[i]) * dg
                         for i in range(3))
        c001, c101 = L[ix(r0, g0, b0 + 1)], L[ix(r0 + 1, g0, b0 + 1)]
        return tuple(c000[i] + (c001[i] - c000[i]) * db + (c101[i] - c001[i]) * dr + (c111[i] - c101[i]) * dg
                     for i in range(3))
    if db > dg:
        c001, c011 = L[ix(r0, g0, b0 + 1)], L[ix(r0, g0 + 1, b0 + 1)]
        return tuple(c000[i] + (c001[i] - c000[i]) * db + (c011[i] - c001[i]) * dg + (c111[i] - c011[i]) * dr
                     for i in range(3))
    if db > dr:
        c010, c011 = L[ix(r0, g0 + 1, b0)], L[ix(r0, g0 + 1, b0 + 1)]
        return tuple(c000[i] + (c010[i] - c000[i]) * dg + (c011[i] - c010[i]) * db + (c111[i] - c011[i]) * dr
                     for i in range(3))
    c010, c110 = L[ix(r0, g0 + 1, b0)], L[ix(r0 + 1, g0 + 1, b0)]
    return tuple(c000[i] + (c010[i] - c000[i]) * dg + (c110[i] - c010[i]) * dr + (c111[i] - c110[i]) * db
                 for i in range(3))


def fit_lut(pairs: Sequence[Tuple[Sequence[float], Sequence[float]]], size: int = 17, iterations: int = 6,
            title: str = "") -> Lut3D:
    lut = Lut3D(size, title=title)
    s = size - 1
    touched: set = set()
    for _ in range(iterations):
        num = [[0.0, 0.0, 0.0] for _ in range(size ** 3)]
        den = [0.0] * (size ** 3)
        for src, dst in pairs:
            pred = sample_tetrahedral(lut, src)
            res = [dst[i] - pred[i] for i in range(3)]
            fr, fg, fb = _sat(src[0]) * s, _sat(src[1]) * s, _sat(src[2]) * s
            r0, g0, b0 = min(int(fr), s - 1), min(int(fg), s - 1), min(int(fb), s - 1)
            dr, dg, db = fr - r0, fg - g0, fb - b0
            for cr, wr in ((r0, 1 - dr), (r0 + 1, dr)):
                for cg, wg in ((g0, 1 - dg), (g0 + 1, dg)):
                    for cb, wb in ((b0, 1 - db), (b0 + 1, db)):
                        w = wr * wg * wb
                        if w <= 0.0:
                            continue
                        k = lut.index(cr, cg, cb)
                        num[k][0] += w * res[0]
                        num[k][1] += w * res[1]
                        num[k][2] += w * res[2]
                        den[k] += w
        for k in range(size ** 3):
            if den[k] > 1e-9:
                touched.add(k)
                for i in range(3):
                    lut.data[k][i] += num[k][i] / den[k]
    _fill_untouched(lut, touched)
    for node in lut.data:
        for i in range(3):
            node[i] = _sat(node[i])
    return lut


def _fill_untouched(lut: Lut3D, touched: set) -> None:
    """Nodes no capture sample reached take the mean offset (from identity) of their filled 6-neighbours,
    growing outwards, so sparse captures do not leave identity holes inside a graded LUT."""
    n = lut.size
    inv = 1.0 / (n - 1)
    ident = lambda k: (k % n * inv, k // n % n * inv, k // (n * n) * inv)  # noqa: E731
    filled = set(touched)
    if not filled:
        return
    while len(filled) < n ** 3:
        new = {}
        for k in range(n ** 3):
            if k in filled:
                continue
            r, g, b = k % n, k // n % n, k // (n * n)
            offs = []
            for dr, dg, db in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
                rr, gg, bb = r + dr, g + dg, b + db
                if 0 <= rr < n and 0 <= gg < n and 0 <= bb < n:
                    j = lut.index(rr, gg, bb)
                    if j in filled:
                        idj = ident(j)
                        offs.append([lut.data[j][i] - idj[i] for i in range(3)])
            if offs:
                idk = ident(k)
                new[k] = [idk[i] + sum(o[i] for o in offs) / len(offs) for i in range(3)]
        if not new:
            break
        for k, v in new.items():
            lut.data[k] = v
        filled.update(new)


def write_cube(lut: Lut3D) -> str:
    """Same layout as ``lut_write_cube`` (renderer/src/look/lut3d.cpp)."""
    lines = ["# FUSE look LUT (.cube, Adobe Cube LUT Specification 1.0)", f'TITLE "{lut.title}"',
             f"LUT_3D_SIZE {lut.size}", "DOMAIN_MIN 0 0 0", "DOMAIN_MAX 1 1 1"]
    lines += [f"{v[0]:.9g} {v[1]:.9g} {v[2]:.9g}" for v in lut.data]
    return "\n".join(lines) + "\n"


def parse_cube(text: str) -> Lut3D:
    """Strict reader with the renderer's rules (keywords before data, 3D size 2..256, N³ rows, red fastest)."""
    size, title, data, started = 0, "", [], False
    for n, raw in enumerate(text.split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line[0] in "-+." or line[0].isdigit():
            vals = line.split()
            if len(vals) != 3:
                raise RecipeError(f"cube line {n}: expected three numbers")
            data.append([float(v) for v in vals])
            started = True
            continue
        if started:
            raise RecipeError(f"cube line {n}: keyword after table data")
        key, _, rest = line.partition(" ")
        if key == "TITLE":
            title = rest.strip().strip('"')
        elif key == "LUT_3D_SIZE":
            size = int(rest)
        elif key in ("DOMAIN_MIN", "DOMAIN_MAX"):
            v = [float(x) for x in rest.split()]
            if v != ([0.0] * 3 if key == "DOMAIN_MIN" else [1.0] * 3):
                raise RecipeError("only the unit domain is emitted by fuse_ai")
        else:
            raise RecipeError(f"cube line {n}: unknown keyword {key!r}")
    if len(data) != size ** 3:
        raise RecipeError(f"3D table has {len(data)} entries, expected {size ** 3}")
    return Lut3D(size, data, title)


# ---- presets + .fuselook -------------------------------------------------------------------------------------------


def load_preset(name: str) -> Dict[str, Any]:
    p = presets_dir() / f"{name}.look-preset.json"
    try:
        d = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise RecipeError(f"cannot read look preset {p}: {e}") from None
    if d.get("schema") != "fuse.ai-look-preset/1" or d.get("name") != name:
        raise RecipeError(f"{p} is not a fuse.ai-look-preset/1 file named {name!r}")
    return d


def fuselook(preset: Dict[str, Any], *, lut_path: Optional[str], level: str = "") -> Dict[str, Any]:
    base = json.loads(json.dumps(preset["base"]))
    grade = base.setdefault("colorGrade", {})
    if lut_path:
        grade["lut"] = lut_path
        grade["lutStrength"] = float(preset.get("lut_strength", 1.0))
    name = preset["name"] + (f"_{level}" if level else "")
    return {"schemaVersion": 1, "kind": LOOK_KIND, "name": name,
            "description": preset["description"] + (f" Level: {level}." if level else ""), "base": base}


def emit(preset_name: str, out_dir: str | Path, *, lut: Optional[Lut3D] = None, level: str = "") -> Dict[str, str]:
    preset = load_preset(preset_name)
    if preset.get("needs_lut") and lut is None:
        raise RecipeError(f"look preset {preset_name!r} needs a LUT fitted to captures (fuse_ai tool look-fit)")
    out = Path(out_dir)
    stem = preset_name + (f"_{level}" if level else "")
    written: Dict[str, str] = {}
    lut_rel = None
    if lut is not None:
        lut_rel = f"luts/{stem}.cube"
        (out / "luts").mkdir(parents=True, exist_ok=True)
        (out / lut_rel).write_text(write_cube(lut), encoding="utf-8", newline="\n")
        written["cube"] = str(out / lut_rel)
    look = fuselook(preset, lut_path=lut_rel, level=level)
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{stem}.fuselook").write_text(json.dumps(look, indent=2) + "\n", encoding="utf-8", newline="\n")
    written["fuselook"] = str(out / f"{stem}.fuselook")
    return written


def validate_fuselook(d: Dict[str, Any], base_dir: Optional[Path] = None) -> List[str]:
    """The structural rules of look_schema.hpp that fuse_ai's output relies on."""
    errs = []
    if d.get("kind") != LOOK_KIND or d.get("schemaVersion") != 1:
        errs.append("kind must be fuse.look and schemaVersion 1")
    if not isinstance(d.get("name"), str) or not d["name"]:
        errs.append("name must be a non-empty string")
    base = d.get("base")
    if not isinstance(base, dict):
        errs.append("base must be an object")
        return errs
    known = {"ambientOcclusion", "depthOfField", "motionBlur", "bloom", "lensDirt", "lensFlare", "exposure", "toneMap",
             "colorGrade", "sharpen", "chromaticAberration", "vignette", "filmGrain", "outputTransform"}  # look_params.cpp
    for k in base:
        if k not in known:
            errs.append(f"base.{k} is not a look group")
    tm = base.get("toneMap", {}).get("operator")
    if tm is not None and tm not in ("aces", "filmic", "reinhard", "neutral"):  # kLookToneMapOperatorNames
        errs.append(f"unknown tone map operator {tm!r}")
    lut = base.get("colorGrade", {}).get("lut")
    if lut is not None:
        if not isinstance(lut, str) or not lut.endswith(".cube"):
            errs.append("colorGrade.lut must name a .cube file")
        elif base_dir is not None:
            try:
                parse_cube((base_dir / lut).read_text(encoding="utf-8"))
            except (OSError, RecipeError) as e:
                errs.append(f"colorGrade.lut: {e}")
    return errs


# ---- the W5.4 synthetic grade + captures ------------------------------------------------------------------------


def synthetic_grade(c: Sequence[float]) -> Vec3:
    """A known 'legacy' grade on sRGB-encoded colour: lift/gamma/gain, warm shift, 85 % saturation, S-curve."""
    lift, gamma, gain = (0.03, 0.02, 0.0), (1.10, 1.05, 0.95), (1.04, 1.0, 0.90)
    out = []
    for i in range(3):
        v = _sat(c[i])
        v = lift[i] + (gain[i] - lift[i]) * v
        v = _sat(v) ** gamma[i]
        out.append(v)
    luma = 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2]
    out = [luma + 0.85 * (v - luma) for v in out]
    out = [_sat(v + 0.12 * v * (1 - v) * (2 * v - 1)) for v in out]  # gentle S-curve
    return (out[0], out[1], out[2])


def chart_pairs(steps: int = 12, extra: int = 1500, seed: int = 1, grade=synthetic_grade) -> List[Tuple[Vec3, Vec3]]:
    """Captures: a steps³ colour chart plus random colours, each paired with its graded capture."""
    rng = random.Random(seed)
    pts = [(r / (steps - 1), g / (steps - 1), b / (steps - 1)) for b in range(steps) for g in range(steps)
           for r in range(steps)]
    pts += [(rng.random(), rng.random(), rng.random()) for _ in range(extra)]
    return [(p, grade(p)) for p in pts]


def evaluate(lut: Lut3D, grade=synthetic_grade, n: int = 2000, seed: int = 99) -> Dict[str, float]:
    rng = random.Random(seed)
    des = []
    for _ in range(n):
        c = (rng.random(), rng.random(), rng.random())
        des.append(delta_e76(sample_tetrahedral(lut, c), grade(c)))
    des.sort()
    return {"mean_de76": sum(des) / n, "p95_de76": des[int(n * 0.95)], "max_de76": des[-1], "n": n}


def identity_error(grade=synthetic_grade, n: int = 2000, seed: int = 99) -> float:
    rng = random.Random(seed)
    return sum(delta_e76(c, grade(c)) for c in ((rng.random(), rng.random(), rng.random()) for _ in range(n))) / n
