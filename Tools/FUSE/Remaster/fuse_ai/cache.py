"""Result cache keyed by the canonical recipe hash (plan §3.3, "Cache").

Layout under the cache dir (default ``build/ai-cache/``, git-ignored per W0.6)::

    blobs/<aa>/<sha256>          output bytes, content-addressed by their sha256
    entries/<recipe-hash>.json   {"recipe_hash", "recipe", "provider", "model", "task", "outputs": [...], "meta", "cost"}

Modes (:class:`Mode`):

* ``replay`` - the CI default. Only cached outputs are used; a miss raises :class:`ReplayMiss`.
* ``record`` - call the provider and store the result (a hit is reused unless ``force``).
* ``mock``   - route to the ``mock`` provider; the cache is neither read nor written.

Blobs are verified against their sha256 on every read (:class:`CacheCorrupt` on mismatch). Writes go
through a temp file + ``os.replace`` so a crash never leaves a half-written entry.
"""

from __future__ import annotations

import enum
import hashlib
import json
import os
import tempfile
from pathlib import Path
from typing import Any, Dict, Optional

from .errors import CacheCorrupt, RecipeError
from .providers import Cost, Output, Result

DEFAULT_CACHE_DIR = Path("build") / "ai-cache"
ENTRY_SCHEMA = "fuse.ai-cache-entry/1"


class Mode(str, enum.Enum):
    REPLAY = "replay"
    RECORD = "record"
    MOCK = "mock"

    @classmethod
    def parse(cls, value: str) -> "Mode":
        try:
            return cls(value)
        except ValueError:
            raise RecipeError(f"unknown mode {value!r}; expected replay, record or mock") from None


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=".tmp-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


class ResultCache:
    def __init__(self, root: str | Path = DEFAULT_CACHE_DIR):
        self.root = Path(root)

    # ---- blobs --------------------------------------------------------------------------------------------------

    def blob_path(self, sha256: str) -> Path:
        return self.root / "blobs" / sha256[:2] / sha256

    def put_blob(self, data: bytes) -> str:
        sha = hashlib.sha256(data).hexdigest()
        p = self.blob_path(sha)
        if not p.exists():
            _atomic_write(p, data)
        return sha

    def get_blob(self, sha256: str) -> bytes:
        p = self.blob_path(sha256)
        try:
            data = p.read_bytes()
        except OSError:
            raise CacheCorrupt(f"cache blob {sha256} is missing ({p})") from None
        if hashlib.sha256(data).hexdigest() != sha256:
            raise CacheCorrupt(f"cache blob {sha256} does not match its hash ({p})")
        return data

    def has_blob(self, sha256: str) -> bool:
        return self.blob_path(sha256).is_file()

    # ---- entries ------------------------------------------------------------------------------------------------

    def entry_path(self, recipe_hash: str) -> Path:
        return self.root / "entries" / f"{recipe_hash}.json"

    def lookup(self, recipe_hash: str) -> Optional[Result]:
        p = self.entry_path(recipe_hash)
        if not p.is_file():
            return None
        try:
            entry = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            raise CacheCorrupt(f"cache entry {p} is unreadable: {e}") from None
        if entry.get("schema") != ENTRY_SCHEMA or entry.get("recipe_hash") != recipe_hash:
            raise CacheCorrupt(f"cache entry {p} has the wrong schema or hash")
        outputs = [Output(o["name"], o["media_type"], self.get_blob(o["sha256"])) for o in entry["outputs"]]
        return Result(outputs=outputs, meta=entry.get("meta", {}), cost=Cost.from_json(entry.get("cost")),
                      provider_id=entry.get("provider", ""), model_id=entry.get("model", ""), from_cache=True)

    def store(self, recipe_hash: str, hash_payload: Dict[str, Any], result: Result) -> Dict[str, Any]:
        outs = []
        for o in result.outputs:
            outs.append({"name": o.name, "media_type": o.media_type, "sha256": self.put_blob(o.data),
                         "size": len(o.data)})
        entry = {
            "schema": ENTRY_SCHEMA,
            "recipe_hash": recipe_hash,
            "recipe": hash_payload,
            "provider": result.provider_id,
            "model": result.model_id,
            "task": hash_payload.get("task"),
            "outputs": outs,
            "meta": result.meta,
            "cost": result.cost.to_json(),
        }
        text = json.dumps(entry, sort_keys=True, indent=2, ensure_ascii=False) + "\n"
        _atomic_write(self.entry_path(recipe_hash), text.encode("utf-8"))
        return entry
