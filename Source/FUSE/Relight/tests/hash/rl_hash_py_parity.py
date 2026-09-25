#!/usr/bin/env python3
"""FUSE Relight RL-0.5 ctest rl_hash_py_parity: C++ fuse_relight_hash vs the independent Python
reference (Tools/FUSE/Relight/remix_hash_ref.py).

1. The Python reference checks the official xxHash vectors (its own pure-Python xxHash, and the
   `xxhash` wheel too when it is installed).
2. It recomputes every line of the upstream-derived KAT table.
3. fuse_relight_hash_kat --emit writes <count> random cases per function with the library's
   outputs; the reference recomputes all of them. The case file is deleted on success.
"""

import argparse
import importlib.util
import os
import subprocess
import sys


def load_ref(path):
    spec = importlib.util.spec_from_file_location("remix_hash_ref", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def report(ref, label, path):
    with open(path, encoding="utf-8") as fh:
        counts, failures = ref.verify_lines(fh)
    for f in [f for f in failures if f][:20]:
        print("FAIL:", f)
    print("%s: %d functions, %d cases, %d failure(s)" % (label, len(counts), sum(counts.values()), len(failures)))
    return not failures and counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--kat", required=True)
    ap.add_argument("--vectors", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--count", default="10000")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--runner", nargs=argparse.REMAINDER, default=[],
                    help="emulator command for a cross-compiled --exe (CMAKE_CROSSCOMPILING_EMULATOR)")
    args = ap.parse_args()
    ref = load_ref(args.ref)
    ok = True

    impls = ["pure"] + (["wheel"] if ref._xxhash_wheel is not None else [])
    for impl in impls:
        ref.set_impl(impl)
        count, failures = ref.verify_vectors(args.vectors)
        for f in failures[:10]:
            print("FAIL:", f)
        print("xxHash official vectors (%s): %d checked, %d failure(s)" % (impl, count, len(failures)))
        ok &= not failures and count > 0
    ref.set_impl("auto")
    print("reference xxHash for the case files:", "xxhash wheel" if ref.using_wheel() else "pure Python")

    ok &= bool(report(ref, "KAT table", args.kat))

    os.makedirs(args.work, exist_ok=True)
    cases = os.path.join(args.work, "rl_hash_cases.txt")
    cmd = list(args.runner) + [args.exe, "--emit", cases, "--count", args.count]
    rc = subprocess.call(cmd)
    if rc == 77:
        print("SKIP: the C++ driver could not run (emulator unavailable)")
        return 77
    if rc != 0:
        print("FAIL: %s exited with %d" % (" ".join(cmd), rc))
        return 1
    emitted = report(ref, "C++ vs Python, %s random cases per function" % args.count, cases)
    ok &= bool(emitted)
    if emitted:
        os.remove(cases)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
