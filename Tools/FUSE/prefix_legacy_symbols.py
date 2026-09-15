#!/usr/bin/env python3
"""
WP-02 prep — evidence-driven legacy symbol prefix planner.

Reads collision data from docs/unification/symbol-collision-report.md and scans
Engine/source (T3D) and third_party/Torque2D/engine/source (T2D) for top-priority
Con:: API and class symbols. Emits a rename PLAN (dry-run by default); does NOT
rewrite the Engine trees wholesale.

Usage:
  python3 Tools/FUSE/prefix_legacy_symbols.py --list-con
  python3 Tools/FUSE/prefix_legacy_symbols.py --plan --dimension t3d
  python3 Tools/FUSE/prefix_legacy_symbols.py --plan --dimension t2d --output plan.json

U2 will consume the JSON plan to drive fuse_t3d_legacy / fuse_t2d_legacy quarantine.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, asdict
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
T3D_ROOT = REPO_ROOT / "Engine" / "source"
T2D_ROOT = REPO_ROOT / "third_party" / "Torque2D" / "engine" / "source"
COLLISION_DOC = REPO_ROOT / "docs" / "unification" / "symbol-collision-report.md"

CON_PATTERN = re.compile(r"\bCon::([A-Za-z_][A-Za-z0-9_]*)\s*\(")
CLASS_PATTERN = re.compile(r"^\s*class\s+([A-Za-z_][A-Za-z0-9_]*)\b")

PRIORITY_CON_SYMBOLS = [
    "execute",
    "executef",
    "printf",
    "errorf",
    "warnf",
    "getVariable",
    "setVariable",
    "getIntVariable",
    "setIntVariable",
    "getBoolVariable",
    "setBoolVariable",
    "addVariable",
    "init",
    "expandPath",
    "collapsePath",
    "getData",
    "setData",
    "isFunction",
    "threadSafeExecute",
]

PRIORITY_CLASSES = [
    "SimObject",
    "SimGroup",
    "SimSet",
    "ConsoleObject",
    "CodeBlock",
    "BitStream",
    "GameConnection",
    "GuiCanvas",
    "GuiControl",
]


@dataclass
class SymbolHit:
    symbol: str
    kind: str
    path: str
    line: int
    proposed_name: str


def scan_con_symbols(root: Path, prefix: str) -> list[SymbolHit]:
    hits: list[SymbolHit] = []
    if not root.exists():
        return hits

    for path in sorted(root.rglob("*")):
        if path.suffix not in {".cpp", ".h", ".cc", ".c"}:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for line_no, line in enumerate(text.splitlines(), start=1):
            for match in CON_PATTERN.finditer(line):
                name = match.group(1)
                if name in PRIORITY_CON_SYMBOLS:
                    hits.append(
                        SymbolHit(
                            symbol=name,
                            kind="Con::function",
                            path=str(path.relative_to(REPO_ROOT)),
                            line=line_no,
                            proposed_name=f"{prefix}Con_{name}",
                        )
                    )
    return hits


def scan_classes(root: Path, prefix: str) -> list[SymbolHit]:
    hits: list[SymbolHit] = []
    if not root.exists():
        return hits

    for path in sorted(root.rglob("*")):
        if path.suffix not in {".h", ".hpp"}:
            continue
        try:
            lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
        except OSError:
            continue
        for line_no, line in enumerate(lines, start=1):
            match = CLASS_PATTERN.match(line)
            if not match:
                continue
            name = match.group(1)
            if name in PRIORITY_CLASSES:
                hits.append(
                    SymbolHit(
                        symbol=name,
                        kind="class",
                        path=str(path.relative_to(REPO_ROOT)),
                        line=line_no,
                        proposed_name=f"{prefix}{name}",
                    )
                )
    return hits


def load_doc_con_list() -> list[str]:
    if not COLLISION_DOC.exists():
        return PRIORITY_CON_SYMBOLS
    text = COLLISION_DOC.read_text(encoding="utf-8", errors="ignore")
    match = re.search(r"`Con::` API overlap \(33 functions\):\*\* (.+?)\.", text)
    if not match:
        return PRIORITY_CON_SYMBOLS
    raw = match.group(1)
    return [token.strip().strip("`") for token in raw.split(",")]


def build_plan(dimension: str) -> dict:
    if dimension == "t3d":
        root = T3D_ROOT
        prefix = "fuse_t3d_"
    elif dimension == "t2d":
        root = T2D_ROOT
        prefix = "fuse_t2d_"
    else:
        raise ValueError(f"unknown dimension: {dimension}")

    con_hits = scan_con_symbols(root, prefix)
    class_hits = scan_classes(root, prefix)

    return {
        "dimension": dimension,
        "prefix": prefix,
        "source_root": str(root.relative_to(REPO_ROOT)) if root.exists() else str(root),
        "doc_con_symbols": load_doc_con_list(),
        "hits": [asdict(hit) for hit in con_hits + class_hits],
        "summary": {
            "con_hits": len(con_hits),
            "class_hits": len(class_hits),
            "unique_symbols": sorted({hit.symbol for hit in con_hits + class_hits}),
        },
        "notes": [
            "Dry-run planner only — U2 applies renames inside fuse_t3d_legacy / fuse_t2d_legacy static libs.",
            "See docs/unification/symbol-collision-report.md for full collision metrics.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="FUSE legacy symbol prefix planner (WP-02 prep)")
    parser.add_argument("--list-con", action="store_true", help="Print Con:: symbols from collision report")
    parser.add_argument("--plan", action="store_true", help="Scan sources and emit rename plan")
    parser.add_argument("--dimension", choices=["t3d", "t2d"], default="t3d")
    parser.add_argument("--output", type=Path, help="Write JSON plan to this path")
    args = parser.parse_args()

    if args.list_con:
        for symbol in load_doc_con_list():
            print(symbol)
        return 0

    if args.plan:
        plan = build_plan(args.dimension)
        payload = json.dumps(plan, indent=2)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(payload + "\n", encoding="utf-8")
            print(f"Wrote plan to {args.output}")
        else:
            print(payload)
        return 0

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
