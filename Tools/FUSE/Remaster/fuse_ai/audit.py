"""Append-only JSONL audit log (plan §0.1.5: "the upload log (hash, provider, model, time, purpose) goes to
the per-game audit file").

One JSON object per line, keys sorted, no whitespace, so the file is deterministic when a clock is
injected. Lines are only ever appended (``open(..., "a")``); nothing in fuse_ai rewrites or truncates it.

Upload entries (written *before* a cloud provider is called, so a crash mid-call still leaves the record)::

    {"event": "upload", "time": "…Z", "recipe_hash": …, "recipe_id": …, "provider": "gemini", "model": …,
     "task": …, "purpose": …, "inputs": [{"hash": …, "origin": …, "derived_from_original": …}],
     "originals": true, "opt_in": "opt-in", "confirmed_by": …, "tier": "paid", "game_id": …}
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, List, Optional

from .budget import Clock, iso, utc_now

AUDIT_SCHEMA = "fuse.ai-audit/1"


class AuditLog:
    def __init__(self, path: str | Path, clock: Clock = utc_now):
        self.path = Path(path)
        self.clock = clock

    def append(self, event: str, **fields: Any) -> Dict[str, Any]:
        entry: Dict[str, Any] = {"schema": AUDIT_SCHEMA, "event": event, "time": iso(self.clock())}
        entry.update(fields)
        line = json.dumps(entry, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n"
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.path, "a", encoding="utf-8", newline="\n") as f:
            f.write(line)
            f.flush()
        return entry

    def entries(self, event: Optional[str] = None) -> List[Dict[str, Any]]:
        if not self.path.is_file():
            return []
        out = []
        with open(self.path, encoding="utf-8") as f:
            for line in f:
                if line.strip():
                    e = json.loads(line)
                    if event is None or e.get("event") == event:
                        out.append(e)
        return out
