"""Content-addressed cache (root default build/asset-cache, git-ignored; AssetGen uses <root>/assetgen/).

Layout under <root>/assetgen/:
  cas/<aa>/<sha256>              immutable blobs, named by the sha256 of their bytes
  actions/<aa>/<key>.json        action records: cache key -> [{name, sha256, size}], key inputs
  assets/<asset id>/<file>       materialised copies of the latest outputs (what licence records name,
                                 relative to <root>, i.e. the asset-cache root the licence lint takes)

The action key is sha256(canonical JSON of {pipeline version, generator name, generator version,
generator source sha256, normalised recipe}); the seed is part of the recipe. A hit re-hashes each
blob before trusting it, so a corrupted or hand-edited cache entry is detected and regenerated.
All writes go through a temp file + os.replace (atomic on POSIX and Windows).
"""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
from pathlib import Path


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=".tmp-")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


class Cache:
    def __init__(self, root):
        self.root = Path(root)
        self.base = self.root / "assetgen"

    # ---- blobs ------------------------------------------------------------------------------
    def blob_path(self, digest: str) -> Path:
        return self.base / "cas" / digest[:2] / digest

    def put_blob(self, data: bytes) -> str:
        digest = sha256_bytes(data)
        path = self.blob_path(digest)
        if not (path.is_file() and sha256_bytes(path.read_bytes()) == digest):
            _atomic_write(path, data)
        return digest

    def get_blob(self, digest: str):
        """Bytes of blob `digest`, or None when missing or corrupted."""
        path = self.blob_path(digest)
        if not path.is_file():
            return None
        data = path.read_bytes()
        return data if sha256_bytes(data) == digest else None

    # ---- actions ------------------------------------------------------------------------------
    def action_path(self, key: str) -> Path:
        return self.base / "actions" / key[:2] / f"{key}.json"

    def lookup(self, key: str):
        """[(name, bytes)] for a complete, uncorrupted action, else None."""
        path = self.action_path(key)
        if not path.is_file():
            return None
        try:
            record = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        outputs = []
        for entry in record.get("outputs", []):
            data = self.get_blob(entry.get("sha256", ""))
            if data is None or len(data) != entry.get("size"):
                return None
            outputs.append((entry["name"], data))
        return outputs

    def store(self, key: str, key_inputs: dict, outputs) -> list:
        entries = [{"name": name, "sha256": self.put_blob(data), "size": len(data)} for name, data in outputs]
        record = {"key": key, "inputs": key_inputs, "outputs": entries}
        _atomic_write(self.action_path(key), (json.dumps(record, sort_keys=True, indent=1) + "\n").encode("utf-8"))
        return entries

    # ---- materialisation ------------------------------------------------------------------------
    def asset_dir(self, asset_id: str) -> Path:
        return self.base / "assets" / Path(*asset_id.split("/"))

    def materialise(self, asset_id: str, outputs, dest_root=None) -> list:
        """Writes outputs to <dest_root or cache>/assets/<id>/; returns their paths (stale files removed)."""
        base = (Path(dest_root) / Path(*asset_id.split("/"))) if dest_root else self.asset_dir(asset_id)
        base.mkdir(parents=True, exist_ok=True)
        names = {name for name, _ in outputs}
        for stale in base.iterdir():
            if stale.is_file() and stale.name not in names and not stale.name.startswith("."):
                stale.unlink()
        paths = []
        for name, data in outputs:
            path = base / name
            if not (path.is_file() and path.read_bytes() == data):
                _atomic_write(path, data)
            paths.append(path)
        return paths
