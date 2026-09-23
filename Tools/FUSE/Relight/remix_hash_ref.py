#!/usr/bin/env python3
"""FUSE Relight: independent Python reference for the Remix-compatible asset hashes.

Reference for docs/plans/FUSE_REMIX_PORT_PLAN.md section 4.1 (bit-exact with dxvk-remix @0867d3c),
checked against the C++ library Source/FUSE/Relight/hash by ctest rl_hash_py_parity. Written from
the specification, not transliterated from the C++ port:

  * xxHash: its own pure-Python XXH64 and XXH3_64bits (with seed); the `xxhash` wheel (BSD-2) is
    used instead when it is installed and `--impl` allows it (it is much faster);
  * the SSE4.1 legacy position discretisation: float32 products are formed exactly in double
    precision and rounded to binary32 with integer arithmetic (the C++ side emulates MULPS bit by
    bit instead); floor goes through math.floor;
  * the D3DFORMAT layout table is restated directly per D3D format.

Command line:
  remix_hash_ref.py verify FILE...       check case lines (outputs must match); exit 0/1
  remix_hash_ref.py vectors FILE         check the official xxHash vectors (xxhash_vectors.txt)
  remix_hash_ref.py xxh3|xxh64 HEX [--seed S]
  common option: --impl auto|pure|wheel  (default auto)

Case line format (shared with Source/FUSE/Relight/tests/hash/case_io.hpp): one case per line,
`<function> key=value ...`. Byte strings are `hex:<hex>`, `gen:<seed hex>:<len>` (splitmix64
stream, each 64-bit output little-endian) or `fgen:<seed hex>:<words>` (float bit patterns from the
same stream, see fgen_bytes). Functions and keys (inputs -> outputs):

  xxh64     d seed                          -> out
  xxh3      d seed|none                     -> out
  geomdesc  ic vc it topo                   -> out            hashGeometryDescriptor
  vlayout   p n t c (def:stream:stride:vk)  -> stride out     hashVertexLayout
  region    d stride esize size uniq        -> out            hashVertexRegionIndexed
  uniq      d isize count max               -> out            deduplicateSortIndices
  legidx    d isize count                   -> out            hashIndicesLegacy
  disc      v scale                         -> step inv out   discretize_SSE (one lane)
  legpos    d stride size scale h0 h1       -> out (h0,h1)    hashRegionLegacy
  vshader   bc f nf i ni b nb               -> out            vertex shader component
  rule      s                               -> out fmt id     createRule, canonical form, rule_id
  combine   f rule                          -> out def        getHashForRule(Impl)
  draw      prim pc itype idx vb pos tc n c vs [vs*] rule scale asset mat
            -> ok [f ic vc min max topo it ps key leg0 leg1 def0 def1]
  texlayout fmt w h d opt pitch src         -> info layout hash obs
  texdesc   w flags                         -> out            D3D9_COMMON_TEXTURE_DESC::CalculateHash
  light     type en f                       -> out            RtLight*::updateCachedHash
  hexfmt    h                               -> out opt mesh   hashToString
  parse     s                               -> opt prim       std::stoull / getNamedHash
"""

from __future__ import annotations

import argparse
import math
import struct
import sys

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1

# ---------------------------------------------------------------------------------------------------
# xxHash (pure Python, written from the xxHash specification)
# ---------------------------------------------------------------------------------------------------

P32_1 = 0x9E3779B1
P32_2 = 0x85EBCA77
P32_3 = 0xC2B2AE3D
P64_1 = 0x9E3779B185EBCA87
P64_2 = 0xC2B2AE3D27D4EB4F
P64_3 = 0x165667B19E3779F9
P64_4 = 0x85EBCA77C2B2AE63
P64_5 = 0x27D4EB2F165667C5
PRIME_MX1 = 0x165667919E3779F9
PRIME_MX2 = 0x9FB21C651E98DF25

XXH3_SECRET = bytes.fromhex(
    "b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671f"
    "cb79e64eccc0e578825ad07dccff7221b8084674f743248ee03590e6813a264c"
    "3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819ef46a9deacd8"
    "a8fa763fe39c343ff9dcbbc7c70b4f1d8a51e04bcdb45931c89f7ec9d9787364"
    "eac5ac8334d3ebc3c581a0fffa1363eb170ddd51b7f0da49d316552629d4689e"
    "2b16be587d47a1fc8ff8b8d17ad031ce45cb3a8f95160428afd7fbcabb4b407e")
assert len(XXH3_SECRET) == 192


def _r64(b, i):
    return int.from_bytes(b[i:i + 8], "little")


def _r32(b, i):
    return int.from_bytes(b[i:i + 4], "little")


def _rotl64(x, r):
    return ((x << r) | (x >> (64 - r))) & M64


def _xxh64_round(acc, lane):
    acc = (acc + lane * P64_2) & M64
    return (_rotl64(acc, 31) * P64_1) & M64


def _xxh64_avalanche(h):
    h ^= h >> 33
    h = (h * P64_2) & M64
    h ^= h >> 29
    h = (h * P64_3) & M64
    return h ^ (h >> 32)


def pure_xxh64(data: bytes, seed: int = 0) -> int:
    n = len(data)
    i = 0
    seed &= M64
    if n >= 32:
        v1 = (seed + P64_1 + P64_2) & M64
        v2 = (seed + P64_2) & M64
        v3 = seed
        v4 = (seed - P64_1) & M64
        while i + 32 <= n:
            a, b, c, d = struct.unpack_from("<4Q", data, i)
            v1 = _xxh64_round(v1, a)
            v2 = _xxh64_round(v2, b)
            v3 = _xxh64_round(v3, c)
            v4 = _xxh64_round(v4, d)
            i += 32
        h = (_rotl64(v1, 1) + _rotl64(v2, 7) + _rotl64(v3, 12) + _rotl64(v4, 18)) & M64
        for v in (v1, v2, v3, v4):
            h ^= _xxh64_round(0, v)
            h = (h * P64_1 + P64_4) & M64
    else:
        h = (seed + P64_5) & M64
    h = (h + n) & M64
    while i + 8 <= n:
        h ^= _xxh64_round(0, _r64(data, i))
        h = (_rotl64(h, 27) * P64_1 + P64_4) & M64
        i += 8
    if i + 4 <= n:
        h ^= (_r32(data, i) * P64_1) & M64
        h = (_rotl64(h, 23) * P64_2 + P64_3) & M64
        i += 4
    while i < n:
        h ^= (data[i] * P64_5) & M64
        h = (_rotl64(h, 11) * P64_1) & M64
        i += 1
    return _xxh64_avalanche(h)


def _xxh3_avalanche(h):
    h ^= h >> 37
    h = (h * PRIME_MX1) & M64
    return h ^ (h >> 32)


def _rrmxmx(h, length):
    h ^= _rotl64(h, 49) ^ _rotl64(h, 24)
    h = (h * PRIME_MX2) & M64
    h ^= (h >> 35) + length
    h = (h * PRIME_MX2) & M64
    return h ^ (h >> 28)


def _fold64(a, b):
    p = a * b
    return (p ^ (p >> 64)) & M64


def _swap32(x):
    return int.from_bytes(x.to_bytes(4, "little"), "big")


def _swap64(x):
    return int.from_bytes(x.to_bytes(8, "little"), "big")


def _mix16(data, i, secret, j, seed):
    lo = _r64(data, i)
    hi = _r64(data, i + 8)
    return _fold64(lo ^ ((_r64(secret, j) + seed) & M64), hi ^ ((_r64(secret, j + 8) - seed) & M64))


def _xxh3_long(data, secret):
    acc = [P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1]
    n = len(data)
    size = len(secret)
    stripes_per_block = (size - 64) // 8
    block_len = 64 * stripes_per_block
    blocks = (n - 1) // block_len

    def stripe(off, soff):
        lanes = struct.unpack_from("<8Q", data, off)
        keys = struct.unpack_from("<8Q", secret, soff)
        for k in range(8):
            v = lanes[k]
            key = v ^ keys[k]
            acc[k ^ 1] = (acc[k ^ 1] + v) & M64
            acc[k] = (acc[k] + (key & M32) * (key >> 32)) & M64

    for b in range(blocks):
        for s in range(stripes_per_block):
            stripe(b * block_len + s * 64, s * 8)
        keys = struct.unpack_from("<8Q", secret, size - 64)
        for k in range(8):
            a = acc[k]
            a ^= a >> 47
            a ^= keys[k]
            acc[k] = (a * P32_1) & M64
    last_stripes = ((n - 1) - block_len * blocks) // 64
    for s in range(last_stripes):
        stripe(blocks * block_len + s * 64, s * 8)
    stripe(n - 64, size - 64 - 7)
    result = (n * P64_1) & M64
    for k in range(4):
        result = (result + _fold64(acc[2 * k] ^ _r64(secret, 11 + 16 * k), acc[2 * k + 1] ^ _r64(secret, 11 + 16 * k + 8))) & M64
    return _xxh3_avalanche(result)


def pure_xxh3_64(data: bytes, seed: int = 0) -> int:
    n = len(data)
    seed &= M64
    s = XXH3_SECRET
    if n == 0:
        return _xxh64_avalanche(seed ^ (_r64(s, 56) ^ _r64(s, 64)))
    if n <= 3:
        combined = (data[0] << 16) | (data[n >> 1] << 24) | data[n - 1] | (n << 8)
        bitflip = ((_r32(s, 0) ^ _r32(s, 4)) + seed) & M64
        return _xxh64_avalanche(combined ^ bitflip)
    if n <= 8:
        seed2 = seed ^ (_swap32(seed & M32) << 32)
        bitflip = ((_r64(s, 8) ^ _r64(s, 16)) - seed2) & M64
        keyed = ((_r32(data, n - 4) + (_r32(data, 0) << 32)) & M64) ^ bitflip
        return _rrmxmx(keyed, n)
    if n <= 16:
        lo = _r64(data, 0) ^ (((_r64(s, 24) ^ _r64(s, 32)) + seed) & M64)
        hi = _r64(data, n - 8) ^ (((_r64(s, 40) ^ _r64(s, 48)) - seed) & M64)
        return _xxh3_avalanche((n + _swap64(lo) + hi + _fold64(lo, hi)) & M64)
    if n <= 128:
        acc = (n * P64_1) & M64
        pairs = [(0, n - 16)]
        if n > 32:
            pairs.append((16, n - 32))
        if n > 64:
            pairs.append((32, n - 48))
        if n > 96:
            pairs.append((48, n - 64))
        for k, (a, b) in enumerate(pairs):
            acc += _mix16(data, a, s, 32 * k, seed) + _mix16(data, b, s, 32 * k + 16, seed)
        return _xxh3_avalanche(acc & M64)
    if n <= 240:
        acc = (n * P64_1) & M64
        for k in range(8):
            acc += _mix16(data, 16 * k, s, 16 * k, seed)
        acc = _xxh3_avalanche(acc & M64)
        acc_end = _mix16(data, n - 16, s, 136 - 17, seed)
        for k in range(8, n // 16):
            acc_end += _mix16(data, 16 * k, s, 16 * (k - 8) + 3, seed)
        return _xxh3_avalanche((acc + acc_end) & M64)
    if seed == 0:
        return _xxh3_long(data, s)
    custom = b"".join(
        ((_r64(s, 16 * k) + seed) & M64).to_bytes(8, "little") + ((_r64(s, 16 * k + 8) - seed) & M64).to_bytes(8, "little")
        for k in range(12))
    return _xxh3_long(data, custom)


try:  # optional accelerator (BSD-2 wheel wrapping the reference C implementation)
    import xxhash as _xxhash_wheel  # type: ignore
except ImportError:  # pragma: no cover - depends on the environment
    _xxhash_wheel = None

_IMPL = "auto"


def set_impl(impl: str) -> None:
    global _IMPL
    if impl == "wheel" and _xxhash_wheel is None:
        raise SystemExit("remix_hash_ref: --impl wheel requested but the xxhash wheel is not installed")
    _IMPL = impl


def using_wheel() -> bool:
    return _xxhash_wheel is not None and _IMPL != "pure"


def xxh64(data: bytes, seed: int = 0) -> int:
    if using_wheel():
        return _xxhash_wheel.xxh64_intdigest(data, seed & M64)
    return pure_xxh64(data, seed)


def xxh3_64(data: bytes, seed: int = 0) -> int:
    if using_wheel():
        return _xxhash_wheel.xxh3_64_intdigest(data, seed & M64)
    return pure_xxh3_64(data, seed)


def u32le(v: int) -> bytes:
    return (v & M32).to_bytes(4, "little")


def u64le(v: int) -> bytes:
    return (v & M64).to_bytes(8, "little")


# ---------------------------------------------------------------------------------------------------
# binary32 arithmetic for the legacy position hash
# ---------------------------------------------------------------------------------------------------

def _is_nan(bits):
    return (bits & 0x7F800000) == 0x7F800000 and (bits & 0x007FFFFF) != 0


def _f32_value(bits):
    """Exact value of a non-NaN binary32 pattern as a Python float."""
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def _round_to_f32_bits(x: float) -> int:
    """Round a finite double to binary32 (nearest, ties to even; gradual underflow; overflow to inf)."""
    sign = 0x80000000 if math.copysign(1.0, x) < 0 else 0
    a = abs(x)
    if a == 0.0:
        return sign
    m, e = math.frexp(a)                 # a = m * 2**e, 0.5 <= m < 1
    mant = int(m * (1 << 53))            # exact 53-bit integer: a = mant * 2**(e - 53)
    exp = e - 1                          # a = 1.xxx * 2**exp
    if exp < -126:
        shift = 53 - (e + 149)           # quantum 2**-149
    else:
        shift = 53 - 24                  # keep 24 significant bits
    if shift >= 60:
        return sign                      # far below half the smallest subnormal
    q, r = mant >> shift, mant & ((1 << shift) - 1)
    half = 1 << (shift - 1)
    if r > half or (r == half and (q & 1)):
        q += 1
    if exp < -126:
        return sign | q                  # subnormal; a carry lands on the smallest normal encoding
    if q == (1 << 24):
        q >>= 1
        exp += 1
    if exp > 127:
        return sign | 0x7F800000
    return sign | ((exp + 127) << 23) | (q & 0x7FFFFF)


def f32_mul_bits(a: int, b: int) -> int:
    """MULSS on bit patterns with the x86 NaN rules (first NaN operand quieted; inf*0 -> 0xFFC00000)."""
    if _is_nan(a):
        return a | 0x00400000
    if _is_nan(b):
        return b | 0x00400000
    a_inf = (a & 0x7FFFFFFF) == 0x7F800000
    b_inf = (b & 0x7FFFFFFF) == 0x7F800000
    a_zero = (a & 0x7FFFFFFF) == 0
    b_zero = (b & 0x7FFFFFFF) == 0
    sign = (a ^ b) & 0x80000000
    if a_inf or b_inf:
        return 0xFFC00000 if (a_zero or b_zero) else sign | 0x7F800000
    if a_zero or b_zero:
        return sign
    return _round_to_f32_bits(_f32_value(a) * _f32_value(b))  # the double product is exact


def f32_floor_bits(bits: int) -> int:
    """ROUNDSS with _MM_FROUND_FLOOR."""
    if (bits & 0x7F800000) == 0x7F800000:
        return bits                      # inf / NaN pass through
    x = _f32_value(bits)
    if x == 0.0:
        return bits                      # keeps -0
    f = math.floor(x)
    if f == 0:
        return 0                         # 0 < x < 1 -> +0
    return struct.unpack("<I", struct.pack("<f", float(f)))[0]


def f32_bits(x: float) -> int:
    return _round_to_f32_bits(x) if math.isfinite(x) else struct.unpack("<I", struct.pack("<f", x))[0]


def legacy_step_bits(scale_bits: int):
    """0.01f * (100.f * sceneScale) and 1.f / step, each rounded to binary32."""
    meter = f32_mul_bits(0x42C80000, scale_bits)          # 100.f * sceneScale
    step = f32_mul_bits(0x3C23D70A, meter)                # 0.01f * meterToWorldUnitScale
    step_value = _f32_value(step) if not _is_nan(step) else float("nan")
    if step_value == 0.0:
        inv = 0x7F800000 | (step & 0x80000000)
    elif math.isinf(step_value) or math.isnan(step_value):
        inv = 0 if math.isinf(step_value) else step | 0x00400000
    else:
        inv = f32_bits(1.0 / step_value)                  # a correctly rounded double quotient rounds right
    return step, inv


def discretize_bits(value: int, step: int, inv: int) -> int:
    return f32_mul_bits(f32_floor_bits(f32_mul_bits(value, inv)), step)


# ---------------------------------------------------------------------------------------------------
# D3D enumerations
# ---------------------------------------------------------------------------------------------------

def fourcc(s: str) -> int:
    return s.encode()[0] | (s.encode()[1] << 8) | (s.encode()[2] << 16) | (s.encode()[3] << 24)


# D3DDECLTYPE -> (VkFormat, element bytes)
DECLTYPE = {
    0: (100, 4), 1: (103, 8), 2: (106, 12), 3: (109, 16), 4: (44, 4), 5: (39, 4), 6: (80, 4), 7: (94, 8),
    8: (37, 4), 9: (78, 4), 10: (92, 8), 11: (77, 4), 12: (91, 8), 13: (66, 4), 14: (65, 4), 15: (83, 4),
    16: (97, 8),
}

# D3DPRIMITIVETYPE -> (VkPrimitiveTopology, vertices for n primitives)
PRIMITIVE = {
    1: (0, lambda n: n), 2: (1, lambda n: 2 * n), 3: (2, lambda n: n + 1), 4: (3, lambda n: 3 * n),
    5: (4, lambda n: n + 2), 6: (5, lambda n: n + 2),
}


def primitive_info(prim: int, count: int):
    topo, verts = PRIMITIVE.get(prim, PRIMITIVE[4])
    return topo, verts(count) & M32


# D3DFORMAT -> (VkFormat of the mapping, element bytes, block w, block h, plane count). Formats
# absent here have no mapping; UNSUPPORTED_SIZE then gives GetUnsupportedFormatInfo's size.
_B = 1000156000
FORMATS = {
    21: (44, 4, 1, 1, 1), 22: (44, 4, 1, 1, 1),                  # A8R8G8B8, X8R8G8B8
    23: (4, 2, 1, 1, 1),                                          # R5G6B5
    24: (8, 2, 1, 1, 1), 25: (8, 2, 1, 1, 1),                    # X1R5G5B5, A1R5G5B5
    26: (1000340000, 2, 1, 1, 1), 30: (1000340000, 2, 1, 1, 1),  # A4R4G4B4, X4R4G4B4
    28: (9, 1, 1, 1, 1),                                          # A8
    31: (64, 4, 1, 1, 1),                                         # A2B10G10R10
    32: (37, 4, 1, 1, 1), 33: (37, 4, 1, 1, 1),                  # A8B8G8R8, X8B8G8R8
    34: (77, 4, 1, 1, 1),                                         # G16R16
    35: (58, 4, 1, 1, 1),                                         # A2R10G10B10
    36: (91, 8, 1, 1, 1),                                         # A16B16G16R16
    50: (9, 1, 1, 1, 1),                                          # L8
    51: (16, 2, 1, 1, 1),                                         # A8L8
    52: (1, 1, 1, 1, 1),                                          # A4L4
    60: (17, 2, 1, 1, 1),                                         # V8U8
    61: (5, 2, 1, 1, 1),                                          # L6V5U5 (converted)
    62: (44, 4, 1, 1, 1),                                         # X8L8V8U8 (converted)
    63: (38, 4, 1, 1, 1),                                         # Q8W8V8U8
    64: (78, 4, 1, 1, 1),                                         # V16U16
    65: (122, 4, 1, 1, 1),                                        # W11V11U10 (converted)
    67: (64, 4, 1, 1, 1),                                         # A2W10V10U10 (converted)
    fourcc("UYVY"): (44, 4, 1, 1, 1), fourcc("YUY2"): (44, 4, 1, 1, 1),
    fourcc("RGBG"): (_B, 4, 2, 1, 1), fourcc("GRGB"): (_B + 1, 4, 2, 1, 1),
    fourcc("DXT1"): (133, 8, 4, 4, 1),
    fourcc("DXT2"): (135, 16, 4, 4, 1), fourcc("DXT3"): (135, 16, 4, 4, 1),
    fourcc("DXT4"): (137, 16, 4, 4, 1), fourcc("DXT5"): (137, 16, 4, 4, 1),
    70: (124, 2, 1, 1, 1), 80: (124, 2, 1, 1, 1),                # D16_LOCKABLE, D16
    71: (126, 4, 1, 1, 1), 82: (126, 4, 1, 1, 1), 84: (126, 4, 1, 1, 1),  # D32, D32F_LOCKABLE, D32_LOCKABLE
    75: (129, 4, 1, 1, 1), 77: (129, 4, 1, 1, 1), 83: (129, 4, 1, 1, 1),  # D24S8, D24X8, D24FS8
    85: (127, 1, 1, 1, 1),                                        # S8_LOCKABLE
    81: (70, 2, 1, 1, 1),                                         # L16
    100: (13, 1, 1, 1, 1), 101: (74, 2, 1, 1, 1), 102: (98, 4, 1, 1, 1),  # VERTEXDATA, INDEX16, INDEX32
    110: (92, 8, 1, 1, 1),                                        # Q16W16V16U16
    111: (76, 2, 1, 1, 1), 112: (83, 4, 1, 1, 1), 113: (97, 8, 1, 1, 1),  # R16F, G16R16F, A16B16G16R16F
    114: (100, 4, 1, 1, 1), 115: (103, 8, 1, 1, 1), 116: (109, 16, 1, 1, 1),  # R32F, G32R32F, A32B32G32R32F
    119: (65, 4, 1, 1, 1),                                        # A2B10G10R10_XR_BIAS
    199: (13, 1, 1, 1, 1),                                        # BINARYBUFFER
    fourcc("ATI1"): (139, 8, 4, 4, 1), fourcc("ATI2"): (141, 16, 4, 4, 1),
    fourcc("DF24"): (129, 4, 1, 1, 1), fourcc("DF16"): (124, 2, 1, 1, 1),
    fourcc("INTZ"): (129, 4, 1, 1, 1),
    fourcc("NV12"): (9, 1, 1, 1, 2), fourcc("YV12"): (9, 1, 1, 1, 3),
}
UNSUPPORTED_SIZE = {20: 3, 27: 1, 29: 2, 40: 2, 41: 1, 61: 2, 62: 4, 67: 4, 117: 2}


def texture_format_info(fmt: int, opt: int):
    """(mapped, vkFormat, elementSize, blockW, blockH, planeCount); opt bits: x4r4g4b4, df, d32, d24s8."""
    entry = FORMATS.get(fmt)
    if entry is not None:
        if (fmt == 30 and not opt & 1) or (fmt in (fourcc("DF16"), fourcc("DF24")) and not opt & 2) or (fmt == 71 and not opt & 4):
            entry = None
    if entry is None:
        return (0, 0, UNSUPPORTED_SIZE.get(fmt, 0), 1, 1, 1)
    vk, size, bw, bh, planes = entry
    if vk == 129 and not opt & 8:
        vk, size = 130, 8  # D24_UNORM_S8_UINT unsupported -> D32_SFLOAT_S8_UINT
    return (1, vk, size, bw, bh, planes)


def texture_layout(fmt: int, w: int, h: int, d: int, opt: int):
    _, _, size, bw, bh, planes = texture_format_info(fmt, opt)
    w, h, d = max(1, w), max(1, h), max(1, d)
    blocks_w = -(-w // bw)
    blocks_h = -(-h // bh)
    planes = min(planes, 2)
    row_bytes = (size * blocks_w + 3) & ~3
    rows = planes * blocks_h * d
    return blocks_w, blocks_h, d, planes, row_bytes, rows, row_bytes * rows


# ---------------------------------------------------------------------------------------------------
# geometry
# ---------------------------------------------------------------------------------------------------

COMPONENTS = ["positions", "legacypositions0", "legacypositions1", "texcoords", "indices", "legacyindices",
              "geometrydescriptor", "vertexlayout", "vertexshader"]
POSITIONS, LEGACY0, LEGACY1, TEXCOORDS, INDICES, LEGACYIDX, GEOMDESC, VLAYOUT, VSHADER = range(9)
RULE_LEGACY0 = (1 << LEGACY0) | (1 << LEGACYIDX)
RULE_LEGACY1 = (1 << LEGACY1) | (1 << LEGACYIDX)
DEFAULT_ASSET_RULE = "positions,indices,geometrydescriptor"
DEFAULT_GENERATION_RULE = "positions,indices,texcoords,geometrydescriptor,vertexlayout,vertexshader"


def parse_rule(text: str) -> int:
    bits = 0
    for token in text.replace(" ", "").split(","):
        if token in COMPONENTS:
            bits |= 1 << COMPONENTS.index(token)
    return bits


def format_rule(bits: int) -> str:
    return ",".join(name for i, name in enumerate(COMPONENTS) if bits >> i & 1)


def combine(fields, rule: int) -> int:
    """GeometryHashes::getHashForRuleImpl: a zero running value takes the next field as-is."""
    h = 0
    for i in range(9):
        if rule >> i & 1:
            h = fields[i] if h == 0 else xxh64(u64le(fields[i]), h)
    return h


def rule_defined_upstream(fields, rule: int) -> bool:
    if rule == RULE_LEGACY0:
        return fields[LEGACY0] != 0
    if rule == RULE_LEGACY1:
        return fields[LEGACY1] != 0
    return True


def geometry_descriptor(ic, vc, it, topo) -> int:
    h = 0
    for v in (ic, vc, topo, it):
        h = xxh3_64(u32le(v), h)
    return h


def vertex_layout_stride(p, n, t, c) -> int:
    """Each element: (defined, stream, stride, vkFormat)."""
    interleaved = all(not e[0] or (e[1] == p[1] and e[2] == p[2]) for e in (n, t, c))
    friendly = (p[3] in (106, 109) and (not n[0] or n[3] in (106, 109, 98)) and (not t[0] or t[3] in (103, 106, 109))
                and (not c[0] or c[3] == 44))
    if interleaved and friendly:
        return p[2]
    return 12 + (12 if n[0] else 0) + (8 if t[0] else 0) + (4 if c[0] else 0)


def vertex_region(data: bytes, base: int, size: int, stride: int, esize: int, uniq) -> int:
    h = 0
    if uniq:
        for idx in uniq:
            off = base + idx * stride if esize else base
            h = xxh3_64(data[off:off + esize], h)
    elif stride:
        for off in range(0, size, stride):
            h = xxh3_64(data[base + off:base + off + esize], h)
    return h


def read_indices(data: bytes, isize: int, count: int):
    fmt = "<%d%s" % (count, "H" if isize == 2 else "I")
    return list(struct.unpack_from(fmt, data, 0))


def legacy_indices(data: bytes, isize: int, count: int) -> int:
    total = count * isize
    if total <= 1024:
        return xxh3_64(data[:total])
    step = (total // 512) & M32
    h = 0
    for i in range(0, count, step):
        h = xxh3_64(data[i * isize:(i + 1) * isize], h)
    return h


def legacy_positions(data: bytes, base: int, size: int, stride: int, scale_bits: int, h0: int, h1: int):
    if stride == 0:
        return h0, h1
    step, inv = legacy_step_bits(scale_bits)
    capture = min(size, 20 * stride)
    for off in range(0, size, stride):
        if off == capture:
            h0 = h1
        x, y, z = struct.unpack_from("<3I", data, base + off)
        h1 = xxh3_64(struct.pack("<3I", discretize_bits(x, step, inv), discretize_bits(y, step, inv),
                                 discretize_bits(z, step, inv)), h1)
    return h0, h1


def vertex_shader(bc: bytes, f: bytes, nf: int, i: bytes, ni: int, b: bytes, nb: int) -> int:
    h = xxh3_64(bc)
    h = xxh3_64(f[:nf * 16], h)
    h = xxh3_64(i[:ni * 16], h)
    return xxh3_64(b[:nb * 4 // 32], h)


def draw_hashes(prim, pc, itype, idx: bytes, vb: bytes, elements, vs, rule, scale_bits):
    """D3D9Rtx::prepareDrawGeometryForRT + computeHash. elements: dict name -> (off, stride, decl,
    stream) or None. vs: None or (bc, f, nf, i, ni, b, nb). Returns None for a skipped draw."""
    topo, count = primitive_info(prim, pc)
    pos = elements["pos"]
    if pos is None:
        return None
    indexed = itype in (0, 1)
    if indexed:
        isize = 2 if itype == 0 else 4
        values = read_indices(idx, isize, count)
        lo, hi = min(values), max(values)
        if lo == hi:
            return None
        rebased = [v - lo for v in values]
        rebased_bytes = struct.pack("<%d%s" % (count, "H" if isize == 2 else "I"), *rebased)
        ic, vc, it = count, hi - lo + 1, itype
        uniq = sorted(set(rebased))
    else:
        lo = hi = 0
        ic, vc, it = 0, count, 0   # an undefined index buffer reports VK_INDEX_TYPE_UINT16 (0)
        uniq = []
    if vc == 0:
        return None
    fields = [0] * 9

    def region(e):
        if e is None:
            return 0, 0, 0, 0
        off, stride, decl, _ = e
        return off + lo * stride, stride * vc, stride, DECLTYPE.get(decl, (0, 0))[1]

    if vs is not None and rule >> GEOMDESC & 1:
        fields[VSHADER] = vertex_shader(*vs)
    if rule >> GEOMDESC & 1:
        fields[GEOMDESC] = geometry_descriptor(ic, vc, it, topo)
    if rule >> VLAYOUT & 1:
        def layout(e):
            if e is None:
                return (False, 0, 0, 0)
            return (True, e[3], e[1], DECLTYPE.get(e[2], (0, 0))[0])
        fields[VLAYOUT] = xxh3_64(u64le(vertex_layout_stride(layout(pos), layout(elements["n"]), layout(elements["tc"]),
                                                              layout(elements["c"]))))
    if indexed:
        if rule >> INDICES & 1:
            fields[INDICES] = xxh3_64(rebased_bytes)
        if rule >> LEGACYIDX & 1:
            fields[LEGACYIDX] = legacy_indices(rebased_bytes, isize, count)
    if rule >> POSITIONS & 1:
        fields[POSITIONS] = vertex_region(vb, *region(pos), uniq)
    if rule >> TEXCOORDS & 1:
        fields[TEXCOORDS] = vertex_region(vb, *region(elements["tc"]), uniq)
    if rule >> LEGACY0 & 1 or rule >> LEGACY1 & 1:
        base, size, stride, _ = region(pos)
        fields[LEGACY0], fields[LEGACY1] = legacy_positions(vb, base, size, stride, scale_bits, 0, 0)
    return dict(fields=fields, ic=ic, vc=vc, min=lo, max=hi, topo=topo, it=it, ps=pos[1])


def legacy_key(d, rule, mat) -> int:
    h = combine(d["fields"], rule)
    for v in (d["ic"], d["vc"], d["topo"], d["ps"], d["it"]):
        h = xxh64(u32le(v), h)
    return h ^ mat


# ---------------------------------------------------------------------------------------------------
# lights, descriptors, strings
# ---------------------------------------------------------------------------------------------------

def _floats(bits_list) -> bytes:
    return b"".join(u32le(b) for b in bits_list)


def light_hash(kind: int, enabled: bool, f) -> int:
    def shaping(i):
        if not enabled:
            return 0
        h = xxh64(_floats(f[i:i + 3]), 0)
        for k in (3, 4, 5):
            h = xxh64(_floats([f[i + k]]), h)
        return h

    h = kind
    if kind == 0:
        for part in (f[0:3], f[3:4]):
            h = xxh64(_floats(part), h)
        return xxh64(u64le(h), shaping(4))
    if kind in (1, 2):
        for part in (f[0:3], f[3:5], f[5:8], f[8:11], f[11:14]):
            h = xxh64(_floats(part), h)
        return xxh64(u64le(h), shaping(14))
    if kind == 3:
        for part in (f[0:3], f[3:4], f[4:7], f[7:8]):
            h = xxh64(_floats(part), h)
        return h
    for part in (f[0:3], f[3:4]):
        h = xxh64(_floats(part), h)
    return h


def texture_descriptor(words, flags) -> int:
    data = b"".join(u32le(w) for w in words) + bytes([flags & 1, flags >> 1 & 1, flags >> 2 & 1, 0])
    return xxh3_64(data)


def hash_to_string(h: int) -> str:
    return "%016X" % h


_HEX = "0123456789abcdefABCDEF"


def windows_strtoull16(s: str):
    """(value, digits_found, overflow) of strtoull(s, &end, 16) as the Windows CRT evaluates it."""
    i = 0
    while i < len(s) and s[i] in " \t\n\v\f\r":
        i += 1
    neg = False
    if i < len(s) and s[i] in "+-":
        neg = s[i] == "-"
        i += 1
    if s[i:i + 2] in ("0x", "0X"):
        if i + 2 >= len(s) or s[i + 2] not in _HEX:
            return 0, False, False   # Windows: a bare prefix is no conversion at all
        i += 2
    j = i
    while j < len(s) and s[j] in _HEX:
        j += 1
    if j == i:
        return 0, False, False
    v = int(s[i:j], 16)
    if v > M64:
        return M64, True, True
    return ((-v) & M64 if neg else v), True, False


def parse_option_hash(s: str):
    v, found, overflow = windows_strtoull16(s)
    return None if (not found or overflow) else v


def prim_name_hash(name: str, prefix: str = "mesh_") -> int:
    if not name.startswith(prefix):
        return 0
    return windows_strtoull16(name[len(prefix):])[0]


# ---------------------------------------------------------------------------------------------------
# case lines
# ---------------------------------------------------------------------------------------------------

SPECIAL_FLOATS = [0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00001, 0x7F800001, 0x00000001,
                  0x807FFFFF, 0x7F7FFFFF, 0xFF7FFFFF, 0x00800000, 0x3F800000, 0xBF800000, 0x4B000000, 0x4B7FFFFF]


def _splitmix(seed: int):
    state = seed & M64
    while True:
        state = (state + 0x9E3779B97F4A7C15) & M64
        z = state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        yield z ^ (z >> 31)


def gen_bytes(seed: int, length: int) -> bytes:
    out = bytearray()
    g = _splitmix(seed)
    while len(out) < length:
        out += next(g).to_bytes(8, "little")
    return bytes(out[:length])


def fgen_bytes(seed: int, words: int) -> bytes:
    out = []
    g = _splitmix(seed)
    for i in range(words):
        if i % 2 == 0:
            v = next(g)
            w = v & M32
        else:
            w = v >> 32
        sel = w & 7
        if sel == 0:
            bits = SPECIAL_FLOATS[(w >> 3) % 16]
        elif sel == 1:
            bits = w
        else:
            bits = (((w >> 3) & 1) << 31) | ((117 + ((w >> 4) % 24)) << 23) | (w >> 9)
        out.append(bits)
    return struct.pack("<%dI" % words, *out)


def decode_bytes(spec: str) -> bytes:
    if spec.startswith("hex:"):
        return bytes.fromhex(spec[4:])
    kind, seed, count = spec.split(":")
    if kind == "gen":
        return gen_bytes(int(seed, 16), int(count))
    if kind == "fgen":
        return fgen_bytes(int(seed, 16), int(count))
    raise ValueError("bad byte spec " + spec[:20])


def h64(v: int) -> str:
    return "%016x" % (v & M64)


def h32(v: int) -> str:
    return "%08x" % (v & M32)


def _ints(text: str):
    return [] if text == "-" else [int(x) for x in text.split(",")]


def _element(text: str):
    if text == "-":
        return None
    off, stride, decl, stream = (int(x) for x in text.split(":"))
    return off, stride, decl, stream


def compute(fn: str, kv: dict) -> dict:
    """Outputs (as strings) for one case's inputs."""
    B = decode_bytes
    if fn == "xxh64":
        return {"out": h64(xxh64(B(kv["d"]), int(kv["seed"], 16)))}
    if fn == "xxh3":
        seed = 0 if kv["seed"] == "none" else int(kv["seed"], 16)
        return {"out": h64(xxh3_64(B(kv["d"]), seed))}
    if fn == "geomdesc":
        return {"out": h64(geometry_descriptor(int(kv["ic"]), int(kv["vc"]), int(kv["it"]), int(kv["topo"])))}
    if fn == "vlayout":
        els = [tuple(int(x) for x in kv[k].split(":")) for k in ("p", "n", "t", "c")]
        els = [(e[0] != 0, e[1], e[2], e[3]) for e in els]
        stride = vertex_layout_stride(*els)
        return {"stride": str(stride), "out": h64(xxh3_64(u64le(stride)))}
    if fn == "region":
        return {"out": h64(vertex_region(B(kv["d"]), 0, int(kv["size"]), int(kv["stride"]), int(kv["esize"]),
                                         _ints(kv["uniq"])))}
    if fn == "uniq":
        values = [v for v in read_indices(B(kv["d"]), int(kv["isize"]), int(kv["count"])) if v <= int(kv["max"])]
        u = sorted(set(values))
        return {"out": ",".join(str(x) for x in u) if u else "-"}
    if fn == "legidx":
        return {"out": h64(legacy_indices(B(kv["d"]), int(kv["isize"]), int(kv["count"])))}
    if fn == "disc":
        step, inv = legacy_step_bits(int(kv["scale"], 16))
        return {"step": h32(step), "inv": h32(inv), "out": h32(discretize_bits(int(kv["v"], 16), step, inv))}
    if fn == "legpos":
        h0, h1 = legacy_positions(B(kv["d"]), 0, int(kv["size"]), int(kv["stride"]), int(kv["scale"], 16),
                                  int(kv["h0"], 16), int(kv["h1"], 16))
        return {"out": h64(h0) + "," + h64(h1)}
    if fn == "vshader":
        return {"out": h64(vertex_shader(B(kv["bc"]), B(kv["f"]), int(kv["nf"]), B(kv["i"]), int(kv["ni"]), B(kv["b"]),
                                         int(kv["nb"])))}
    if fn == "rule":
        text = B(kv["s"]).decode("latin-1")
        bits = parse_rule(text) if text else 0
        canon = format_rule(bits)
        return {"out": str(bits), "fmt": "hex:" + canon.encode().hex(), "id": h64(xxh3_64(canon.encode()))}
    if fn == "combine":
        fields = [int(x, 16) for x in kv["f"].split(",")]
        rule = int(kv["rule"])
        return {"out": h64(combine(fields, rule)), "def": "1" if rule_defined_upstream(fields, rule) else "0"}
    if fn == "draw":
        elements = {k: _element(kv[k]) for k in ("pos", "tc", "n", "c")}
        vs = None
        if kv["vs"] == "1":
            vs = (B(kv["vsbc"]), B(kv["vsf"]), int(kv["vsnf"]), B(kv["vsi"]), int(kv["vsni"]), B(kv["vsb"]), int(kv["vsnb"]))
        d = draw_hashes(int(kv["prim"]), int(kv["pc"]), int(kv["itype"]), B(kv["idx"]), B(kv["vb"]), elements, vs,
                        int(kv["rule"]), int(kv["scale"], 16))
        if d is None:
            return {"ok": "0"}
        mat = int(kv["mat"], 16)
        f = d["fields"]
        return {"ok": "1", "f": ",".join(h64(x) for x in f), "ic": str(d["ic"]), "vc": str(d["vc"]), "min": str(d["min"]),
                "max": str(d["max"]), "topo": str(d["topo"]), "it": str(d["it"]), "ps": str(d["ps"]),
                "key": h64(combine(f, int(kv["asset"])) ^ mat), "leg0": h64(legacy_key(d, RULE_LEGACY0, mat)),
                "leg1": h64(legacy_key(d, RULE_LEGACY1, mat)),
                "def0": "1" if rule_defined_upstream(f, RULE_LEGACY0) else "0",
                "def1": "1" if rule_defined_upstream(f, RULE_LEGACY1) else "0"}
    if fn == "texlayout":
        fmt, opt = int(kv["fmt"]), int(kv["opt"])
        info = texture_format_info(fmt, opt)
        layout = texture_layout(fmt, int(kv["w"]), int(kv["h"]), int(kv["d"]), opt)
        row_bytes, rows = layout[4], layout[5]
        pitch = int(kv["pitch"])
        src = B(kv["src"])
        take = min(pitch, row_bytes)
        packed = b"".join(src[r * pitch:r * pitch + take] + bytes(row_bytes - take) for r in range(rows))
        return {"info": ",".join(str(x) for x in info), "layout": ",".join(str(x) for x in layout),
                "hash": h64(xxh3_64(packed)), "obs": h64(xxh64(packed, 0))}
    if fn == "texdesc":
        return {"out": h64(texture_descriptor([int(x) for x in kv["w"].split(",")], int(kv["flags"])))}
    if fn == "light":
        return {"out": h64(light_hash(int(kv["type"]), kv["en"] == "1", [int(x, 16) for x in kv["f"].split(",")]))}
    if fn == "hexfmt":
        h = int(kv["h"], 16)
        return {"out": hash_to_string(h), "opt": "0x" + hash_to_string(h), "mesh": "mesh_" + hash_to_string(h)}
    if fn == "parse":
        s = B(kv["s"]).decode("latin-1")
        opt = parse_option_hash(s)
        return {"opt": "none" if opt is None else h64(opt), "prim": h64(prim_name_hash(s))}
    raise KeyError(fn)


def parse_line(line: str):
    line = line.rstrip("\r\n")
    if not line or line.startswith("#"):
        return None
    parts = line.split(" ")
    kv = {}
    for token in parts[1:]:
        if token:
            k, _, v = token.partition("=")
            kv[k] = v
    return parts[0], kv


def verify_lines(lines, max_report: int = 20):
    """Returns (per-function counts, list of failure messages)."""
    counts, failures = {}, []
    for line in lines:
        parsed = parse_line(line)
        if parsed is None:
            continue
        fn, kv = parsed
        try:
            got = compute(fn, kv)
        except Exception as e:  # noqa: BLE001 - report malformed cases as failures
            failures.append("%s: exception %r in %s" % (fn, e, line[:200]))
            continue
        counts[fn] = counts.get(fn, 0) + 1
        for k, v in got.items():
            if kv.get(k) != v:
                if len(failures) < max_report:
                    failures.append("%s %s: case %s, reference %s\n    %s" % (fn, k, kv.get(k), v, line[:300]))
                else:
                    failures.append("")
    return counts, failures


def sanity_buffer(n: int = 4096 + 64 + 1) -> bytes:
    gen = 2654435761
    out = bytearray()
    for _ in range(n):
        out.append(gen >> 56)
        gen = (gen * 11400714785074694797) & M64
    return bytes(out)


def verify_vectors(path: str):
    buf = sanity_buffer()
    failures, count = [], 0
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if not line.strip() or line.startswith("#"):
                continue
            fn, n, seed, expected = line.split()
            data = buf[:int(n)]
            got = xxh64(data, int(seed, 16)) if fn == "xxh64" else xxh3_64(data, int(seed, 16))
            count += 1
            if got != int(expected, 16):
                failures.append("%s len %s seed %s: expected %s got %s" % (fn, n, seed, expected, h64(got)))
    return count, failures


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--impl", choices=("auto", "pure", "wheel"), default="auto")
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("verify")
    v.add_argument("files", nargs="+")
    vv = sub.add_parser("vectors")
    vv.add_argument("file")
    for name in ("xxh3", "xxh64"):
        p = sub.add_parser(name)
        p.add_argument("hex")
        p.add_argument("--seed", default="0")
    args = ap.parse_args(argv)
    set_impl(args.impl)
    if args.cmd in ("xxh3", "xxh64"):
        data = bytes.fromhex(args.hex)
        f = xxh3_64 if args.cmd == "xxh3" else xxh64
        print(h64(f(data, int(args.seed, 16))))
        return 0
    if args.cmd == "vectors":
        count, failures = verify_vectors(args.file)
        for f in failures[:20]:
            print("FAIL:", f)
        print("xxHash vectors: %d checked (%s xxHash)" % (count, "wheel" if using_wheel() else "pure-Python"))
        return 1 if failures else 0
    status = 0
    for path in args.files:
        with open(path, encoding="utf-8") as fh:
            counts, failures = verify_lines(fh)
        for f in [f for f in failures if f][:20]:
            print("FAIL:", f)
        print("%s: %s -> %s" % (path, " ".join("%s=%d" % kv for kv in sorted(counts.items())),
                                "%d failure(s)" % len(failures) if failures else "all match"))
        status |= 1 if failures or not counts else 0
    return status


if __name__ == "__main__":
    sys.exit(main())
