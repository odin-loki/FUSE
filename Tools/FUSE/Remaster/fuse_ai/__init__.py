"""fuse_ai: the provider-agnostic AI services layer of FUSE Remaster.

Plan: docs/plans/FUSE_REMASTER_PLAN.md §3 (the AI services layer), tasks W0.5 (package skeleton:
Provider protocol, recipe schema, cache with replay/record/mock, budgets, audit log) and W0.8 (the
cloud opt-in guard of §0.1.5).

Stdlib only. Nothing in this package opens a network connection: in Wave 0 only the ``mock``
providers and the ``replay`` cache mode are real. The local and cloud provider ids exist as stubs
that refuse to run with :class:`fuse_ai.errors.ProviderNotImplemented`.

Modules
    errors     exception hierarchy (each maps to a CLI exit code)
    providers  Task enum, Request / Result / Cost, Provider protocol, the §3.1 provider table, registry
    imaging    tiny deterministic RGBA8 image + PNG (stored deflate) + WAV helpers for the mocks
    mock       deterministic CPU mock providers for every Task (§3.1 "mock" row)
    recipe     the ``fuse.ai-recipe/1`` schema and its canonical hash (§3.3)
    cache      result cache keyed by the canonical recipe hash, content-addressed blobs (§3.3)
    budget     ``ai-budget.json`` caps and the spend ledger (§3.3, §7.4)
    audit      append-only JSONL audit log (§0.1.5)
    policy     the W0.8 cloud opt-in guard (§0.1.5)
    runner     ``run_recipe()``: guard -> cache -> budget -> provider -> cache/ledger/audit
"""

__version__ = "0.1.0"
RECIPE_SCHEMA = "fuse.ai-recipe/1"
BUDGET_SCHEMA = "fuse.ai-budget/1"
PROJECT_SCHEMA = "fuse.remaster-project/1"

__all__ = ["__version__", "RECIPE_SCHEMA", "BUDGET_SCHEMA", "PROJECT_SCHEMA"]
