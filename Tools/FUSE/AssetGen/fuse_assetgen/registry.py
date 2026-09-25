"""Generator registry.

A generator is a function `fn(ctx) -> None` registered with @generator(name, version, params, ...).
It reads `ctx.params` (validated, defaults filled), draws every random number from `ctx.stream`
(fuse_assetgen.seed.SeedStream of the recipe seed) and emits files with `ctx.emit(suffix_name, bytes)`.
It must not read the clock, the environment, the network or files outside `ctx.inputs`.

`version` is bumped whenever the output bytes of an unchanged recipe change; together with the
sha256 of the generator's source file it is part of the cache key.
"""

from __future__ import annotations

import hashlib
import importlib
import inspect
import re
from dataclasses import dataclass, field
from pathlib import Path

_OUTPUT_RE = re.compile(r"^[a-z0-9_]+\.(png|json|obj|gltf|bin|wav|txt)$")


@dataclass
class Generator:
    name: str
    version: int
    fn: object
    params: dict
    required: list = field(default_factory=list)
    description: str = ""

    @property
    def source_path(self) -> Path:
        return Path(inspect.getsourcefile(self.fn)).resolve()

    def source_sha256(self) -> str:
        # CRLF folded so a text=auto checkout hashes the same as the LF one.
        data = self.source_path.read_bytes().replace(b"\r\n", b"\n")
        return hashlib.sha256(data).hexdigest()


class Registry:
    def __init__(self):
        self._gens = {}

    def add(self, gen: Generator) -> None:
        if gen.name in self._gens:
            raise ValueError(f"generator {gen.name!r} registered twice")
        self._gens[gen.name] = gen

    def get(self, name):
        return self._gens.get(name)

    def names(self):
        return sorted(self._gens)


REGISTRY = Registry()


def generator(name: str, version: int, params: dict, required=(), description: str = "", registry: Registry = None):
    def wrap(fn):
        (registry or REGISTRY).add(Generator(name, version, fn, params, list(required), description))
        return fn
    return wrap


class GenContext:
    """What a generator sees. `emit` collects outputs in call order (names must be unique)."""

    def __init__(self, recipe: dict, stream):
        self.recipe = recipe
        self.params = recipe["params"]
        self.stream = stream
        self.asset_name = recipe["id"].rsplit("/", 1)[1]
        self.outputs = []  # list of (file name, bytes)

    def emit(self, suffix_name: str, data: bytes) -> str:
        """Emits `<asset leaf>_<suffix_name>` (e.g. 'height.png' -> 'noise_a_height.png')."""
        fname = f"{self.asset_name}_{suffix_name}"
        if not _OUTPUT_RE.match(fname):
            raise ValueError(f"output name {fname!r} must match {_OUTPUT_RE.pattern}")
        if any(n == fname for n, _ in self.outputs):
            raise ValueError(f"output {fname!r} emitted twice")
        if not isinstance(data, (bytes, bytearray)):
            raise TypeError("emit() takes bytes")
        self.outputs.append((fname, bytes(data)))
        return fname


def load_builtin_generators() -> Registry:
    """Imports fuse_assetgen.generators.* so their decorators register (idempotent)."""
    from . import generators
    for mod in generators.BUILTIN:
        importlib.import_module(f"{generators.__name__}.{mod}")
    return REGISTRY
