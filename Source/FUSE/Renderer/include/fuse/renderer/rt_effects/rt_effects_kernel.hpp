#pragma once

// WP-6.2 ray-traced shadows and reflections: the math shared by the kernels (shaders/rt_effects/
// rtfx_common.{glsl,slang} are line-for-line twins), the CPU mirror the gates use to re-derive the rays
// the GPU dumped, and (instantiated with f64) the offline reference (rt_effects_reference.hpp).
//
// Sequence: Owen-scrambled Sobol (Burley 2020, "Practical Hash-based Owen Scrambling"): the first two
// Sobol dimensions (a (0, 2)-sequence in base 2), index shuffled and each dimension scrambled by the
// Laine-Karras hash in bit-reversed order, seeded per (pixel, light / reflection stream, run seed).
// Sample n of a pixel = frameIndex * samples + s, so every frame continues the pixel's sequence and the
// first 2^m samples of it are a (0, m, 2)-net (fuse_rp_rt_effects_sequence checks it). Integer-only up
// to the final exact 24-bit float conversion: CPU and GPU draw bit-identical (u, v).
//
// Sampling (the estimators' definitions; the converged reference integrates the same quantities):
//   shadows      visibility = fraction of the light the ray origin sees: rectangle / disk uniform by
//                area, sun disk uniform by solid angle inside the cone, punctual kinds one ray;
//   reflections  GGX visible-normal sampling (Dupuy and Benyoub 2023, spherical caps) at alpha =
//                roughness^2 around the G-buffer normal, a perfect mirror below `mirrorRoughness`;
//                the output is the lobe-averaged incoming radiance (directions reflected below the
//                surface contribute 0).
// Ray origins: P + N (normalBias + viewBias |P - camera|) with P reconstructed from the G-buffer depth
// through the inverse view-projection (Vulkan clip, forward z/w) and N the decoded RT0 normal.
//
// Device-safe: <cmath> and <fuse/types.hpp> only.

#include <fuse/renderer/rt_effects/rt_effects_types.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer::rt_effects {

template <typename T>
struct RtfxVec3 {
    T x = T(0);
    T y = T(0);
    T z = T(0);
};

template <typename T>
inline RtfxVec3<T> rtfxAdd(const RtfxVec3<T>& a, const RtfxVec3<T>& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
template <typename T>
inline RtfxVec3<T> rtfxSub(const RtfxVec3<T>& a, const RtfxVec3<T>& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
template <typename T>
inline RtfxVec3<T> rtfxScale(const RtfxVec3<T>& a, T s) {
    return {a.x * s, a.y * s, a.z * s};
}
template <typename T>
inline T rtfxDot(const RtfxVec3<T>& a, const RtfxVec3<T>& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
template <typename T>
inline RtfxVec3<T> rtfxCross(const RtfxVec3<T>& a, const RtfxVec3<T>& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
template <typename T>
inline T rtfxLength(const RtfxVec3<T>& a) {
    return std::sqrt(rtfxDot(a, a));
}
/// Unit vector, or `fallback` for a (near) zero vector.
template <typename T>
inline RtfxVec3<T> rtfxNormalize(const RtfxVec3<T>& a, const RtfxVec3<T>& fallback) {
    const T l = rtfxLength(a);
    if (!(l > T(1e-20))) {
        return fallback;
    }
    const T inv = T(1) / l;
    return {a.x * inv, a.y * inv, a.z * inv};
}
template <typename T>
inline RtfxVec3<T> rtfxLoad(const f32 v[3]) {
    return {static_cast<T>(v[0]), static_cast<T>(v[1]), static_cast<T>(v[2])};
}

inline constexpr f64 kRtfxPi = 3.14159265358979323846;

// --- sequence -------------------------------------------------------------------------------------------
/// lowbias32 (Wellons): the pixel / stream hash.
inline u32 rtfxHash(u32 x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

/// Seed of one pixel's sequence for `stream` (shadow light channel 0..3, kRtfxStreamReflection).
inline constexpr u32 kRtfxStreamReflection = 16u;
inline u32 rtfxPixelSeed(u32 px, u32 py, u32 stream, u32 seed) {
    return rtfxHash(px + rtfxHash(py + rtfxHash(stream + rtfxHash(seed))));
}

inline u32 rtfxReverseBits(u32 x) {
    x = ((x >> 1u) & 0x55555555u) | ((x & 0x55555555u) << 1u);
    x = ((x >> 2u) & 0x33333333u) | ((x & 0x33333333u) << 2u);
    x = ((x >> 4u) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4u);
    x = ((x >> 8u) & 0x00FF00FFu) | ((x & 0x00FF00FFu) << 8u);
    return (x >> 16u) | (x << 16u);
}

/// Laine-Karras permutation (bit j depends only on bits below j), Burley 2020 constants.
inline u32 rtfxLaineKarras(u32 x, u32 seed) {
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}

/// Nested uniform (Owen) scramble of a 32-bit fixed-point value.
inline u32 rtfxOwen(u32 x, u32 seed) {
    return rtfxReverseBits(rtfxLaineKarras(rtfxReverseBits(x), seed));
}

inline u32 rtfxHashCombine(u32 seed, u32 v) {
    return seed ^ (v + (seed << 6u) + (seed >> 2u));
}

/// Sobol dimensions 0 (van der Corput) and 1 (x + 1, m_i = 1) of `index`, 32-bit fixed point.
inline void rtfxSobol2(u32 index, u32& x, u32& y) {
    x = rtfxReverseBits(index);
    u32 v = 0x80000000u;
    u32 r = 0u;
    for (u32 bit = 0; bit < 32u; ++bit) {
        if (((index >> bit) & 1u) != 0u) {
            r ^= v;
        }
        v ^= v >> 1u;
    }
    y = r;
}

/// Owen-scrambled, index-shuffled Sobol point n of the sequence `seed` (Burley 2020, section 4).
inline void rtfxSample2(u32 n, u32 seed, u32& ux, u32& uy) {
    const u32 index = rtfxOwen(n, seed);
    u32 x = 0u;
    u32 y = 0u;
    rtfxSobol2(index, x, y);
    ux = rtfxOwen(x, rtfxHashCombine(seed, 0u));
    uy = rtfxOwen(y, rtfxHashCombine(seed, 1u));
}

/// Exact [0, 1) float of the top 24 bits.
inline f32 rtfxUnit(u32 x) { return static_cast<f32>(x >> 8u) * (1.f / 16777216.f); }

// --- surface --------------------------------------------------------------------------------------------
/// World position of a pixel centre at device depth `depth` (forward z/w). `invViewProj` column-major.
template <typename T>
inline RtfxVec3<T> rtfxReconstruct(const f32 invViewProj[16], u32 px, u32 py, T invWidth, T invHeight, T depth) {
    const T nx = (static_cast<T>(px) + T(0.5)) * invWidth * T(2) - T(1);
    const T ny = (static_cast<T>(py) + T(0.5)) * invHeight * T(2) - T(1);
    const f32* m = invViewProj;
    const T x = static_cast<T>(m[0]) * nx + static_cast<T>(m[4]) * ny + static_cast<T>(m[8]) * depth + static_cast<T>(m[12]);
    const T y = static_cast<T>(m[1]) * nx + static_cast<T>(m[5]) * ny + static_cast<T>(m[9]) * depth + static_cast<T>(m[13]);
    const T z = static_cast<T>(m[2]) * nx + static_cast<T>(m[6]) * ny + static_cast<T>(m[10]) * depth + static_cast<T>(m[14]);
    const T w = static_cast<T>(m[3]) * nx + static_cast<T>(m[7]) * ny + static_cast<T>(m[11]) * depth + static_cast<T>(m[15]);
    const T inv = T(1) / w;
    return {x * inv, y * inv, z * inv};
}

/// Signed octahedral RT0 normal (lc_common's fuse_lc_oct_decode).
template <typename T>
inline RtfxVec3<T> rtfxOctDecode(T ox, T oy) {
    RtfxVec3<T> n{ox, oy, T(1) - std::fabs(ox) - std::fabs(oy)};
    if (n.z < T(0)) {
        const T x = (T(1) - std::fabs(oy)) * (ox >= T(0) ? T(1) : T(-1));
        const T y = (T(1) - std::fabs(ox)) * (oy >= T(0) ? T(1) : T(-1));
        n.x = x;
        n.y = y;
    }
    return rtfxNormalize(n, RtfxVec3<T>{T(0), T(0), T(1)});
}

/// Ray origin above the surface.
template <typename T>
inline RtfxVec3<T> rtfxRayOrigin(const RtfxVec3<T>& p, const RtfxVec3<T>& n, const RtfxVec3<T>& camera, T normalBias,
                                 T viewBias) {
    const T bias = normalBias + viewBias * rtfxLength(rtfxSub(p, camera));
    return rtfxAdd(p, rtfxScale(n, bias));
}

/// Orthonormal basis around a unit `n` (Duff et al. 2017).
template <typename T>
inline void rtfxBasis(const RtfxVec3<T>& n, RtfxVec3<T>& t, RtfxVec3<T>& b) {
    const T sign = n.z >= T(0) ? T(1) : T(-1);
    const T a = T(-1) / (sign + n.z);
    const T c = n.x * n.y * a;
    t = {T(1) + sign * n.x * n.x * a, sign * c, -sign * n.x};
    b = {c, sign + n.y * n.y * a, -n.y};
}

// --- light sampling ---------------------------------------------------------------------------------------
/// Uniform point of the unit disk (Shirley-Chiu concentric map) from (u, v) in [0, 1)^2.
template <typename T>
inline void rtfxConcentric(T u, T v, T& dx, T& dy) {
    const T a = T(2) * u - T(1);
    const T b = T(2) * v - T(1);
    if (a == T(0) && b == T(0)) {
        dx = T(0);
        dy = T(0);
        return;
    }
    const T quarter = static_cast<T>(kRtfxPi * 0.25);
    T r = T(0);
    T phi = T(0);
    if (std::fabs(a) > std::fabs(b)) {
        r = a;
        phi = quarter * (b / a);
    } else {
        r = b;
        phi = static_cast<T>(kRtfxPi * 0.5) - quarter * (a / b);
    }
    dx = r * std::cos(phi);
    dy = r * std::sin(phi);
}

/// Shadow ray of sample (u, v) of `light` from `origin`: unit direction + tMax. False for kind None.
template <typename T>
inline bool rtfxShadowRay(const RtfxShadowLight& light, const RtfxVec3<T>& origin, T u, T v, T farDistance,
                          RtfxVec3<T>& dir, T& tMax) {
    const RtfxVec3<T> up{T(0), T(1), T(0)};
    switch (light.kind) {
    case kRtfxLightDirectional:
        dir = rtfxNormalize(rtfxLoad<T>(light.direction), up);
        tMax = farDistance;
        return true;
    case kRtfxLightSun: {
        const RtfxVec3<T> axis = rtfxNormalize(rtfxLoad<T>(light.direction), up);
        RtfxVec3<T> t{};
        RtfxVec3<T> b{};
        rtfxBasis(axis, t, b);
        const T cosTheta = T(1) - u * (T(1) - static_cast<T>(light.cosCone));
        const T sinTheta = std::sqrt(std::fmax(T(0), T(1) - cosTheta * cosTheta));
        const T phi = static_cast<T>(2.0 * kRtfxPi) * v;
        const RtfxVec3<T> d = rtfxAdd(rtfxAdd(rtfxScale(t, sinTheta * std::cos(phi)), rtfxScale(b, sinTheta * std::sin(phi))),
                                      rtfxScale(axis, cosTheta));
        dir = rtfxNormalize(d, axis);
        tMax = farDistance;
        return true;
    }
    case kRtfxLightPoint:
    case kRtfxLightRect:
    case kRtfxLightDisk: {
        RtfxVec3<T> target = rtfxLoad<T>(light.position);
        if (light.kind == kRtfxLightRect) {
            const T sx = T(2) * u - T(1);
            const T sy = T(2) * v - T(1);
            target = rtfxAdd(target, rtfxAdd(rtfxScale(rtfxLoad<T>(light.axisX), sx), rtfxScale(rtfxLoad<T>(light.axisY), sy)));
        } else if (light.kind == kRtfxLightDisk) {
            T dx = T(0);
            T dy = T(0);
            rtfxConcentric(u, v, dx, dy);
            target = rtfxAdd(target, rtfxAdd(rtfxScale(rtfxLoad<T>(light.axisX), dx), rtfxScale(rtfxLoad<T>(light.axisY), dy)));
        }
        const RtfxVec3<T> d = rtfxSub(target, origin);
        const T dist = rtfxLength(d);
        if (!(dist > T(1e-6))) {
            dir = up;
            tMax = T(0);
            return true; // the origin is on the light: nothing can occlude it
        }
        dir = rtfxScale(d, T(1) / dist);
        tMax = dist * T(0.9999); // stop short of the (analytic, untraced) emitter
        return true;
    }
    default:
        return false;
    }
}

// --- reflections --------------------------------------------------------------------------------------------
/// GGX visible normal (tangent space, +z = normal) for view `wi` (unit, wi.z > 0) and alpha.
template <typename T>
inline RtfxVec3<T> rtfxSampleVndf(const RtfxVec3<T>& wi, T alpha, T u, T v) {
    const RtfxVec3<T> z{T(0), T(0), T(1)};
    const RtfxVec3<T> wiStd = rtfxNormalize(RtfxVec3<T>{wi.x * alpha, wi.y * alpha, wi.z}, z);
    const T phi = static_cast<T>(2.0 * kRtfxPi) * u;
    const T cz = (T(1) - v) * (T(1) + wiStd.z) - wiStd.z;
    const T sinTheta = std::sqrt(std::fmin(std::fmax(T(1) - cz * cz, T(0)), T(1)));
    const RtfxVec3<T> h{sinTheta * std::cos(phi) + wiStd.x, sinTheta * std::sin(phi) + wiStd.y, cz + wiStd.z};
    return rtfxNormalize(RtfxVec3<T>{h.x * alpha, h.y * alpha, h.z}, z);
}

/// Reflection direction of sample (u, v) at a surface with unit normal `n`, unit direction to the viewer
/// `view` and `roughness`. False (no ray, zero radiance) when the direction falls below the surface.
template <typename T>
inline bool rtfxReflectionRay(const RtfxVec3<T>& n, const RtfxVec3<T>& view, T roughness, T mirrorRoughness, T u, T v,
                              RtfxVec3<T>& dir) {
    RtfxVec3<T> t{};
    RtfxVec3<T> b{};
    rtfxBasis(n, t, b);
    const RtfxVec3<T> wi = rtfxNormalize(RtfxVec3<T>{rtfxDot(view, t), rtfxDot(view, b), std::fmax(rtfxDot(view, n), T(1e-4))}, n);
    RtfxVec3<T> m{T(0), T(0), T(1)};
    const T r = std::fmin(std::fmax(roughness, T(0)), T(1));
    if (r >= mirrorRoughness) {
        m = rtfxSampleVndf(wi, r * r, u, v);
    }
    const T d = T(2) * rtfxDot(wi, m);
    const RtfxVec3<T> l{m.x * d - wi.x, m.y * d - wi.y, m.z * d - wi.z};
    if (!(l.z > T(0))) {
        return false;
    }
    dir = rtfxNormalize(rtfxAdd(rtfxAdd(rtfxScale(t, l.x), rtfxScale(b, l.y)), rtfxScale(n, l.z)), n);
    return true;
}

/// GGX visible-normal density D_v(m) = G1(wi) max(0, wi.m) D(m) / wi.z for alpha (the sampler's pdf over
/// half vectors; used by the gates).
template <typename T>
inline T rtfxVndfPdf(const RtfxVec3<T>& wi, const RtfxVec3<T>& m, T alpha) {
    if (!(m.z > T(0))) {
        return T(0);
    }
    const T a2 = alpha * alpha;
    const T c2 = m.z * m.z;
    const T denom = c2 * (a2 - T(1)) + T(1);
    const T d = a2 / (static_cast<T>(kRtfxPi) * denom * denom);
    const T tan2 = (T(1) - wi.z * wi.z) / (wi.z * wi.z);
    const T lambda = (std::sqrt(T(1) + a2 * tan2) - T(1)) * T(0.5);
    const T g1 = T(1) / (T(1) + lambda);
    return g1 * std::fmax(T(0), rtfxDot(wi, m)) * d / wi.z;
}

} // namespace fuse::renderer::rt_effects
