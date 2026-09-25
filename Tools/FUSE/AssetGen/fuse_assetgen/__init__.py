"""FUSE AssetGen: deterministic, seed-driven asset generators (FUSE_ASSET_PLAN §3.1, Wave 0 W0.9).

Every generator takes a JSON (or YAML) recipe plus a seed and produces byte-identical outputs for the
same (generator version, recipe, seed). Outputs land in a content-addressed cache (default
build/asset-cache/assetgen, git-ignored) and each generated asset gets a licence record in the
Content/licences.lock.json format of FUSE_ASSET_PLAN §2.4.

Python standard library only. Modules:
  schema     recipe loading (JSON, YAML when PyYAML is importable) and validation
  seed       SplitMix64 / PCG32 random streams derived from the recipe seed by name
  cache      content-addressed blob store + action cache keyed by generator/recipe/seed
  licence    §2.4 licence records and lock-file merging
  pngio      deterministic PNG writer (stored deflate, no zlib-version dependence)
  registry   generator registry (@generator decorator, per-generator parameter schema)
  pipeline   generate(recipe) -> outputs + licence record
  cli        `python -m fuse_assetgen {validate,generate,verify,list,hash}`
"""

__all__ = ["__version__", "PIPELINE_VERSION"]

__version__ = "0.1.0"
# Bumped when the cache key derivation or the output layout changes (invalidates every cache entry).
PIPELINE_VERSION = 1
