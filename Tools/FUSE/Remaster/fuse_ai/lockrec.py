"""Licence-lock records for AI outputs (plan §2.6, §3.3; W4.4 "SynthID noted in the lock").

:func:`lock_record` turns a :class:`~fuse_ai.runner.RunOutcome` into an ``assets[]`` entry of the asset plan's
``Content/licences.lock.json`` extended per §2.6: ``origin: ai`` with an ``ai`` block (provider, model, served
model version, tier, recipe, seed), the computed ``distribution``, ``human_authorship``, ``game_id`` and the
``cloud_uploads`` that sent an original to a cloud API.

* Nano Banana (EDIT_IMAGE / GEN_IMAGE through ``gemini``) outputs carry ``watermark: synthid`` (§0.1 table:
  every image carries an invisible SynthID watermark [M5]) and the ``LicenseRef-AI-Gemini-Image`` licence id.
* ``distribution`` is the runner's computed value (§2.2): any original (or original-derived) input makes the
  output **recipe-only**, and a recipe-only record lists no shippable files - the patch ships the recipe and
  the user's machine rebuilds the output. Only ``ship`` records list output files.
"""

from __future__ import annotations

import hashlib
from typing import Any, Dict, List, Optional

from .runner import RunOutcome

LICENCE_BY_PROVIDER = {"gemini": "LicenseRef-AI-Gemini", "local-llm": "Apache-2.0-model-output",
                       "mock": "LicenseRef-FUSE-Generated"}


def lock_record(outcome: RunOutcome, asset_id: str, *, files_dir: str = "", game_id: Optional[str] = None,
                tier: Optional[str] = None) -> Dict[str, Any]:
    recipe = outcome.recipe
    meta = outcome.result.meta
    out = recipe.output or {}
    dist = out.get("distribution", "recipe-only")
    provider = outcome.provider_id
    licence = meta.get("licence") or LICENCE_BY_PROVIDER.get(provider, f"LicenseRef-AI-{provider}")
    files: List[Dict[str, str]] = []
    if dist == "ship":
        prefix = files_dir.rstrip("/") + "/" if files_dir else ""
        files = [{"path": prefix + o.name, "sha256": hashlib.sha256(o.data).hexdigest()}
                 for o in outcome.result.outputs]
    rec: Dict[str, Any] = {
        "id": asset_id,
        "origin": "ai",
        "source": provider,
        "author": "FUSE contributors (AI-assisted)",
        "generator": f"Tools/FUSE/Remaster/fuse_ai {provider}",
        "seed": recipe.seed,
        "inputs": [{"hash": i.content_key, "origin": i.origin, "derived_from_original": bool(i.derived_from_original)}
                   for i in recipe.inputs],
        "licence": licence,
        "attribution": None,
        "distribution": dist,
        "files": files,
        "derived_from": [f"oaid:{i.oaid}" if i.oaid else f"sha256:{i.content_key}" for i in recipe.inputs],
        "review": {"required": True, "by": None, "date": None},
        "human_authorship": "selection",
        "ai": {"provider": provider, "model": outcome.result.model_id or recipe.model.get("id"),
               "model_version": meta.get("model_version"), "tier": tier,
               "recipe": f"recipe:{recipe.id}@{outcome.recipe_hash[:12]}", "seed": recipe.seed,
               "output_sha256": out.get("sha256")},
    }
    if dist != "ship":
        rec["recipe_only"] = True
    if meta.get("watermark"):
        rec["watermark"] = meta["watermark"]
    if game_id:
        rec["game_id"] = game_id
    uploads = [e for e in outcome.audit_entries if e.get("event") == "upload"]
    if uploads:
        rec["cloud_uploads"] = [f"{e['time']}/{e['recipe_hash'][:12]}" for e in uploads]
    return rec
