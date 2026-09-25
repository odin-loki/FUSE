// FUSE Relight RL-4.3: the C++ dialect of the single-source BSDF (kernels/bsdf_core.h). It supplies the HLSL-style
// vector types, the math overloads the core calls unqualified and the dialect macros, then includes the core inside
// namespace fuse::relight::bsdf. Everything is FUSE_HOST_DEVICE inline and allocation-free, so the core is usable
// from compute_kernel bodies on every backend (docs/compute-kernels.md).
//
// Float semantics: every call below forwards to the float overload of <cmath> (no double promotion), so the
// CpuReference and CpuParallel backends are bit-identical; against the GPU (Slang / GLSL twins of the same text) the
// differences are the device's transcendental and division rounding (gate: <= 1e-4, render/material/tests).
#pragma once

#include <fuse/types.hpp>

#include <cmath>
#include <cstdint>

namespace fuse::relight::bsdf {

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
FUSE_HOST_DEVICE constexpr float3 operator/(const float3& a, const float3& b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
FUSE_HOST_DEVICE constexpr float3 operator*(const float3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
FUSE_HOST_DEVICE constexpr float3 operator*(float s, const float3& a) { return {s * a.x, s * a.y, s * a.z}; }
FUSE_HOST_DEVICE constexpr float3 operator/(const float3& a, float s) { return {a.x / s, a.y / s, a.z / s}; }
FUSE_HOST_DEVICE constexpr float3 operator-(const float3& a) { return {-a.x, -a.y, -a.z}; }

// Scalar math: float overloads only (hides the global double functions for unqualified calls in the core).
FUSE_HOST_DEVICE inline float sqrt(float x) { return std::sqrt(x); }
FUSE_HOST_DEVICE inline float exp(float x) { return std::exp(x); }
FUSE_HOST_DEVICE inline float log(float x) { return std::log(x); }
FUSE_HOST_DEVICE inline float pow(float x, float y) { return std::pow(x, y); }
FUSE_HOST_DEVICE inline float cos(float x) { return std::cos(x); }
FUSE_HOST_DEVICE inline float sin(float x) { return std::sin(x); }
FUSE_HOST_DEVICE inline float floor(float x) { return std::floor(x); }
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
FUSE_HOST_DEVICE constexpr float3 max(const float3& a, const float3& b) { return {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)}; }
FUSE_HOST_DEVICE constexpr float3 min(const float3& a, const float3& b) { return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)}; }

#define FUSE_BSDF_FN FUSE_HOST_DEVICE inline
#define FUSE_BSDF_CONST inline constexpr
#define FUSE_BSDF_OUT(T) T&
#define FUSE_BSDF_LUT_PARAM const float *lut,
#define FUSE_BSDF_LUT_ARG lut,
#define FUSE_BSDF_LUT_READ(i) lut[(i)]
#define FUSE_BSDF_WORDS_PARAM(name) const float4* name

#include "bsdf_core.h"

#undef FUSE_BSDF_FN
#undef FUSE_BSDF_CONST
#undef FUSE_BSDF_OUT
#undef FUSE_BSDF_LUT_PARAM
#undef FUSE_BSDF_LUT_ARG
#undef FUSE_BSDF_LUT_READ
#undef FUSE_BSDF_WORDS_PARAM

/// The packed GPU layout of a BsdfMaterial (inverse of the core's bsdfMaterialUnpack).
FUSE_HOST_DEVICE inline void bsdfMaterialPack(const BsdfMaterial& m, float4* w) {
    w[0] = {float(m.model), float(m.flags), float(m.diffuseModel), m.opacity};
    w[1] = {m.albedo.x, m.albedo.y, m.albedo.z, m.roughness};
    w[2] = {m.anisotropy, m.metallic, m.ior, m.thinFilmThickness};
    w[3] = {m.transmittance.x, m.transmittance.y, m.transmittance.z, m.mediumDistance};
    w[4] = {m.layerColor.x, m.layerColor.y, m.layerColor.z, m.layerOpacity};
    w[5] = {m.sssTransmittance.x, m.sssTransmittance.y, m.sssTransmittance.z, m.sssMeasurementDistance};
    w[6] = {m.sssSingleScatterAlbedo.x, m.sssSingleScatterAlbedo.y, m.sssSingleScatterAlbedo.z, m.sssAnisotropy};
    w[7] = {m.sssRadius.x, m.sssRadius.y, m.sssRadius.z, m.hairBetaM};
    w[8] = {m.hairBetaN, m.hairAlpha, m.hairH, 0.f};
    w[9] = {m.hairSigmaA.x, m.hairSigmaA.y, m.hairSigmaA.z, 0.f};
    w[10] = {m.emission.x, m.emission.y, m.emission.z, 0.f};
}

/// A material with every field set (the core struct has no default member initializers, being shared with the
/// shader dialects): an opaque, white, half-rough Lambert dielectric.
FUSE_HOST_DEVICE inline BsdfMaterial bsdfMaterialDefault() {
    BsdfMaterial m{};
    m.model = kBsdfModelOpaque;
    m.flags = 0u;
    m.diffuseModel = kBsdfDiffuseLambert;
    m.opacity = 1.f;
    m.albedo = float3(1.f);
    m.roughness = 0.5f;
    m.anisotropy = 0.f;
    m.metallic = 0.f;
    m.ior = 1.5f;
    m.thinFilmThickness = 0.f;
    m.transmittance = float3(1.f);
    m.mediumDistance = 1.f;
    m.layerColor = float3(0.f);
    m.layerOpacity = 0.f;
    m.sssTransmittance = float3(0.5f);
    m.sssMeasurementDistance = 0.f;
    m.sssSingleScatterAlbedo = float3(0.5f);
    m.sssAnisotropy = 0.f;
    m.sssRadius = float3(0.5f);
    m.hairBetaM = 0.3f;
    m.hairBetaN = 0.3f;
    m.hairAlpha = 0.f;
    m.hairH = 0.f;
    m.hairSigmaA = float3(0.f);
    m.emission = float3(0.f);
    return m;
}

} // namespace fuse::relight::bsdf
