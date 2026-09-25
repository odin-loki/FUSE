// FUSE Relight RL-4.4: the C++ dialect of the single-source light model (kernels/light_core.h). It supplies the
// HLSL-style vector types, the math overloads the core calls unqualified and the dialect macros, then includes the
// core inside namespace fuse::relight::lightk. Everything is FUSE_HOST_DEVICE inline and allocation-free, so the
// core is usable from compute_kernel bodies on every backend (docs/compute-kernels.md).
//
// Float semantics: every call forwards to the float overload of <cmath> (no double promotion) except the two
// RL-1.5 expressions upstream evaluates in double (FUSE_RL_STABLE_COS, FUSE_RL_DISCRIMINANT), so the CPU conversion
// is bit-identical to RL-1.5's convertLegacyLight; CpuReference and CpuParallel are bit-identical; against the GPU
// (Slang / GLSL twins of the same text) the differences are the device's transcendental and division rounding
// (gate: render/lights/tests).
#pragma once

#include <fuse/types.hpp>

#include <cmath>
#include <cstdint>

namespace fuse::relight::lightk {

using uint = std::uint32_t;

struct float3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    constexpr float3() = default;
    FUSE_HOST_DEVICE constexpr explicit float3(float s) : x(s), y(s), z(s) {}
    FUSE_HOST_DEVICE constexpr float3(float a, float b, float c) : x(a), y(b), z(c) {}
    FUSE_HOST_DEVICE constexpr float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    FUSE_HOST_DEVICE constexpr const float& operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

struct float4 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float w = 0.f;
    constexpr float4() = default;
    FUSE_HOST_DEVICE constexpr float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};

FUSE_HOST_DEVICE constexpr float3 operator+(const float3& a, const float3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
FUSE_HOST_DEVICE constexpr float3 operator-(const float3& a, const float3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
FUSE_HOST_DEVICE constexpr float3 operator*(const float3& a, const float3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
FUSE_HOST_DEVICE constexpr float3 operator*(const float3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
FUSE_HOST_DEVICE constexpr float3 operator*(float s, const float3& a) { return {s * a.x, s * a.y, s * a.z}; }
FUSE_HOST_DEVICE constexpr float3 operator/(const float3& a, float s) { return {a.x / s, a.y / s, a.z / s}; }
FUSE_HOST_DEVICE constexpr float3 operator-(const float3& a) { return {-a.x, -a.y, -a.z}; }

// Scalar math: float overloads only (hides the global double functions for unqualified calls in the core).
FUSE_HOST_DEVICE inline float sqrt(float x) { return std::sqrt(x); }
FUSE_HOST_DEVICE inline float pow(float x, float y) { return std::pow(x, y); }
FUSE_HOST_DEVICE inline float cos(float x) { return std::cos(x); }
FUSE_HOST_DEVICE inline float sin(float x) { return std::sin(x); }
FUSE_HOST_DEVICE inline float abs(float x) { return std::fabs(x); }
FUSE_HOST_DEVICE constexpr float min(float a, float b) { return b < a ? b : a; }
FUSE_HOST_DEVICE constexpr float max(float a, float b) { return a < b ? b : a; }
FUSE_HOST_DEVICE constexpr float clamp(float x, float lo, float hi) { return min(max(x, lo), hi); }

// Vector math.
FUSE_HOST_DEVICE constexpr float dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
FUSE_HOST_DEVICE inline float length(const float3& a) { return sqrt(dot(a, a)); }
FUSE_HOST_DEVICE inline float3 normalize(const float3& a) { return a * (1.f / length(a)); }
FUSE_HOST_DEVICE constexpr float3 cross(const float3& a, const float3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// RL-1.5 stableCos: cos in double, rounded once.
FUSE_HOST_DEVICE inline float stableCos(float x) { return static_cast<float>(std::cos(static_cast<double>(x))); }
/// Upstream `b * b - 4.0 * a * c` (a double expression, narrowed once).
FUSE_HOST_DEVICE inline float discriminantD(float a, float b, float c) {
    return static_cast<float>(static_cast<double>(b * b) - 4.0 * static_cast<double>(a) * static_cast<double>(c));
}

#define FUSE_RL_FN FUSE_HOST_DEVICE inline
#define FUSE_RL_CONST inline constexpr
#define FUSE_RL_OUT(T) T&
#define FUSE_RL_LIGHT_WORDS(name) const float4* name
#define FUSE_RL_D3D_WORDS(name) const float4* name
#define FUSE_RL_STABLE_COS(x) stableCos(x)
#define FUSE_RL_DISCRIMINANT(a, b, c) discriminantD((a), (b), (c))

#include "light_core.h"

#undef FUSE_RL_FN
#undef FUSE_RL_CONST
#undef FUSE_RL_OUT
#undef FUSE_RL_LIGHT_WORDS
#undef FUSE_RL_D3D_WORDS
#undef FUSE_RL_STABLE_COS
#undef FUSE_RL_DISCRIMINANT

/// Packs a record (rlLightWord 0..5).
FUSE_HOST_DEVICE inline void rlLightPack(const RlLight& L, float4* w) {
    for (uint i = 0; i < kRlLightWords; ++i) {
        w[i] = rlLightWord(L, i);
    }
}

} // namespace fuse::relight::lightk
