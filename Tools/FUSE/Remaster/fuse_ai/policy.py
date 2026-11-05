"""W0.8: the cloud opt-in guard (plan §0.1.5, §7 Wave 0 row W0.8).

Rule (§0.1.5): no original asset, capture, screenshot or script is sent to any cloud API unless the
per-game project file sets ``"cloud_upload": "opt-in"`` *and* the user confirmed it once per game. Even
then only **paid-tier** keys may receive original content, and every upload is written to the per-game
audit log. Local providers (and ``mock``) never need the opt-in: nothing leaves the machine.

Per-game project file (``remaster.project.json``, written by ``fuse_remaster init`` with ``cloud_upload: none``)::

    {"schema": "fuse.remaster-project/1", "game_id": "tge-demo",
     "cloud_upload": "none" | "opt-in",
     "cloud_upload_confirmed": {"by": "<name>", "date": "YYYY-MM-DD", "game_id": "tge-demo"},
     "cloud_tier": {"gemini": "paid" | "free"},
     "audit_log": "ai-audit.jsonl"}

An input counts as original content when ``origin == "original"`` or ``derived_from_original`` is true
(a derivative with an original anywhere in its chain); a missing origin is treated as original.

:func:`check_request` is called by :func:`fuse_ai.runner.run_recipe` before the cache lookup and before
any provider call, in every mode. On success it stamps ``req.clearance`` with an HMAC (per-process
random key) over the recipe hash, provider id and input provenance; cloud providers call
:func:`verify_clearance` at the top of ``run()`` and refuse a request that did not pass the guard or was
changed after it, so calling a cloud provider directly cannot bypass the guard either.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import PROJECT_SCHEMA
from .errors import CloudOptInRequired, PaidTierRequired, PolicyError, RecipeError
from .providers import InputRef, ProviderInfo, Request

CLOUD_UPLOAD_VALUES = ("none", "opt-in")
TIERS = ("paid", "free")
_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}")
_SECRET = os.urandom(32)  # per process; clearances never outlive the process


@dataclass
class ProjectPolicy:
    game_id: str = ""
    cloud_upload: str = "none"
    confirmed: Optional[Dict[str, Any]] = None
    cloud_tier: Dict[str, str] = field(default_factory=dict)
    audit_log: Optional[str] = None
    path: Optional[Path] = None

    @classmethod
    def from_json(cls, d: Any, path: Optional[Path] = None) -> "ProjectPolicy":
        if not isinstance(d, dict):
            raise RecipeError("remaster.project.json must be a JSON object")
        if d.get("schema", PROJECT_SCHEMA) != PROJECT_SCHEMA:
            raise RecipeError(f"remaster.project.json schema must be {PROJECT_SCHEMA!r}")
        cu = d.get("cloud_upload", "none")
        if cu not in CLOUD_UPLOAD_VALUES:
            raise RecipeError(f"cloud_upload must be one of {CLOUD_UPLOAD_VALUES}, got {cu!r}")
        conf = d.get("cloud_upload_confirmed")
        if conf is not None and not isinstance(conf, dict):
            raise RecipeError("cloud_upload_confirmed must be an object {by, date}")
        tiers = d.get("cloud_tier", {}) or {}
        if not isinstance(tiers, dict) or any(t not in TIERS for t in tiers.values()):
            raise RecipeError(f"cloud_tier must map provider ids to one of {TIERS}")
        audit = d.get("audit_log")
        if audit is not None and not isinstance(audit, str):
            raise RecipeError("audit_log must be a string path")
        return cls(game_id=str(d.get("game_id", "")), cloud_upload=cu, confirmed=conf, cloud_tier=dict(tiers),
                   audit_log=audit, path=path)

    @classmethod
    def load(cls, path: Optional[str | Path]) -> "ProjectPolicy":
        """No project file means the safe default: ``cloud_upload: none``."""
        if path is None:
            return cls()
        p = Path(path)
        try:
            d = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            raise RecipeError(f"cannot read project file {p}: {e}") from None
        return cls.from_json(d, p)

    @property
    def is_confirmed(self) -> bool:
        c = self.confirmed
        if not c:
            return False
        by, date = c.get("by"), c.get("date")
        if not (isinstance(by, str) and by.strip() and isinstance(date, str) and _DATE.match(date)):
            return False
        # A confirmation copied from another game's project file does not count.
        if c.get("game_id") is not None and c.get("game_id") != self.game_id:
            return False
        return True

    def audit_path(self, fallback_dir: Path) -> Path:
        if self.audit_log:
            p = Path(self.audit_log)
            if not p.is_absolute() and self.path is not None:
                p = self.path.parent / p
            return p
        if self.path is not None:
            return self.path.parent / "ai-audit.jsonl"
        return fallback_dir / "ai-audit.jsonl"


@dataclass
class Decision:
    cloud: bool
    originals: List[InputRef]
    tier: Optional[str] = None

    @property
    def uploads_original(self) -> bool:
        return self.cloud and bool(self.originals)


def _fingerprint(req: Request) -> bytes:
    prov = [[i.content_key, i.origin, bool(i.derived_from_original)] for i in req.inputs]
    return json.dumps([req.recipe_hash, req.provider_id, prov], sort_keys=True).encode("utf-8")


def _token(req: Request) -> str:
    return hmac.new(_SECRET, _fingerprint(req), hashlib.sha256).hexdigest()


def check_request(req: Request, info: ProviderInfo, project: ProjectPolicy) -> Decision:
    """The guard. Raises CloudOptInRequired / PaidTierRequired; on success stamps ``req.clearance``."""
    originals = [i for i in req.inputs if i.is_original_content]
    if not info.cloud:
        req.clearance = _token(req)
        return Decision(cloud=False, originals=originals)
    tier = project.cloud_tier.get(info.id)
    if originals:
        what = ", ".join(i.content_key or i.path or "?" for i in originals)
        if info.human:
            raise CloudOptInRequired(
                f"provider {info.id!r} (reference / consumer products such as Project Genie) is never given "
                f"original assets, even with the opt-in (§0.1.5); original inputs: {what}")
        if project.cloud_upload != "opt-in":
            raise CloudOptInRequired(
                f"recipe sends original content ({what}) to cloud provider {info.id!r}, but the project "
                f"{'file ' + str(project.path) if project.path else '(none given)'} has cloud_upload: "
                f"{project.cloud_upload!r}. Set \"cloud_upload\": \"opt-in\" and confirm it once for this game "
                "(§0.1.5), or use a local provider")
        if not project.is_confirmed:
            raise CloudOptInRequired(
                f"cloud_upload is 'opt-in' for game {project.game_id!r} but the per-game confirmation is "
                "missing or incomplete (cloud_upload_confirmed: {by, date}); confirm it in the editor or CLI")
        if tier != "paid":
            raise PaidTierRequired(
                f"original content may only go to {info.id!r} with a paid-tier key (cloud_tier.{info.id} is "
                f"{tier!r}); unpaid-tier prompts are used to improve the provider's products (§0.1.5)")
    req.clearance = _token(req)
    return Decision(cloud=True, originals=originals, tier=tier)


def verify_clearance(req: Request) -> None:
    """Called by cloud providers before doing anything; refuses requests the guard did not clear."""
    if not req.clearance or not hmac.compare_digest(req.clearance, _token(req)):
        raise PolicyError(f"request {req.recipe_hash or '?'} to {req.provider_id!r} was not cleared by the "
                          "cloud opt-in guard (fuse_ai.policy.check_request); use fuse_ai.runner.run_recipe")
