#pragma once

// B1 row "GRIA Alpha evaluates correctly on both host and CUDA device kernel": one FUSE_HOST_DEVICE
// evaluation of every fuse::Alpha operation over a fixed input sweep (clamping edges, NaN/inf, the
// exact / chaos-edge / approximate thresholds, entropy ratios, blends). The C++23 host TU, nvcc's host
// pass and a __global__ kernel all run evaluateAlpha(); fuse_cuda_shared_headers_gate compares them.

#include <fuse/gria.hpp>
#include <fuse/types.hpp>

#include <cstring>
#include <limits>
#include <type_traits>

namespace fuse::cuda_gate {

struct AlphaInput {
    f32 value;
    f32 h_output;
    f32 h_input;
    f32 exact;
    f32 approximate;
};
static_assert(std::is_trivially_copyable_v<AlphaInput> && sizeof(AlphaInput) == 20);

struct AlphaRecord {
    f32 get;          ///< Alpha(value).get()
    f32 entropy;      ///< Alpha::from_entropy_ratio(h_output, h_input).get()
    f32 blend;        ///< Alpha(value).blend(exact, approximate)
    f32 default_get;  ///< Alpha{}.get() (0)
    u32 flags;        ///< bit0 is_exact, bit1 is_approximate, bit2 at_edge_of_chaos, bit3 == Alpha(get),
                      ///< bit4 == ALPHA_EXACT, bit5 == ALPHA_CHAOS_EDGE, bit6 == ALPHA_APPROXIMATE
};
static_assert(std::is_trivially_copyable_v<AlphaRecord> && sizeof(AlphaRecord) == 20);

FUSE_HOST_DEVICE inline AlphaRecord evaluateAlpha(const AlphaInput& in) {
    const Alpha a(in.value);
    const Alpha e = Alpha::from_entropy_ratio(in.h_output, in.h_input);
    AlphaRecord r{};
    r.get = a.get();
    r.entropy = e.get();
    r.blend = a.blend(in.exact, in.approximate);
    r.default_get = Alpha{}.get();
    r.flags = (a.is_exact() ? 1u : 0u) | (a.is_approximate() ? 2u : 0u) | (a.at_edge_of_chaos() ? 4u : 0u) |
              (a == Alpha(a.get()) ? 8u : 0u) | (a == Alpha::exact() ? 16u : 0u) |
              (a == Alpha::chaos_edge() ? 32u : 0u) | (a == Alpha::approximate() ? 64u : 0u);
    return r;
}

// The host constants and the device-callable functions are the same values.
static_assert(ALPHA_EXACT == Alpha::exact() && ALPHA_CHAOS_EDGE == Alpha::chaos_edge() &&
              ALPHA_APPROXIMATE == Alpha::approximate());

inline constexpr u32 kAlphaSweepCount = 4096;

/// Deterministic sweep: the first entries hit every threshold / clamp edge and non-finite input, the
/// rest cover [-0.5, 1.5] with varied entropy pairs and blend endpoints.
inline AlphaInput alphaSweepInput(u32 i) {
    constexpr f32 kInf = std::numeric_limits<f32>::infinity();
    const f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
    const f32 special[] = {0.f,     -0.f,       1.f,        -1.f,        2.f,         0.01f,      0.00999f,
                           0.0101f, 0.49f,      0.4901f,    0.5f,        0.5099f,     0.51f,      0.99f,
                           0.9901f, 1.0001f,    kInf,       -kInf,       kNaN,        1e-45f,     1e30f,
                           0.25f,   0.75f,      0.999999f,  1e-7f,       0.33333334f, 0.66666669f};
    constexpr u32 kSpecial = static_cast<u32>(sizeof(special) / sizeof(special[0]));
    AlphaInput in{};
    in.value = i < kSpecial ? special[i] : -0.5f + 2.f * static_cast<f32>(i - kSpecial) / static_cast<f32>(kAlphaSweepCount);
    const u32 h = i * 2654435761u;
    in.h_input = (i % 7u == 0u) ? 0.f : static_cast<f32>((h >> 8) % 1000u) * 0.01f;       // 0 = exact by definition
    in.h_output = (i % 11u == 0u) ? in.h_input * 1.5f : static_cast<f32>((h >> 16) % 1000u) * 0.01f;
    in.exact = static_cast<f32>(static_cast<int>(h % 201u) - 100) * 0.37f;
    in.approximate = static_cast<f32>(static_cast<int>((h >> 4) % 201u) - 100) * 0.53f;
    return in;
}

/// Bitwise float equality that treats every NaN as equal (none are expected: Alpha clamps NaN to 0).
inline bool sameBits(f32 a, f32 b) {
    u32 ua = 0;
    u32 ub = 0;
    std::memcpy(&ua, &a, sizeof(ua));
    std::memcpy(&ub, &b, sizeof(ub));
    return ua == ub || (a != a && b != b);
}

} // namespace fuse::cuda_gate
