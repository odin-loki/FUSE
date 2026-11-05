"""Spend budgets: ``ai-budget.json`` caps and the spend ledger (plan §3.3 "Budgets", §7.4 defaults).

``ai-budget.json`` (``Content/remaster/<game>/ai-budget.json``)::

    {"schema": "fuse.ai-budget/1",
     "defaults":  {"per_run_usd": 50, "cloud_monthly_usd": 300, "local_gpu_hours_per_run": 24},
     "providers": {"gemini": {"monthly_usd": 300, "per_run_usd": 50, "rpm": 60},
                   "local-torch": {"gpu_hours_per_run": 12}}}

Every key is optional; the defaults are the §7.4 ones (US$50 per run and US$300 per month per game for
cloud providers, 24 GPU-hours per run for local providers). ``rpm`` (requests per minute) has no default
except Gemini's 60 **[SPECULATIVE: set to the key's real quota]**.

:meth:`Budget.check` refuses (``BudgetExceeded``) when an estimate exceeds what remains of the per-run
USD cap, the provider's monthly USD cap, its per-run GPU-hours cap, or its request rate.
:meth:`Budget.record` adds the actual spend to the in-memory run totals and to the ledger JSON
(``ai-ledger.json``: per month per provider USD / tokens / GPU seconds / kWh / requests, plus the recent
request times for the rate limit). The runner also writes the actual cost into the recipe ``cost`` block.
"""

from __future__ import annotations

import datetime as _dt
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

from . import BUDGET_SCHEMA
from .cache import _atomic_write
from .errors import BudgetExceeded, RecipeError
from .providers import Cost, ProviderInfo

LEDGER_SCHEMA = "fuse.ai-ledger/1"
DEFAULT_PER_RUN_USD = 50.0
DEFAULT_CLOUD_MONTHLY_USD = 300.0
DEFAULT_LOCAL_GPU_HOURS_PER_RUN = 24.0
DEFAULT_RPM = {"gemini": 60}

Clock = Callable[[], _dt.datetime]


def utc_now() -> _dt.datetime:
    return _dt.datetime.now(_dt.timezone.utc)


def iso(t: _dt.datetime) -> str:
    return t.astimezone(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _parse_iso(s: str) -> _dt.datetime:
    return _dt.datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=_dt.timezone.utc)


@dataclass
class ProviderLimits:
    monthly_usd: Optional[float] = None
    per_run_usd: Optional[float] = None
    rpm: Optional[int] = None
    gpu_hours_per_run: Optional[float] = None


@dataclass
class BudgetConfig:
    per_run_usd: float = DEFAULT_PER_RUN_USD
    cloud_monthly_usd: float = DEFAULT_CLOUD_MONTHLY_USD
    local_gpu_hours_per_run: float = DEFAULT_LOCAL_GPU_HOURS_PER_RUN
    providers: Dict[str, ProviderLimits] = field(default_factory=dict)

    @classmethod
    def from_json(cls, d: Any) -> "BudgetConfig":
        if not isinstance(d, dict):
            raise RecipeError("ai-budget.json must be a JSON object")
        if d.get("schema", BUDGET_SCHEMA) != BUDGET_SCHEMA:
            raise RecipeError(f"ai-budget.json schema must be {BUDGET_SCHEMA!r}")
        dflt = d.get("defaults", {}) or {}
        cfg = cls(per_run_usd=_num(dflt, "per_run_usd", DEFAULT_PER_RUN_USD),
                  cloud_monthly_usd=_num(dflt, "cloud_monthly_usd", DEFAULT_CLOUD_MONTHLY_USD),
                  local_gpu_hours_per_run=_num(dflt, "local_gpu_hours_per_run", DEFAULT_LOCAL_GPU_HOURS_PER_RUN))
        for pid, lim in (d.get("providers", {}) or {}).items():
            if not isinstance(lim, dict):
                raise RecipeError(f"ai-budget.json providers.{pid} must be an object")
            unknown = set(lim) - {"monthly_usd", "per_run_usd", "rpm", "gpu_hours_per_run"}
            if unknown:
                raise RecipeError(f"ai-budget.json providers.{pid}: unknown keys {sorted(unknown)}")
            cfg.providers[pid] = ProviderLimits(
                monthly_usd=_num(lim, "monthly_usd", None), per_run_usd=_num(lim, "per_run_usd", None),
                rpm=int(lim["rpm"]) if lim.get("rpm") is not None else None,
                gpu_hours_per_run=_num(lim, "gpu_hours_per_run", None))
        return cfg

    @classmethod
    def load(cls, path: Optional[str | Path]) -> "BudgetConfig":
        if path is None:
            return cls()
        try:
            return cls.from_json(json.loads(Path(path).read_text(encoding="utf-8")))
        except (OSError, json.JSONDecodeError) as e:
            raise RecipeError(f"cannot read budget file {path}: {e}") from None

    def limits_for(self, info: ProviderInfo) -> ProviderLimits:
        lim = self.providers.get(info.id, ProviderLimits())
        out = ProviderLimits(**vars(lim))
        if info.cloud and out.monthly_usd is None:
            out.monthly_usd = self.cloud_monthly_usd
        if out.rpm is None:
            out.rpm = DEFAULT_RPM.get(info.id)
        if not info.cloud and out.gpu_hours_per_run is None:
            out.gpu_hours_per_run = self.local_gpu_hours_per_run
        return out


def _num(d: Dict[str, Any], key: str, default: Optional[float]) -> Optional[float]:
    v = d.get(key, default)
    if v is None:
        return None
    if isinstance(v, bool) or not isinstance(v, (int, float)) or v < 0:
        raise RecipeError(f"budget value {key!r} must be a non-negative number")
    return float(v)


class Budget:
    """Per-run state (in memory) plus the persistent ledger. One instance == one run."""

    def __init__(self, config: Optional[BudgetConfig] = None, ledger_path: Optional[str | Path] = None,
                 clock: Clock = utc_now):
        self.config = config or BudgetConfig()
        self.ledger_path = Path(ledger_path) if ledger_path else None
        self.clock = clock
        self.run_usd_total = 0.0
        self.run_usd: Dict[str, float] = {}
        self.run_gpu_seconds: Dict[str, float] = {}
        self.ledger = self._load_ledger()

    # ---- ledger -------------------------------------------------------------------------------------------------

    def _load_ledger(self) -> Dict[str, Any]:
        empty = {"schema": LEDGER_SCHEMA, "months": {}, "recent": {}}
        if not self.ledger_path or not self.ledger_path.is_file():
            return empty
        try:
            d = json.loads(self.ledger_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            raise RecipeError(f"cannot read ledger {self.ledger_path}: {e}") from None
        if d.get("schema") != LEDGER_SCHEMA:
            raise RecipeError(f"ledger {self.ledger_path} has the wrong schema")
        d.setdefault("months", {})
        d.setdefault("recent", {})
        return d

    def _month(self) -> str:
        return self.clock().astimezone(_dt.timezone.utc).strftime("%Y-%m")

    def month_spent_usd(self, provider_id: str) -> float:
        return float(self.ledger["months"].get(self._month(), {}).get(provider_id, {}).get("usd", 0.0))

    def _recent(self, provider_id: str) -> List[str]:
        now = self.clock()
        cutoff = now - _dt.timedelta(seconds=60)
        return [t for t in self.ledger["recent"].get(provider_id, []) if _parse_iso(t) > cutoff]

    # ---- check / record -----------------------------------------------------------------------------------------

    def check(self, info: ProviderInfo, est: Cost) -> None:
        lim = self.config.limits_for(info)
        pid = info.id
        problems = []
        if est.usd > 0 or self.run_usd_total > 0:
            remaining = self.config.per_run_usd - self.run_usd_total
            if est.usd > remaining + 1e-9:
                problems.append(f"estimate US${est.usd:.4f} exceeds the per-run cap (US${remaining:.4f} of "
                                f"US${self.config.per_run_usd:.2f} left)")
        if lim.per_run_usd is not None:
            remaining = lim.per_run_usd - self.run_usd.get(pid, 0.0)
            if est.usd > remaining + 1e-9:
                problems.append(f"estimate US${est.usd:.4f} exceeds {pid}'s per-run cap (US${remaining:.4f} left)")
        if lim.monthly_usd is not None and est.usd > 0:
            remaining = lim.monthly_usd - self.month_spent_usd(pid)
            if est.usd > remaining + 1e-9:
                problems.append(f"estimate US${est.usd:.4f} exceeds {pid}'s monthly cap for {self._month()} "
                                f"(US${remaining:.4f} of US${lim.monthly_usd:.2f} left)")
        if lim.gpu_hours_per_run is not None and est.gpu:
            remaining_s = lim.gpu_hours_per_run * 3600.0 - self.run_gpu_seconds.get(pid, 0.0)
            if est.seconds > remaining_s + 1e-9:
                problems.append(f"estimate {est.seconds / 3600.0:.3f} GPU-hours exceeds {pid}'s per-run cap "
                                f"({remaining_s / 3600.0:.3f} of {lim.gpu_hours_per_run:g} left)")
        if lim.rpm is not None and len(self._recent(pid)) >= lim.rpm:
            problems.append(f"{pid} is at its rate limit of {lim.rpm} requests per minute")
        if problems:
            raise BudgetExceeded(f"budget refused {pid}: " + "; ".join(problems))

    def check_plan(self, items: List[Tuple[ProviderInfo, Cost]]) -> Dict[str, Any]:
        """W4.5 preflight: the *sum* of the estimates of every pending call against the per-run cap, each
        provider's per-run and monthly caps and its GPU-hour cap, before any call is made. The request rate is
        not checked here (it throttles calls; :meth:`check` refuses them one by one). Returns the totals."""
        per_usd: Dict[str, float] = {}
        per_gpu_s: Dict[str, float] = {}
        infos: Dict[str, ProviderInfo] = {}
        for info, est in items:
            infos[info.id] = info
            per_usd[info.id] = per_usd.get(info.id, 0.0) + est.usd
            if est.gpu:
                per_gpu_s[info.id] = per_gpu_s.get(info.id, 0.0) + est.seconds
        total = sum(per_usd.values())
        problems = []
        remaining = self.config.per_run_usd - self.run_usd_total
        if total > remaining + 1e-9:
            problems.append(f"the plan's estimate US${total:.4f} exceeds the per-run cap (US${remaining:.4f} of "
                            f"US${self.config.per_run_usd:.2f} left)")
        for pid, info in sorted(infos.items()):
            lim = self.config.limits_for(info)
            usd = per_usd.get(pid, 0.0)
            if lim.per_run_usd is not None and usd > lim.per_run_usd - self.run_usd.get(pid, 0.0) + 1e-9:
                problems.append(f"{pid}: plan US${usd:.4f} exceeds its per-run cap US${lim.per_run_usd:.2f}")
            if lim.monthly_usd is not None and usd > 0:
                left = lim.monthly_usd - self.month_spent_usd(pid)
                if usd > left + 1e-9:
                    problems.append(f"{pid}: plan US${usd:.4f} exceeds the monthly cap for {self._month()} "
                                    f"(US${left:.4f} of US${lim.monthly_usd:.2f} left)")
            gpu_s = per_gpu_s.get(pid, 0.0)
            if lim.gpu_hours_per_run is not None and gpu_s > 0:
                left_s = lim.gpu_hours_per_run * 3600.0 - self.run_gpu_seconds.get(pid, 0.0)
                if gpu_s > left_s + 1e-9:
                    problems.append(f"{pid}: plan {gpu_s / 3600.0:.3f} GPU-hours exceeds its per-run cap "
                                    f"({left_s / 3600.0:.3f} of {lim.gpu_hours_per_run:g} left)")
        if problems:
            raise BudgetExceeded(f"budget refused the plan of {len(items)} call(s) before any call: "
                                 + "; ".join(problems))
        return {"calls": len(items), "usd": round(total, 6), "per_provider_usd": per_usd,
                "per_provider_gpu_hours": {k: v / 3600.0 for k, v in per_gpu_s.items()}}

    def record(self, info: ProviderInfo, actual: Cost) -> None:
        pid = info.id
        self.run_usd_total += actual.usd
        self.run_usd[pid] = self.run_usd.get(pid, 0.0) + actual.usd
        if actual.gpu:
            self.run_gpu_seconds[pid] = self.run_gpu_seconds.get(pid, 0.0) + actual.seconds
        month = self.ledger["months"].setdefault(self._month(), {})
        row = month.setdefault(pid, {"usd": 0.0, "tokens_in": 0, "tokens_out": 0, "gpu_seconds": 0.0, "kwh": 0.0,
                                     "requests": 0})
        row["usd"] = round(row["usd"] + actual.usd, 6)
        row["tokens_in"] += actual.tokens_in
        row["tokens_out"] += actual.tokens_out
        if actual.gpu:
            row["gpu_seconds"] = round(row["gpu_seconds"] + actual.seconds, 3)
        row["kwh"] = round(row["kwh"] + actual.kwh, 6)
        row["requests"] += 1
        self.ledger["recent"][pid] = self._recent(pid) + [iso(self.clock())]
        if self.ledger_path:
            text = json.dumps(self.ledger, sort_keys=True, indent=2) + "\n"
            _atomic_write(self.ledger_path, text.encode("utf-8"))
