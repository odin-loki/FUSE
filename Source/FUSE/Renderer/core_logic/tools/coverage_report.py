#!/usr/bin/env python3
"""Line + branch coverage report for Source/FUSE/Renderer/core_logic (WP-0.8).

Usage: coverage_report.py --gcov gcov --objdir <dir with .gcda/.gcno> --root <core_logic dir>
                          [--run <test exe> [args...]] [--min-branch 100]

With --run the stale .gcda files under --objdir are deleted, the executable is run, then every
.gcda is fed to `gcov --json-format --stdout -b`. Branches are merged per
(file, line, branch index) across translation units and template instantiations: a branch
counts as taken if any instantiation took it. Exception ("throw") edges are ignored (the
cores are built with -fno-exceptions). Only files under <root>/include and <root>/src count.
Exits 1 when merged branch coverage is below --min-branch percent.
"""
import argparse
import json
import os
import subprocess
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gcov", default="gcov")
    ap.add_argument("--objdir", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--min-branch", type=float, default=0.0)
    ap.add_argument("--run", nargs=argparse.REMAINDER)
    a = ap.parse_args()

    gcdas = []
    for d, _, files in os.walk(a.objdir):
        for f in files:
            if f.endswith(".gcda"):
                gcdas.append(os.path.join(d, f))
    if a.run:
        for g in gcdas:
            os.remove(g)
        rc = subprocess.call(a.run)
        if rc != 0:
            print(f"coverage: test run failed (exit {rc})", file=sys.stderr)
            return rc
        gcdas = []
        for d, _, files in os.walk(a.objdir):
            for f in files:
                if f.endswith(".gcda"):
                    gcdas.append(os.path.join(d, f))
    if not gcdas:
        print("coverage: no .gcda files found", file=sys.stderr)
        return 1

    roots = [os.path.realpath(os.path.join(a.root, "include")), os.path.realpath(os.path.join(a.root, "src"))]
    lines = {}     # file -> {line: max count}
    branches = {}  # file -> {(line, idx): taken}
    for g in sorted(gcdas):
        out = subprocess.run([a.gcov, "--json-format", "--stdout", "-b", g], cwd=os.path.dirname(g),
                             capture_output=True, text=True)
        if out.returncode != 0:
            print(out.stderr, file=sys.stderr)
            return 1
        for doc in out.stdout.splitlines():
            doc = doc.strip()
            if not doc.startswith("{"):
                continue
            data = json.loads(doc)
            cwd = data.get("current_working_directory", "")
            for f in data["files"]:
                path = os.path.realpath(os.path.join(cwd, f["file"]))
                if not any(path.startswith(r + os.sep) for r in roots):
                    continue
                fl = lines.setdefault(path, {})
                fb = branches.setdefault(path, {})
                for ln in f["lines"]:
                    n = ln["line_number"]
                    fl[n] = max(fl.get(n, 0), ln["count"])
                    idx = 0
                    for br in ln["branches"]:
                        if br.get("throw"):
                            continue
                        key = (n, idx)
                        fb[key] = fb.get(key, False) or br["count"] > 0
                        idx += 1

    tl = tlc = tb = tbc = 0
    print(f"{'file':60s} {'lines':>16s} {'branches':>16s}")
    for path in sorted(lines):
        l_all = len(lines[path])
        l_cov = sum(1 for c in lines[path].values() if c > 0)
        b_all = len(branches[path])
        b_cov = sum(1 for t in branches[path].values() if t)
        tl += l_all
        tlc += l_cov
        tb += b_all
        tbc += b_cov
        rel = os.path.relpath(path, a.root)
        lp = 100.0 * l_cov / l_all if l_all else 100.0
        bp = 100.0 * b_cov / b_all if b_all else 100.0
        print(f"{rel:60s} {l_cov:5d}/{l_all:<5d}{lp:6.2f}% {b_cov:5d}/{b_all:<5d}{bp:6.2f}%")
        missing = sorted({ln for (ln, _), t in branches[path].items() if not t})
        missing_lines = sorted(ln for ln, c in lines[path].items() if c == 0)
        if missing:
            print(f"    untaken branches on lines: {missing[:40]}")
        if missing_lines:
            print(f"    unexecuted lines: {missing_lines[:40]}")
    lp = 100.0 * tlc / tl if tl else 100.0
    bp = 100.0 * tbc / tb if tb else 100.0
    print(f"{'TOTAL':60s} {tlc:5d}/{tl:<5d}{lp:6.2f}% {tbc:5d}/{tb:<5d}{bp:6.2f}%")
    if bp + 1e-9 < a.min_branch:
        print(f"coverage: branch coverage {bp:.2f}% < required {a.min_branch:.2f}%", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
