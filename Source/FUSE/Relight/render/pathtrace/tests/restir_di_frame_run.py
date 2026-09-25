#!/usr/bin/env python3
"""FUSE Relight RL-5.2: relight.frame.mode = pathtrace with rtx.useRTXDI = True under Wine.

RL-5.1's rl_pt_frame_run.py (the same `frame` / `determinism` checks: every frame path traced, 64-spp frames within
4 sigma of the in-process CPU reference in 8x8 blocks, convergence 4 -> 64 spp, bit-identical reruns) with ReSTIR DI
enabled through the run's rtx.conf; additionally every frame record must report "restir_di": true (the GPU passes
initialised and ran).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rl_pt_frame_run as base  # noqa: E402

_load = base.load


def _load_with_restir(path, name):
    mod = _load(path, name)
    if name == "rl_raster_run" and not getattr(mod, "_restir_di", False):
        orig = mod.rtx_conf

        def rtx_conf(out_dir, app=""):
            wine_path = orig(out_dir, app)
            with open(wine_path[2:].replace("\\", "/"), "a") as f:
                f.write("rtx.useRTXDI = True\n")
            return wine_path

        mod.rtx_conf = rtx_conf
        orig_records = mod.records

        def records(run_dir):
            recs = orig_records(run_dir)
            for r in recs or []:
                if r.get("ev") == "frame" and r.get("raster", {}).get("restir_di") is not True:
                    r["pass"] = "restir_di_off"  # fails check_frames: ReSTIR DI did not run
            return recs

        mod.records = records
        mod._restir_di = True
    return mod


base.load = _load_with_restir

if __name__ == "__main__":
    sys.exit(base.main())
