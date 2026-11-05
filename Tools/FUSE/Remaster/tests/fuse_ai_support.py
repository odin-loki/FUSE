"""Shared helpers for the test_fuse_ai*.py gates (not collected: no ``test_`` prefix)."""

from __future__ import annotations

import datetime as dt
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, Optional

from tests import NETWORK_ATTEMPTS  # noqa: F401  (importing the package installs the network block)

from fuse_ai import RECIPE_SCHEMA, imaging
from fuse_ai.policy import ProjectPolicy
from fuse_ai.recipe import Recipe

FIXED_NOW = dt.datetime(2026, 11, 5, 12, 0, 0, tzinfo=dt.timezone.utc)


class Clock:
    """Injectable, advanceable clock."""

    def __init__(self, now: dt.datetime = FIXED_NOW):
        self.now = now

    def __call__(self) -> dt.datetime:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now = self.now + dt.timedelta(seconds=seconds)


def recipe_dict(task: str = "UPSCALE", provider: Optional[str] = "mock", origin: str = "library",
                **extra: Any) -> Dict[str, Any]:
    d: Dict[str, Any] = {
        "schema": RECIPE_SCHEMA,
        "id": f"test/{task.lower()}",
        "task": task,
        "model": {"id": "test-model"},
        "params": {"scale": 2} if task == "UPSCALE" else {},
        "seed": 7,
        "inputs": [{"oaid": "oaid-1", "canonical_sha256": "1" * 64, "origin": origin, "name": "brick_wall"}],
    }
    if origin == "derived":
        d["inputs"][0]["derived_from_original"] = False
    if provider:
        d["provider"] = provider
    d.update(extra)
    return d


def make_recipe(**kw: Any) -> Recipe:
    return Recipe.from_json(recipe_dict(**kw))


def project(cloud_upload: str = "none", confirmed: bool = False, tier: Optional[str] = None,
            game_id: str = "test-game", path: Optional[Path] = None) -> ProjectPolicy:
    d: Dict[str, Any] = {"schema": "fuse.remaster-project/1", "game_id": game_id, "cloud_upload": cloud_upload}
    if confirmed:
        d["cloud_upload_confirmed"] = {"by": "Test User", "date": "2026-11-05", "game_id": game_id}
    if tier:
        d["cloud_tier"] = {"gemini": tier}
    return ProjectPolicy.from_json(d, path)


def tiny_png(seed: bytes = b"tiny", size: int = 8) -> bytes:
    return imaging.encode_png(imaging.synthetic_image(seed, size, size))


class TempDirCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="fuse_ai_test_"))
        self.cache_dir = self.tmp / "ai-cache"
        self.clock = Clock()

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_json(self, name: str, obj: Any) -> Path:
        p = self.tmp / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(obj, indent=2, sort_keys=True), encoding="utf-8")
        return p
