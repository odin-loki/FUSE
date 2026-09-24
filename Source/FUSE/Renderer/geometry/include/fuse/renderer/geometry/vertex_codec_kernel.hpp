#pragma once

// WP-1.2 vertex compression, single-source (docs/compute-kernels.md): the same bodies run serially
// (CpuReference), over the job system (CpuParallel) and, later, on the GPU. One vertex per item.
//
// Encoded vertex (16 bytes, four SoA streams; see meshlet_format.hpp for the chunks):
//   VPOS  u16 x, y, z  quantised position (QuantParams, per mesh and axis), u16 w: bit 0 = tangent.w < 0
//   VNRM  u32          octahedral normal, snorm16 x in bits 0..15, snorm16 y in bits 16..31
//   VTAN  u32          octahedral tangent (xyz), same packing; handedness lives in VPOS.w bit 0
//   VUV0  u32          uv0 as IEEE half: u in bits 0..15, v in bits 16..31
//
// Dequantisation parity (what a GPU decoder must do to match these CPU references):
//   position  p = offset + float(q) * step                  bit-exact everywhere: step is 2^e and
//             offset a multiple of step with |offset/step| + 65535 < 2^24, so nothing rounds (and a
//             contracted FMA gives the same bits).
//   uv        unpackHalf2x16(VUV0)                           bit-exact: the encoder never emits a
//             subnormal half (values below 2^-14 become 0 or 2^-14), so denormal flushing is moot.
//   oct       o = clamp(float(int16(q)) * kOctInvScale, -1, 1)  bit-exact: one correctly rounded
//             multiply by the same f32 constant (Vulkan: OpFMul is correctly rounded), then the
//             exact fold z = 1 - |x| - |y|, t = max(-z, 0), x += x >= 0 ? -t : t (same for y).
//   normal    normalize(o)                                   NOT bit-exact on GPUs (inversesqrt is
//             2-4 ulp there); parity tolerance 1e-6 absolute per component. Everything before the
//             normalize is bit-exact, so shaders that only need a direction can skip it.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/geometry/meshlet_types.hpp>

#include <cmath>
#include <cstring>

namespace fuse::renderer::geometry::vertex_codec {

inline constexpr const char* kEncodeName = "geometry_vertex_encode";
inline constexpr const char* kDecodeName = "geometry_vertex_decode";
inline constexpr u32 kWorkgroup = 128u;

inline constexpr f32 kOctScale = 32767.f;
/// The f32 constant a GPU decoder multiplies by (0x38000100); division is not correctly rounded there.
inline constexpr f32 kOctInvScale = 1.f / 32767.f;
inline constexpr u32 kQuantMax = 65535u;

FUSE_HOST_DEVICE inline u32 float_bits(f32 v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

FUSE_HOST_DEVICE inline f32 bits_float(u32 bits) {
    f32 v = 0.f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

/// f32 -> IEEE half, round to nearest even, never subnormal: |v| < 2^-15 -> signed zero,
/// 2^-15 <= |v| < 2^-14 -> +-2^-14. NaN -> quiet NaN, overflow (>= 65520) -> infinity.
FUSE_HOST_DEVICE inline u16 float_to_half(f32 v) {
    const u32 x = float_bits(v);
    const u32 sign = (x >> 16) & 0x8000u;
    const u32 ax = x & 0x7FFFFFFFu;
    if (ax > 0x7F800000u) {
        return static_cast<u16>(sign | 0x7E00u);
    }
    if (ax >= 0x477FF000u) {
        return static_cast<u16>(sign | 0x7C00u);
    }
    if (ax < 0x38800000u) {                                           // below 2^-14
        return static_cast<u16>(sign | (ax >= 0x38000000u ? 0x0400u : 0u)); // 2^-15 threshold
    }
    u32 h = (ax - 0x38000000u) >> 13;
    const u32 rem = ax & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u) != 0u)) {
        ++h;
    }
    return static_cast<u16>(sign | h);
}

/// IEEE half -> f32 (exact; matches GLSL unpackHalf2x16).
FUSE_HOST_DEVICE inline f32 half_to_float(u16 h) {
    const u32 sign = static_cast<u32>(h & 0x8000u) << 16;
    const u32 e = (h >> 10) & 0x1Fu;
    const u32 m = h & 0x3FFu;
    if (e == 0u) {
        const f32 mag = static_cast<f32>(m) * 5.9604644775390625e-8f; // m * 2^-24, exact
        return bits_float(sign | float_bits(mag));
    }
    if (e == 31u) {
        return bits_float(sign | 0x7F800000u | (m << 13));
    }
    return bits_float(sign | ((e + 112u) << 23) | (m << 13));
}

/// Quantise one coordinate: nearest q in 0..65535 to (p - offset) / 2^exponent.
FUSE_HOST_DEVICE inline u16 quantize_position(f32 p, f32 offset, s32 exponent) {
    const f64 t = (static_cast<f64>(p) - static_cast<f64>(offset)) * std::ldexp(1.0, -exponent);
    const f64 r = std::floor(t + 0.5);
    if (!(r > 0.0)) {
        return 0u;
    }
    return r >= static_cast<f64>(kQuantMax) ? static_cast<u16>(kQuantMax) : static_cast<u16>(r);
}

/// The dequantisation contract (exact, see the header comment).
FUSE_HOST_DEVICE inline f32 dequantize_position(u32 q, f32 offset, f32 step) { return offset + static_cast<f32>(q) * step; }

FUSE_HOST_DEVICE inline f32 snorm16_to_float(u32 bits16) {
    const s32 q = static_cast<s32>(bits16 & 0xFFFFu) - ((bits16 & 0x8000u) != 0u ? 0x10000 : 0);
    const f32 v = static_cast<f32>(q) * kOctInvScale;
    return v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
}

/// Octahedral decode without the final normalisation (bit-exact part of the contract).
FUSE_HOST_DEVICE inline void oct_decode_raw(u32 packed, f32 out[3]) {
    f32 x = snorm16_to_float(packed);
    f32 y = snorm16_to_float(packed >> 16);
    const f32 z = 1.f - std::fabs(x) - std::fabs(y);
    const f32 t = z < 0.f ? -z : 0.f;
    x += x >= 0.f ? -t : t;
    y += y >= 0.f ? -t : t;
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

FUSE_HOST_DEVICE inline void oct_decode(u32 packed, f32 out[3]) {
    oct_decode_raw(packed, out);
    const f32 len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    const f32 inv = len > 0.f ? 1.f / len : 0.f;
    out[0] *= inv;
    out[1] *= inv;
    out[2] *= inv;
}

FUSE_HOST_DEVICE inline u32 pack_snorm16_pair(s32 qx, s32 qy) {
    return (static_cast<u32>(qx) & 0xFFFFu) | ((static_cast<u32>(qy) & 0xFFFFu) << 16);
}

/// Octahedral encode, "precise" variant: of the four floor/ceil snorm16 candidates, keep the one
/// whose decoded direction is closest to the input. A zero vector encodes +Z.
FUSE_HOST_DEVICE inline u32 oct_encode(f32 x, f32 y, f32 z) {
    const f32 l1 = std::fabs(x) + std::fabs(y) + std::fabs(z);
    if (!(l1 > 0.f)) {
        return 0u;
    }
    f32 u = x / l1;
    f32 v = y / l1;
    if (z < 0.f) {
        const f32 fu = (1.f - std::fabs(v)) * (u >= 0.f ? 1.f : -1.f);
        const f32 fv = (1.f - std::fabs(u)) * (v >= 0.f ? 1.f : -1.f);
        u = fu;
        v = fv;
    }
    // Candidates are ranked in f64: neighbouring codes differ by ~1e-9 in dot product, below f32
    // resolution, and ties in f32 would break decode -> encode idempotence.
    const f64 l2 = std::sqrt(static_cast<f64>(x) * x + static_cast<f64>(y) * y + static_cast<f64>(z) * z);
    const f64 nx = x / l2;
    const f64 ny = y / l2;
    const f64 nz = z / l2;
    const f32 su = std::floor(u * kOctScale);
    const f32 sv = std::floor(v * kOctScale);
    s32 bestX = 0;
    s32 bestY = 0;
    f64 bestDot = -2.0;
    for (u32 c = 0; c < 4u; ++c) {
        s32 qx = static_cast<s32>(su) + static_cast<s32>(c & 1u);
        s32 qy = static_cast<s32>(sv) + static_cast<s32>(c >> 1);
        qx = qx < -32767 ? -32767 : (qx > 32767 ? 32767 : qx);
        qy = qy < -32767 ? -32767 : (qy > 32767 ? 32767 : qy);
        const u32 packed = pack_snorm16_pair(qx, qy);
        f32 d[3];
        oct_decode_raw(packed, d); // exact part of the decode; normalise in f64
        const f64 dl = std::sqrt(static_cast<f64>(d[0]) * d[0] + static_cast<f64>(d[1]) * d[1] + static_cast<f64>(d[2]) * d[2]);
        const f64 dot = (d[0] * nx + d[1] * ny + d[2] * nz) / dl;
        if (dot > bestDot) {
            bestDot = dot;
            bestX = qx;
            bestY = qy;
        }
    }
    // Canonical form: on the folded edges (one coordinate at +-32767) the codes (+-a, b) or (a, +-b)
    // decode to the same direction; keep the non-negative twin so decode -> encode is the identity.
    if (bestY == 32767 || bestY == -32767) {
        bestX = bestX < 0 ? -bestX : bestX;
    }
    if (bestX == 32767 || bestX == -32767) {
        bestY = bestY < 0 ? -bestY : bestY;
    }
    return pack_snorm16_pair(bestX, bestY);
}

struct EncodeParams {
    kernel::Span<const f32> positions; ///< xyz per vertex
    kernel::Span<const f32> normals;   ///< xyz per vertex (any length; zero encodes +Z)
    kernel::Span<const f32> tangents;  ///< xyzw per vertex (w sign = handedness)
    kernel::Span<const f32> uvs;       ///< uv per vertex
    QuantParams quant{};
    kernel::Span<u16> out_pos; ///< 4 per vertex
    kernel::Span<u32> out_nrm;
    kernel::Span<u32> out_tan;
    kernel::Span<u32> out_uv;
};

FUSE_HOST_DEVICE inline void encode_vertex(const EncodeParams& p, u32 v) {
    for (u32 a = 0; a < 3u; ++a) {
        p.out_pos[v * 4u + a] = quantize_position(p.positions[v * 3u + a], p.quant.offset[a], p.quant.exponent[a]);
    }
    p.out_pos[v * 4u + 3u] = p.tangents[v * 4u + 3u] < 0.f ? kVposTangentNegative : u16{0};
    p.out_nrm[v] = oct_encode(p.normals[v * 3u], p.normals[v * 3u + 1u], p.normals[v * 3u + 2u]);
    p.out_tan[v] = oct_encode(p.tangents[v * 4u], p.tangents[v * 4u + 1u], p.tangents[v * 4u + 2u]);
    p.out_uv[v] = static_cast<u32>(float_to_half(p.uvs[v * 2u])) |
                  (static_cast<u32>(float_to_half(p.uvs[v * 2u + 1u])) << 16);
}

struct EncodeKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const EncodeParams& p) const {
        encode_vertex(p, idx.linear);
    }
};

struct DecodeParams {
    kernel::Span<const u16> pos; ///< 4 per vertex
    kernel::Span<const u32> nrm;
    kernel::Span<const u32> tan;
    kernel::Span<const u32> uv;
    QuantParams quant{};
    kernel::Span<f32> out_positions; ///< xyz
    kernel::Span<f32> out_normals;   ///< xyz, normalised
    kernel::Span<f32> out_tangents;  ///< xyzw, w = +-1
    kernel::Span<f32> out_uvs;       ///< uv
};

FUSE_HOST_DEVICE inline void decode_vertex(const DecodeParams& p, u32 v) {
    for (u32 a = 0; a < 3u; ++a) {
        p.out_positions[v * 3u + a] = dequantize_position(p.pos[v * 4u + a], p.quant.offset[a], p.quant.step[a]);
    }
    f32 n[3];
    oct_decode(p.nrm[v], n);
    f32 t[3];
    oct_decode(p.tan[v], t);
    for (u32 a = 0; a < 3u; ++a) {
        p.out_normals[v * 3u + a] = n[a];
        p.out_tangents[v * 4u + a] = t[a];
    }
    p.out_tangents[v * 4u + 3u] = (p.pos[v * 4u + 3u] & kVposTangentNegative) != 0u ? -1.f : 1.f;
    p.out_uvs[v * 2u] = half_to_float(static_cast<u16>(p.uv[v] & 0xFFFFu));
    p.out_uvs[v * 2u + 1u] = half_to_float(static_cast<u16>(p.uv[v] >> 16));
}

struct DecodeKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const DecodeParams& p) const {
        decode_vertex(p, idx.linear);
    }
};

inline kernel::KernelLaunch make_encode_launch(u32 vertex_count) {
    return kernel::KernelLaunch{kEncodeName, kernel::extent1(vertex_count), {kWorkgroup, 1u, 1u}};
}

inline kernel::KernelLaunch make_decode_launch(u32 vertex_count) {
    return kernel::KernelLaunch{kDecodeName, kernel::extent1(vertex_count), {kWorkgroup, 1u, 1u}};
}

} // namespace fuse::renderer::geometry::vertex_codec
