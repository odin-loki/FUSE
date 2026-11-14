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
    runner     ``run_recipe()`` / ``run_many()``: guard -> cache -> budget preflight -> provider -> cache/ledger/audit

Waves 3-5 (no network in CI: REST providers run against recorded HTTP fixtures)
    transport  HTTP transports: ReplayTransport (fixtures), live urllib (refused under FUSE_AI_OFFLINE), retries
    gemini     W4.1/W4.4 Gemini REST provider: pinned ids, batch, context caching, retries, cost, Nano Banana
    local_llm  W4.6 llama.cpp llama-server client (loopback only), same prompts and schemas
    routing    W4.6 originals without the cloud opt-in -> local fallback
    prompts    W4.2 prompt files (Content/recipes/ai/prompts) + JSON-schema output parsing (jsonschema)
    lockrec    W4.4 licence-lock records (watermark: synthid, computed distribution)
    costreport W4.5 ``fuse_ai cost``
    portflow   W4.3 TorqueScript -> spec -> Lua, structural checks, parity harness
    fimage, classify, matcher, fingerprint   W3.1 / W3.4 / W3.5 CPU texture tools
    collision, lightcal, look, emissive      W5.2 / W5.3 / W5.4 / W5.6 CPU tools
    tools      ``python -m fuse_ai tool ...`` CLI for the CPU tools
"""

__version__ = "0.1.0"
RECIPE_SCHEMA = "fuse.ai-recipe/1"
BUDGET_SCHEMA = "fuse.ai-budget/1"
PROJECT_SCHEMA = "fuse.remaster-project/1"

__all__ = ["__version__", "RECIPE_SCHEMA", "BUDGET_SCHEMA", "PROJECT_SCHEMA"]
