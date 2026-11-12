"""``fuse_ai cost``: spend report and plan check (plan §3.3 "Budgets", §7.4; W4.5).

The report has two halves:

* **spent** - from the ledger (``ai-ledger.json``, written by :meth:`Budget.record`): per month and
  provider, USD and tokens for cloud providers, GPU-hours, kWh and the electricity cost for local ones
  (§7.4: a 3090 at ≈ 0.45 kW, AUD 0.35/kWh **[VERIFY the user's tariff]**; override with
  ``"electricity": {"aud_per_kwh": …}`` in ``ai-budget.json``), next to each cap and what is left.
* **plan** - for recipes given on the command line: which are already cached (free), the estimate of each
  call still to make, the totals, and whether they fit the budget. ``fits: false`` exits 4 (the same
  :class:`BudgetExceeded` the runner raises before its first call).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from . import providers
from .budget import Budget, BudgetConfig
from .errors import BudgetExceeded, RecipeError
from .recipe import Recipe
from .runner import Runner

DEFAULT_AUD_PER_KWH = 0.35


def aud_per_kwh(budget_path: Optional[Path]) -> float:
    if not budget_path:
        return DEFAULT_AUD_PER_KWH
    try:
        d = json.loads(Path(budget_path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise RecipeError(f"cannot read budget file {budget_path}: {e}") from None
    v = (d.get("electricity") or {}).get("aud_per_kwh", DEFAULT_AUD_PER_KWH)
    if isinstance(v, bool) or not isinstance(v, (int, float)) or v < 0:
        raise RecipeError("electricity.aud_per_kwh must be a non-negative number")
    return float(v)


def spent(budget: Budget, tariff: float, month: Optional[str] = None) -> Dict[str, Any]:
    month = month or budget._month()
    rows = budget.ledger.get("months", {}).get(month, {})
    out: Dict[str, Any] = {"month": month, "providers": {}, "totals": {}}
    tot = {"usd": 0.0, "gpu_hours": 0.0, "kwh": 0.0, "aud_electricity": 0.0, "requests": 0}
    for pid in sorted(rows):
        r = rows[pid]
        info = providers.info(pid) if pid in providers.registered_ids() or pid in providers.PROVIDER_TABLE else None
        row = {"usd": round(r.get("usd", 0.0), 6), "tokens_in": r.get("tokens_in", 0),
               "tokens_out": r.get("tokens_out", 0), "gpu_hours": round(r.get("gpu_seconds", 0.0) / 3600.0, 6),
               "kwh": round(r.get("kwh", 0.0), 6), "aud_electricity": round(r.get("kwh", 0.0) * tariff, 4),
               "requests": r.get("requests", 0)}
        if info is not None:
            lim = budget.config.limits_for(info)
            if lim.monthly_usd is not None:
                row["monthly_cap_usd"] = lim.monthly_usd
                row["monthly_left_usd"] = round(lim.monthly_usd - row["usd"], 6)
        out["providers"][pid] = row
        for k in tot:
            tot[k] += row[k]
    out["totals"] = {k: (round(v, 6) if isinstance(v, float) else v) for k, v in tot.items()}
    return out


def plan(runner: Runner, items: Sequence[Tuple[Recipe, Optional[Path]]], tariff: float) -> Dict[str, Any]:
    prepared = runner.plan(items)  # resolve, route, guard and cache lookup; no provider call
    rows: List[Dict[str, Any]] = []
    pending = []
    for p in prepared:
        if p.result is not None:
            rows.append({"id": p.recipe.id, "provider": p.effective, "cached": True, "usd": 0.0, "gpu_hours": 0.0})
            continue
        est = p.provider.estimate(p.req)
        pending.append((p.info, est))
        rows.append({"id": p.recipe.id, "provider": p.effective, "cached": False, "usd": round(est.usd, 6),
                     "gpu_hours": round(est.gpu_hours, 6), "kwh": round(est.kwh, 6),
                     "tokens_in": est.tokens_in, "tokens_out": est.tokens_out,
                     "batch": bool(p.req.params.get("batch"))})
    result: Dict[str, Any] = {"recipes": rows,
                              "total_usd": round(sum(r["usd"] for r in rows), 6),
                              "total_gpu_hours": round(sum(r["gpu_hours"] for r in rows), 6),
                              "total_aud_electricity": round(sum(r.get("kwh", 0.0) for r in rows) * tariff, 4)}
    try:
        runner.budget.check_plan(pending)
        result["fits"] = True
        result["problems"] = []
    except BudgetExceeded as e:
        result["fits"] = False
        result["problems"] = [str(e)]
    return result


def report(*, ledger: Optional[Path], budget_path: Optional[Path], month: Optional[str] = None,
           runner: Optional[Runner] = None, items: Sequence[Tuple[Recipe, Optional[Path]]] = ()) -> Dict[str, Any]:
    tariff = aud_per_kwh(budget_path)
    budget = runner.budget if runner else Budget(BudgetConfig.load(budget_path), ledger_path=ledger)
    out: Dict[str, Any] = {"schema": "fuse.ai-cost-report/1", "aud_per_kwh": tariff,
                           "caps": {"per_run_usd": budget.config.per_run_usd,
                                    "cloud_monthly_usd": budget.config.cloud_monthly_usd,
                                    "local_gpu_hours_per_run": budget.config.local_gpu_hours_per_run},
                           "spent": spent(budget, tariff, month)}
    if items:
        if runner is None:
            raise RecipeError("a plan needs a runner")
        out["plan"] = plan(runner, items, tariff)
    return out


def format_text(rep: Dict[str, Any]) -> str:
    lines = [f"fuse_ai cost report ({rep['spent']['month']}); electricity AUD {rep['aud_per_kwh']:.2f}/kWh"]
    lines.append(f"caps: US${rep['caps']['per_run_usd']:.2f} per run, US${rep['caps']['cloud_monthly_usd']:.2f} per "
                 f"month per cloud provider, {rep['caps']['local_gpu_hours_per_run']:g} GPU-hours per run (local)")
    lines.append("spent:")
    if not rep["spent"]["providers"]:
        lines.append("  (nothing this month)")
    for pid, r in rep["spent"]["providers"].items():
        left = f", US${r['monthly_left_usd']:.2f} left" if "monthly_left_usd" in r else ""
        lines.append(f"  {pid:<12} US${r['usd']:.4f}{left}; {r['tokens_in']} in / {r['tokens_out']} out tokens; "
                     f"{r['gpu_hours']:.3f} GPU-h, {r['kwh']:.4f} kWh (AUD {r['aud_electricity']:.2f}); "
                     f"{r['requests']} requests")
    t = rep["spent"]["totals"]
    lines.append(f"  total        US${t['usd']:.4f}; {t['gpu_hours']:.3f} GPU-h; AUD {t['aud_electricity']:.2f} "
                 "electricity")
    if "plan" in rep:
        p = rep["plan"]
        lines.append("plan:")
        for r in p["recipes"]:
            what = "cached" if r["cached"] else f"US${r['usd']:.4f}, {r['gpu_hours']:.3f} GPU-h"
            lines.append(f"  {r['id']:<40} {r['provider']:<10} {what}")
        lines.append(f"  total US${p['total_usd']:.4f}, {p['total_gpu_hours']:.3f} GPU-h, "
                     f"AUD {p['total_aud_electricity']:.2f}: {'fits' if p['fits'] else 'REFUSED'}")
        for prob in p["problems"]:
            lines.append(f"  {prob}")
    return "\n".join(lines) + "\n"
