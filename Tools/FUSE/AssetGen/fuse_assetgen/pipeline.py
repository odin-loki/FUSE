"""generate(recipe) -> outputs (+ cache, materialisation and licence record)."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from pathlib import Path

from . import PIPELINE_VERSION
from .cache import Cache
from .licence import git_revision, make_record
from .registry import GenContext, load_builtin_generators
from .schema import canonical_json, load_and_validate, validate_recipe
from .seed import SeedStream


@dataclass
class Result:
    recipe: dict
    key: str
    cache_hit: bool
    outputs: list  # [(name, bytes)]
    paths: list = field(default_factory=list)
    record: dict = None

    def digests(self):
        return [(n, hashlib.sha256(d).hexdigest()) for n, d in self.outputs]


def cache_key(recipe: dict, gen) -> tuple:
    inputs = {
        "pipeline": PIPELINE_VERSION,
        "generator": gen.name,
        "generator_version": gen.version,
        "generator_source_sha256": gen.source_sha256(),
        # licence / derived_from / description do not change the bytes.
        "recipe": {k: recipe[k] for k in ("schema", "id", "generator", "seed", "params")},
    }
    return hashlib.sha256(canonical_json(inputs)).hexdigest(), inputs


def run_generator(recipe: dict, registry=None) -> list:
    """Runs the generator uncached; returns [(name, bytes)]."""
    registry = registry or load_builtin_generators()
    gen = registry.get(recipe["generator"])
    ctx = GenContext(recipe, SeedStream(recipe["seed"]))
    gen.fn(ctx)
    if not ctx.outputs:
        raise RuntimeError(f"generator {gen.name!r} emitted no output")
    return ctx.outputs


def generate(recipe_or_path, cache_root, seed=None, registry=None, use_cache=True, materialise=True,
             git_rev=None, recipe_path=None) -> Result:
    registry = registry or load_builtin_generators()
    if isinstance(recipe_or_path, (str, Path)):
        recipe_path = Path(recipe_or_path)
        recipe = load_and_validate(recipe_path, registry, seed_override=seed)
    else:
        data = dict(recipe_or_path)
        if seed is not None:
            data["seed"] = seed
        recipe = validate_recipe(data, registry)
    gen = registry.get(recipe["generator"])
    key, key_inputs = cache_key(recipe, gen)
    cache = Cache(cache_root)
    outputs = cache.lookup(key) if use_cache else None
    hit = outputs is not None
    if not hit:
        outputs = run_generator(recipe, registry)
        cache.store(key, key_inputs, outputs)
    result = Result(recipe, key, hit, outputs)
    if materialise:
        result.paths = cache.materialise(recipe["id"], outputs)
        recipe_sha = hashlib.sha256(canonical_json(recipe)).hexdigest()
        result.record = make_record(recipe, gen, list(zip(result.paths, (d for _, d in result.digests()))),
                                    cache.root, git_rev or git_revision(), recipe_path, recipe_sha, key)
    return result
