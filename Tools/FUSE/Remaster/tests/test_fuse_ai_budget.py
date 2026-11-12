"""W0.5 exit criterion: budget refusal (plan §3.3 "Budgets", §7.4 defaults)."""

import json
import unittest

from tests.fuse_ai_support import TempDirCase, make_recipe

from fuse_ai import providers
from fuse_ai.budget import Budget, BudgetConfig
from fuse_ai.cache import Mode
from fuse_ai.errors import BudgetExceeded, ProviderNotImplemented, RecipeError, VramExceeded
from fuse_ai.providers import Cost, Output, Result, Task
from fuse_ai.runner import run_recipe


class CostlyProvider:
    """A local test provider with a configurable USD / GPU cost (never registered outside these tests)."""

    id = "test-costly"
    cloud = False

    def __init__(self):
        self.calls = 0

    def capabilities(self):
        return {Task.UPSCALE}

    def vram_mb(self, req):
        return int(req.params.get("vram_mb", 0))

    def estimate(self, req):
        return Cost(usd=float(req.params.get("usd", 0.0)), seconds=float(req.params.get("seconds", 0.0)),
                    gpu="rtx3090" if req.params.get("seconds") else None)

    def run(self, req):
        self.calls += 1
        cost = self.estimate(req)
        cost.tokens_in = 10
        return Result([Output("out.bin", "application/octet-stream", b"costly-%d" % req.seed)], cost=cost,
                      provider_id=self.id, model_id="test")


class BudgetCase(TempDirCase):
    def setUp(self):
        super().setUp()
        self.provider = CostlyProvider()
        providers.register(self.provider, replace=True)

    def tearDown(self):
        providers.unregister(self.provider.id)
        super().tearDown()

    def budget(self, cfg=None):
        return Budget(BudgetConfig.from_json(cfg or {}), ledger_path=self.tmp / "ai-ledger.json", clock=self.clock)

    def run_costly(self, budget, seed=1, **params):
        rec = make_recipe(provider="test-costly", params=params, seed=seed)
        return run_recipe(rec, Mode.RECORD, cache_dir=self.cache_dir, budget=budget, clock=self.clock)


class Defaults(unittest.TestCase):
    def test_section_7_4_defaults(self):
        cfg = BudgetConfig()
        self.assertEqual(cfg.per_run_usd, 50.0)
        gem = cfg.limits_for(providers.info("gemini"))
        self.assertEqual(gem.monthly_usd, 300.0)
        local = cfg.limits_for(providers.info("local-torch"))
        self.assertEqual(local.gpu_hours_per_run, 24.0)
        self.assertIsNone(local.monthly_usd)

    def test_bad_budget_file(self):
        with self.assertRaises(RecipeError):
            BudgetConfig.from_json({"providers": {"gemini": {"monthly_usd": -1}}})
        with self.assertRaises(RecipeError):
            BudgetConfig.from_json({"providers": {"gemini": {"montly_usd": 1}}})


class Refusals(BudgetCase):
    def test_per_run_cap_refuses_before_the_provider_is_called(self):
        b = self.budget({"defaults": {"per_run_usd": 5}})
        self.run_costly(b, seed=1, usd=3.0)
        with self.assertRaises(BudgetExceeded) as cm:
            self.run_costly(b, seed=2, usd=3.0)  # 3 + 3 > 5
        self.assertIn("per-run cap", str(cm.exception))
        self.assertEqual(self.provider.calls, 1)

    def test_monthly_cap_uses_the_ledger(self):
        cfg = {"providers": {"test-costly": {"monthly_usd": 10}}}
        self.run_costly(self.budget(cfg), seed=1, usd=8.0)
        ledger = json.loads((self.tmp / "ai-ledger.json").read_text(encoding="utf-8"))
        self.assertEqual(ledger["months"]["2026-11"]["test-costly"]["usd"], 8.0)
        with self.assertRaises(BudgetExceeded) as cm:
            self.run_costly(self.budget(cfg), seed=2, usd=4.0)  # a new run, same month
        self.assertIn("monthly cap", str(cm.exception))
        self.clock.advance(31 * 86400)  # next month: the cap resets
        self.run_costly(self.budget(cfg), seed=3, usd=4.0)

    def test_gpu_hours_cap(self):
        b = self.budget({"providers": {"test-costly": {"gpu_hours_per_run": 1}}})
        self.run_costly(b, seed=1, seconds=3000)
        with self.assertRaises(BudgetExceeded) as cm:
            self.run_costly(b, seed=2, seconds=1200)
        self.assertIn("GPU-hours", str(cm.exception))

    def test_rate_limit(self):
        b = self.budget({"providers": {"test-costly": {"rpm": 2}}})
        self.run_costly(b, seed=1)
        self.run_costly(b, seed=2)
        with self.assertRaises(BudgetExceeded):
            self.run_costly(b, seed=3)
        self.clock.advance(61)
        self.run_costly(b, seed=3)

    def test_actual_spend_recorded_in_recipe_cost(self):
        out = self.run_costly(self.budget(), seed=1, usd=1.5)
        self.assertEqual(out.recipe.cost["usd"], 1.5)
        self.assertEqual(out.recipe.cost["tokens_in"], 10)

    def test_vram_budget_of_the_3090_profile(self):
        with self.assertRaises(VramExceeded):
            self.run_costly(self.budget(), seed=1, vram_mb=30000)

    def test_gemini_estimate_refused_before_the_stub_runs(self):
        # library input: the opt-in guard passes; the §3.2 price estimate exceeds the US$50 per-run default.
        rec = make_recipe(task="PORT_SCRIPT", provider="gemini", params={"max_output_tokens": 20_000_000},
                          model={"id": "gemini-3.1-pro-preview"})
        with self.assertRaises(BudgetExceeded):
            run_recipe(rec, Mode.RECORD, cache_dir=self.cache_dir, budget=self.budget(), clock=self.clock)
        # A cheap request gets past the budget and then hits the W0 stub (no network).
        rec = make_recipe(task="PORT_SCRIPT", provider="gemini", model={"id": "gemini-3.8-flash"})
        with self.assertRaises(ProviderNotImplemented):
            run_recipe(rec, Mode.RECORD, cache_dir=self.cache_dir, budget=self.budget(), clock=self.clock)


if __name__ == "__main__":
    unittest.main()
