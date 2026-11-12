"""CLI: ``python -m fuse_ai`` (plan §3.1, §3.3).

    python -m fuse_ai run <recipe.json>... [--mode replay|record|mock] [--project remaster.project.json]
                          [--cache build/ai-cache] [--budget ai-budget.json] [--ledger <cache>/ai-ledger.json]
                          [--out DIR] [--write-recipe] [--force]
    python -m fuse_ai providers [--json]
    python -m fuse_ai hash <recipe.json>...
    python -m fuse_ai validate <recipe.json>...
    python -m fuse_ai cost [<recipe.json>...] [--budget ai-budget.json] [--ledger …] [--cache …] [--project …]
                           [--month YYYY-MM] [--json]
    python -m fuse_ai tool <name> …            CPU tools (W3.x / W5.x), see ``python -m fuse_ai tool --help``

``run`` prints one JSON summary line per recipe on stdout. Errors go to stderr as
``fuse_ai: error: <Kind>: <message>`` with a nonzero exit code: 2 invalid recipe/project/budget,
3 replay miss, 4 budget refused, 5 cloud opt-in guard (CloudOptInRequired / PaidTierRequired),
6 provider not implemented or not configured, 7 VRAM budget, 8 corrupt cache, 9 schema violation (W4.2),
10 transport error, 11 quality gate failed, 1 anything else.

``run`` resolves, routes and guards *every* recipe, then checks the budget over all cache misses together,
before the first provider call (W4.5). ``cost`` prints the spend report and, for recipes, the plan: which are
cached and what the rest would cost; it exits 4 when the plan does not fit the budget.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import List, Optional

from . import __version__, providers
from .budget import Budget, BudgetConfig
from .cache import DEFAULT_CACHE_DIR, Mode
from .errors import FuseAIError
from .policy import ProjectPolicy
from .recipe import dump, load
from .runner import Runner


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="python -m fuse_ai", description="FUSE Remaster AI services (plan §3).")
    ap.add_argument("--version", action="version", version=f"fuse_ai {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    run = sub.add_parser("run", help="run recipes (replay is the CI default)")
    run.add_argument("recipes", nargs="+", type=Path)
    run.add_argument("--mode", choices=[m.value for m in Mode], default=Mode.REPLAY.value)
    run.add_argument("--project", type=Path, help="per-game remaster.project.json (default: cloud_upload none)")
    run.add_argument("--cache", type=Path, default=Path(os.environ.get("FUSE_AI_CACHE", DEFAULT_CACHE_DIR)),
                     help="cache dir (default $FUSE_AI_CACHE or build/ai-cache)")
    run.add_argument("--budget", type=Path, help="ai-budget.json (default: the §7.4 defaults)")
    run.add_argument("--ledger", type=Path, help="spend ledger (default <cache>/ai-ledger.json)")
    run.add_argument("--out", type=Path, help="write output files to DIR/<recipe id>/")
    run.add_argument("--write-recipe", action="store_true", help="write output hashes + cost back into the recipe")
    run.add_argument("--force", action="store_true", help="record: call the provider even on a cache hit")

    prov = sub.add_parser("providers", help="list the §3.1 providers")
    prov.add_argument("--json", action="store_true")

    h = sub.add_parser("hash", help="print canonical recipe hashes")
    h.add_argument("recipes", nargs="+", type=Path)

    v = sub.add_parser("validate", help="validate recipes against fuse.ai-recipe/1")
    v.add_argument("recipes", nargs="+", type=Path)

    c = sub.add_parser("cost", help="spend report (ledger) and, for recipes, the plan vs the budget (W4.5)")
    c.add_argument("recipes", nargs="*", type=Path)
    c.add_argument("--project", type=Path)
    c.add_argument("--cache", type=Path, default=Path(os.environ.get("FUSE_AI_CACHE", DEFAULT_CACHE_DIR)))
    c.add_argument("--budget", type=Path)
    c.add_argument("--ledger", type=Path)
    c.add_argument("--month", help="YYYY-MM (default: the current month)")
    c.add_argument("--json", action="store_true")

    t = sub.add_parser("tool", help="CPU tools: classify, match, fingerprint, rebuild, collision, light-calibrate, "
                                    "look-fit, emissive-lights, port")
    t.add_argument("tool_args", nargs=argparse.REMAINDER)
    return ap


def _safe_dir(recipe_id: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", recipe_id).strip("._") or "recipe"


def _cmd_providers(as_json: bool) -> int:
    rows = []
    for pid in providers.registered_ids():
        info = providers.info(pid)
        rows.append({"id": pid, "cloud": info.cloud, "human": info.human, "implemented": info.implemented,
                     "where": info.where, "tasks": sorted(t.value for t in info.tasks),
                     "default_for": sorted(t.value for t in info.default_for), "notes": info.notes})
    if as_json:
        print(json.dumps(rows, indent=2, sort_keys=True))
        return 0
    for r in rows:
        flags = ",".join(f for f, on in (("cloud", r["cloud"]), ("human", r["human"]),
                                          ("implemented", r["implemented"]), ("stub", not r["implemented"])) if on)
        print(f"{r['id']:<14} [{flags}] {r['where']}")
        print(f"{'':<14} tasks: {', '.join(r['tasks']) or '-'}")
        if r["default_for"]:
            print(f"{'':<14} default for: {', '.join(r['default_for'])}")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.cmd == "providers":
            return _cmd_providers(args.json)
        if args.cmd in ("hash", "validate"):
            for p in args.recipes:
                r = load(p)
                print(f"{r.canonical_hash()}  {p}" if args.cmd == "hash" else f"ok  {p}")
            return 0
        if args.cmd == "tool":
            from .tools import main as tool_main

            return tool_main(args.tool_args)
        project = ProjectPolicy.load(args.project)
        budget = Budget(BudgetConfig.load(args.budget), ledger_path=args.ledger or (args.cache / "ai-ledger.json"))
        if args.cmd == "cost":
            from . import costreport

            runner = Runner(Mode.RECORD, cache_dir=args.cache, project=project, budget=budget)
            items = [(load(p), p.parent) for p in args.recipes]
            rep = costreport.report(ledger=budget.ledger_path, budget_path=args.budget, month=args.month,
                                    runner=runner, items=items)
            if args.json:
                print(json.dumps(rep, indent=2, sort_keys=True))
            else:
                sys.stdout.write(costreport.format_text(rep))
            return 4 if rep.get("plan") and not rep["plan"]["fits"] else 0
        runner = Runner(args.mode, cache_dir=args.cache, project=project, budget=budget, force=args.force)
        loaded = [(load(p), p) for p in args.recipes]
        outcomes = runner.run_many([(r, p.parent) for r, p in loaded])
        for (recipe, path), outcome in zip(loaded, outcomes):
            summary = dict(outcome.summary(), recipe=str(path))
            if args.out:
                d = args.out / _safe_dir(recipe.id)
                d.mkdir(parents=True, exist_ok=True)
                for o in outcome.result.outputs:
                    (d / o.name).write_bytes(o.data)
                summary["out_dir"] = str(d)
            if args.write_recipe:
                dump(outcome.recipe, path)
            print(json.dumps(summary, sort_keys=True))
        return 0
    except FuseAIError as e:
        print(f"fuse_ai: error: {type(e).__name__}: {e}", file=sys.stderr)
        return e.exit_code


if __name__ == "__main__":
    sys.exit(main())
