"""Licence records for generated assets, in the Content/licences.lock.json format (FUSE_ASSET_PLAN §2.4).

A generated record carries what `fuse_lint asset-licences` (W0.5) checks for origin "generated":
  id, origin, source, author, generator "<repo path>@<git revision>", numeric seed, licence,
  retrieval "generator", derived_from, files [{path, sha256, location: "cache"}], review.
Files are named relative to the asset-cache root (location "cache"), so the lint hashes them when the
cache is populated and skips them otherwise. `recipe` / `recipe_sha256` / `generator_version` /
`cache_key` are extra fields that let the asset_determinism gate rebuild and compare the bytes.

Coordination with W0.5 is by format only (no shared code).
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

GENERATED_SOURCE = "FUSE AssetGen"
GENERATED_AUTHOR = "FUSE contributors"


def repo_root() -> Path:
    # fuse_assetgen/ -> AssetGen/ -> FUSE/ -> Tools/ -> repo
    return Path(__file__).resolve().parents[4]


def git_revision(root: Path = None) -> str:
    """HEAD commit (40 hex), FUSE_ASSETGEN_GIT_REV when set, else 'unknown' (which the lint rejects)."""
    env = os.environ.get("FUSE_ASSETGEN_GIT_REV")
    if env:
        return env
    try:
        out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(root or repo_root()), capture_output=True,
                             text=True, timeout=30, check=True)
        rev = out.stdout.strip()
        return rev if len(rev) == 40 else "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def repo_relative(path: Path, root: Path = None) -> str:
    root = (root or repo_root()).resolve()
    try:
        return Path(path).resolve().relative_to(root).as_posix()
    except ValueError:
        return Path(path).resolve().as_posix()


def make_record(recipe: dict, generator, files, cache_root: Path, git_rev: str, recipe_path=None,
                recipe_sha256: str = None, cache_key: str = None) -> dict:
    """`files`: [(absolute path, sha256)] of materialised outputs under `cache_root`."""
    rec = {
        "id": recipe["id"],
        "origin": "generated",
        "source": GENERATED_SOURCE,
        "author": GENERATED_AUTHOR,
        "generator": f"{repo_relative(generator.source_path)}@{git_rev}",
        "generator_version": generator.version,
        "seed": recipe["seed"],
        "licence": recipe["licence"],
        "retrieval": "generator",
        "attribution": None,
        "derived_from": list(recipe["derived_from"]),
        "files": [
            {"path": Path(p).resolve().relative_to(Path(cache_root).resolve()).as_posix(), "sha256": sha, "location": "cache"}
            for p, sha in files
        ],
        "review": {"required": False, "by": None, "date": None},
    }
    if recipe_path is not None:
        rec["recipe"] = repo_relative(recipe_path)
    if recipe_sha256:
        rec["recipe_sha256"] = recipe_sha256
    if cache_key:
        rec["cache_key"] = cache_key
    return rec


def merge_into_lock(lock_path, records) -> dict:
    """Replaces records with the same id in `lock_path` (created if missing), keeps the others,
    sorts by id and writes it back with a stable layout. Returns the lock document."""
    lock_path = Path(lock_path)
    if lock_path.is_file():
        doc = json.loads(lock_path.read_text(encoding="utf-8"))
    else:
        doc = {"schema": 1, "assets": []}
    if doc.get("schema") != 1 or not isinstance(doc.get("assets"), list):
        raise ValueError(f"{lock_path}: not a schema-1 licence lock")
    by_id = {a.get("id"): a for a in doc["assets"]}
    for rec in records:
        by_id[rec["id"]] = rec
    doc["assets"] = [by_id[k] for k in sorted(by_id, key=lambda k: (k is None, k or ""))]
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = lock_path.with_name(lock_path.name + ".tmp")
    tmp.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    os.replace(tmp, lock_path)
    return doc
