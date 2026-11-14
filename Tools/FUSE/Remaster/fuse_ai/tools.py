"""``python -m fuse_ai tool <name> ...``: the CPU tools of Waves 3-5 (no provider, no network).

    classify <image.png>...                                 W3.1 detector scores + decisions (JSON)
    classify-eval [--seed N] [--per-class N]                W3.1 accuracy on the labelled synthetic set
    match-eval [--seed N] [--queries N]                     W3.4 top-3 rate on the synthetic library benchmark
    fingerprint <image.png>                                 W3.5 fuse.ai-fingerprint/1 of an output
    rebuild-check <recipe.json> <rebuilt.png> [--reference author.png]
                                                            W3.5 accept a rebuild (exact / tolerance / fingerprint)
    collision <render.obj> <hull.obj> --tolerance M         W5.2 Hausdorff gate (exit 11 on failure)
    light-calibrate <scene.json>                            W5.3 {"contrib": [{type: lum}], "capture": [v], "curve"}
    look-fit <pairs.json> --preset classic --out DIR [--size 17] [--level NAME]
                                                            W5.4 fit a LUT to [[src rgb], [dst rgb]] pairs, emit .fuselook
    look-emit <preset> --out DIR                            W5.4 emit a preset that needs no LUT
    emissive-lights <mesh.obj> <materials.json> [--source-id ID]
                                                            W5.6 POCO light + entity records (JSON list)
    port-check <port.lua> <spec.json> [--original src] [--trace t.json --expected e.log]
                                                            W4.3 structural checks (+ the parity harness when built)

Exit codes follow the CLI: 2 bad input, 11 gate failure.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import List

from .errors import FuseAIError, GateFailure


def _png(path: Path):
    from .fimage import FImage
    from .imaging import decode_png

    return FImage.from_image(decode_png(path.read_bytes()))


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(prog="python -m fuse_ai tool")
    sub = ap.add_subparsers(dest="tool", required=True)
    c = sub.add_parser("classify"); c.add_argument("images", nargs="+", type=Path)
    ce = sub.add_parser("classify-eval"); ce.add_argument("--seed", type=int, default=20261105)
    ce.add_argument("--per-class", type=int, default=20)
    me = sub.add_parser("match-eval"); me.add_argument("--seed", type=int, default=20261105)
    me.add_argument("--queries", type=int, default=40)
    fp = sub.add_parser("fingerprint"); fp.add_argument("image", type=Path)
    rb = sub.add_parser("rebuild-check"); rb.add_argument("recipe", type=Path); rb.add_argument("rebuilt", type=Path)
    rb.add_argument("--reference", type=Path)
    co = sub.add_parser("collision"); co.add_argument("render", type=Path); co.add_argument("hull", type=Path)
    co.add_argument("--tolerance", type=float, required=True)
    lc = sub.add_parser("light-calibrate"); lc.add_argument("scene", type=Path)
    lf = sub.add_parser("look-fit"); lf.add_argument("pairs", type=Path)
    lf.add_argument("--preset", default="classic"); lf.add_argument("--out", type=Path, required=True)
    lf.add_argument("--size", type=int, default=17); lf.add_argument("--level", default="")
    le = sub.add_parser("look-emit"); le.add_argument("preset"); le.add_argument("--out", type=Path, required=True)
    em = sub.add_parser("emissive-lights"); em.add_argument("mesh", type=Path); em.add_argument("materials", type=Path)
    em.add_argument("--source-id", default="mesh")
    pc = sub.add_parser("port-check"); pc.add_argument("lua", type=Path); pc.add_argument("spec", type=Path)
    pc.add_argument("--original", type=Path); pc.add_argument("--trace", type=Path); pc.add_argument("--expected", type=Path)
    args = ap.parse_args(argv)
    try:
        out = _run(args)
    except FuseAIError as e:
        print(f"fuse_ai: error: {type(e).__name__}: {e}", file=sys.stderr)
        return e.exit_code
    print(json.dumps(out, indent=2, sort_keys=True, default=str))
    return 0


def _run(args) -> object:
    t = args.tool
    if t == "classify":
        from .classify import classify

        return {str(p): classify(_png(p)) for p in args.images}
    if t == "classify-eval":
        from .classify import evaluate

        return evaluate(args.seed, args.per_class)
    if t == "match-eval":
        from .matcher import synthetic_benchmark

        r = synthetic_benchmark(args.seed, args.queries)
        r.pop("rows")
        return r
    if t == "fingerprint":
        from .fingerprint import fingerprint

        return fingerprint(_png(args.image))
    if t == "rebuild-check":
        from .fingerprint import verify_rebuild
        from .recipe import load

        r = load(args.recipe)
        ref = args.reference.read_bytes() if args.reference else None
        return verify_rebuild(r.output or {}, args.rebuilt.read_bytes(), reference=ref)
    if t == "collision":
        from .collision import gate

        return gate(args.render, args.hull, tolerance=args.tolerance)
    if t == "light-calibrate":
        from .lightcal import calibrate

        d = json.loads(args.scene.read_text(encoding="utf-8"))
        return calibrate(d["contrib"], d["capture"], curve=d.get("curve", "srgb"))
    if t == "look-fit":
        from .look import emit, fit_lut

        pairs = json.loads(args.pairs.read_text(encoding="utf-8"))
        lut = fit_lut([(p[0], p[1]) for p in pairs], size=args.size, title=f"{args.preset} {args.level}".strip())
        return emit(args.preset, args.out, lut=lut, level=args.level)
    if t == "look-emit":
        from .look import emit

        return emit(args.preset, args.out)
    if t == "emissive-lights":
        from .emissive import load_obj_groups, proxy_lights

        verts, groups = load_obj_groups(args.mesh)
        mats = json.loads(args.materials.read_text(encoding="utf-8"))
        return proxy_lights(verts, groups, mats.get("materials", mats), source_id=args.source_id)
    if t == "port-check":
        from .portflow import run_parity, structural_checks

        spec_text = args.spec.read_text(encoding="utf-8")
        problems = structural_checks(args.lua.read_text(encoding="utf-8"), json.loads(spec_text),
                                     original_source=args.original.read_text(encoding="utf-8") if args.original else None,
                                     spec_text=spec_text)
        res = {"structural_problems": problems}
        if args.trace and args.expected:
            par = run_parity(args.lua, args.trace, args.expected)
            res["parity"] = "harness not built" if par is None else "match"
        if problems:
            raise GateFailure("port check: " + "; ".join(problems))
        return res
    raise FuseAIError(f"unknown tool {t}")
