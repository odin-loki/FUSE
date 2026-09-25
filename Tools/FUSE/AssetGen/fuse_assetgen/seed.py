"""Seed plumbing: every random number a generator uses comes from a stream derived from the recipe seed.

Rules (checked by the asset_determinism gate):
  * never use `random`, `numpy.random` global state, `hash()`, time, the environment or dict/set
    iteration order of unsorted data;
  * derive one named sub-stream per purpose (`stream.derive("octave", i)`) so adding a new consumer
    does not shift the numbers an existing one sees;
  * the generators are fully specified integer algorithms (SplitMix64 for derivation, PCG32 XSH-RR
    for draws), so the bytes do not depend on the Python version or platform.
"""

from __future__ import annotations

import hashlib
import struct

MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1
MAX_SEED = (1 << 63) - 1


def splitmix64(x: int) -> int:
    """One SplitMix64 output for state `x` (Steele et al. 2014)."""
    z = (x + 0x9E3779B97F4A7C15) & MASK64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return z ^ (z >> 31)


def derive_seed(seed: int, *path: object) -> int:
    """Stable 64-bit sub-seed of `seed` for a purpose path such as ("octave", 3).

    Path elements are str / int / bool only; they are encoded with type tags so ("1",) and (1,)
    differ. SHA-256 keeps unrelated paths independent.
    """
    if not isinstance(seed, int) or isinstance(seed, bool) or not 0 <= seed <= MAX_SEED:
        raise ValueError(f"seed must be an integer in [0, 2^63-1], got {seed!r}")
    h = hashlib.sha256(b"fuse-assetgen-seed\0" + struct.pack("<Q", seed))
    for part in path:
        if isinstance(part, bool):
            h.update(b"b" + (b"1" if part else b"0"))
        elif isinstance(part, int):
            h.update(b"i" + str(part).encode("ascii") + b"\0")
        elif isinstance(part, str):
            data = part.encode("utf-8")
            h.update(b"s" + struct.pack("<I", len(data)) + data)
        else:
            raise TypeError(f"seed path elements must be str/int/bool, got {type(part).__name__}")
    return struct.unpack("<Q", h.digest()[:8])[0]


class Pcg32:
    """PCG32 (XSH-RR 64/32, O'Neill 2014) with a fixed stream constant."""

    _MULT = 6364136223846793005
    _INC = 1442695040888963407

    def __init__(self, seed: int):
        self.state = 0
        self._step()
        self.state = (self.state + (seed & MASK64)) & MASK64
        self._step()

    def _step(self) -> None:
        self.state = (self.state * self._MULT + self._INC) & MASK64

    def next_u32(self) -> int:
        old = self.state
        self._step()
        xorshifted = (((old >> 18) ^ old) >> 27) & MASK32
        rot = old >> 59
        return ((xorshifted >> rot) | (xorshifted << ((-rot) & 31))) & MASK32

    def uniform(self) -> float:
        """Float in [0, 1) with 32 bits of entropy (exact in binary64)."""
        return self.next_u32() / 4294967296.0

    def range(self, lo: float, hi: float) -> float:
        return lo + (hi - lo) * self.uniform()

    def below(self, n: int) -> int:
        """Unbiased integer in [0, n) (Lemire-free rejection; n <= 2^32)."""
        if not 0 < n <= (1 << 32):
            raise ValueError("n must be in (0, 2^32]")
        threshold = ((1 << 32) - n) % n
        while True:
            r = self.next_u32()
            if r >= threshold:
                return r % n


class SeedStream:
    """A named random stream. `derive()` gives an independent child; `rng()` the draw generator."""

    def __init__(self, seed: int, path: tuple = ()):
        self.root_seed = seed
        self.path = tuple(path)
        self.seed = derive_seed(seed, *self.path)

    def derive(self, *parts: object) -> "SeedStream":
        return SeedStream(self.root_seed, self.path + tuple(parts))

    def rng(self) -> Pcg32:
        return Pcg32(self.seed)

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"SeedStream(seed={self.root_seed}, path={self.path!r})"
