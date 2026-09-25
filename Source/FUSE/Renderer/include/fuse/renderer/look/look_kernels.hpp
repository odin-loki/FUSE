#pragma once

// Single-source Look kernels (docs/compute-kernels.md). Every per-pixel / per-lattice-point formula of
// the look chain lives here exactly once and runs on CpuReference, CpuParallel and (with a .cu
// trampoline) CUDA. Device-safe: FUSE_HOST_DEVICE inline helpers, POD params, no allocation.
//
// Conventions: images are row-major `width * height` math::Vec3; pixel (x, y) has its centre at
// (x + 0.5, y + 0.5) and uv = centre / size. 3D LUTs are `n^3` Vec3 with red fastest, then green, then
// blue (the .cube order); their domain is [0, 1]^3 in sRGB-encoded display space.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::look::kernels {

using math::Vec2;
using math::Vec3;

inline constexpr kernel::Dim3 kImageWorkgroup{8u, 8u, 1u};
inline constexpr kernel::Dim3 kLinearWorkgroup{64u, 1u, 1u};
/// Maximum number of external LUTs blended in one bake (weights are compacted on the host).
inline constexpr u32 kMaxLutBlend = 8u;

// ---------------------------------------------------------------------------------------------
// Scalar helpers

FUSE_HOST_DEVICE inline f32 saturate(f32 v) { return std::min(std::max(v, 0.f), 1.f); }
FUSE_HOST_DEVICE inline f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
FUSE_HOST_DEVICE inline Vec3 mul3(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
FUSE_HOST_DEVICE inline Vec3 max3(const Vec3& a, f32 m) { return {std::max(a.x, m), std::max(a.y, m), std::max(a.z, m)}; }
FUSE_HOST_DEVICE inline Vec3 min3(const Vec3& a, f32 m) { return {std::min(a.x, m), std::min(a.y, m), std::min(a.z, m)}; }
FUSE_HOST_DEVICE inline Vec3 saturate3(const Vec3& a) { return {saturate(a.x), saturate(a.y), saturate(a.z)}; }
FUSE_HOST_DEVICE inline f32 luminance709(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

/// sRGB OETF (IEC 61966-2-1), same expression as renderer::linear_to_srgb.
FUSE_HOST_DEVICE inline f32 srgb_encode(f32 c) {
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}
FUSE_HOST_DEVICE inline f32 srgb_decode(f32 e) {
    return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
}
FUSE_HOST_DEVICE inline Vec3 srgb_encode3(const Vec3& c) { return {srgb_encode(c.x), srgb_encode(c.y), srgb_encode(c.z)}; }
FUSE_HOST_DEVICE inline Vec3 srgb_decode3(const Vec3& c) { return {srgb_decode(c.x), srgb_decode(c.y), srgb_decode(c.z)}; }

// SMPTE ST 2084 (PQ) constants.
inline constexpr f32 kPqM1 = 2610.f / 16384.f;
inline constexpr f32 kPqM2 = 2523.f / 4096.f * 128.f;
inline constexpr f32 kPqC1 = 3424.f / 4096.f;
inline constexpr f32 kPqC2 = 2413.f / 4096.f * 32.f;
inline constexpr f32 kPqC3 = 2392.f / 4096.f * 32.f;
inline constexpr f32 kPqMaxNits = 10000.f;
/// scRGB: linear Rec.709 where 1.0 = 80 cd/m^2.
inline constexpr f32 kScRgbReferenceNits = 80.f;

/// ST 2084 inverse EOTF: absolute luminance (cd/m^2) -> PQ signal in [0, 1].
FUSE_HOST_DEVICE inline f32 pq_encode_nits(f32 nits) {
    const f32 y = saturate(nits / kPqMaxNits);
    const f32 ym = std::pow(y, kPqM1);
    return std::pow((kPqC1 + kPqC2 * ym) / (1.f + kPqC3 * ym), kPqM2);
}
/// ST 2084 EOTF: PQ signal -> cd/m^2.
FUSE_HOST_DEVICE inline f32 pq_decode_nits(f32 signal) {
    const f32 e = std::pow(saturate(signal), 1.f / kPqM2);
    const f32 num = std::max(e - kPqC1, 0.f);
    return kPqMaxNits * std::pow(num / (kPqC2 - kPqC3 * e), 1.f / kPqM1);
}

/// Linear Rec.709 -> linear Rec.2020 (ITU-R BT.2087).
FUSE_HOST_DEVICE inline Vec3 rec709_to_rec2020(const Vec3& c) {
    return {0.627403896f * c.x + 0.329283039f * c.y + 0.043313065f * c.z,
            0.069097289f * c.x + 0.919540395f * c.y + 0.011362316f * c.z,
            0.016391439f * c.x + 0.088013308f * c.y + 0.895595253f * c.z};
}

// ---------------------------------------------------------------------------------------------
// Tone mapping (same curves as renderer/postprocess/tonemap.cpp; parity-tested against it)

FUSE_HOST_DEVICE inline f32 tonemap_aces(f32 v) {
    return std::min(std::max((v * (2.51f * v + 0.03f)) / (v * (2.43f * v + 0.59f) + 0.14f), 0.f), 1.f);
}
FUSE_HOST_DEVICE inline f32 tonemap_filmic(f32 v) {
    const f32 A = 0.22f, B = 0.30f, C = 0.10f, D = 0.20f, E = 0.01f, F = 0.30f;
    return std::min(std::max(((v * (A * v + C * B) + D * E) / (v * (A * v + B) + D * F)) - E / F, 0.f), 1.f);
}
FUSE_HOST_DEVICE inline f32 tonemap_reinhard(f32 v) { return std::min(std::max(v / (1.f + v), 0.f), 1.f); }

/// HDR display mapping (paper-white relative): identity up to ks = lmax / 2, then an exponential
/// shoulder that is C1 at ks and approaches lmax = peak / paper white asymptotically.
FUSE_HOST_DEVICE inline f32 tonemap_hdr_shoulder(f32 v, f32 lmax) {
    v = std::max(v, 0.f);
    const f32 ks = 0.5f * lmax;
    if (v <= ks) {
        return v;
    }
    const f32 range = lmax - ks;
    return ks + range * (1.f - std::exp(-(v - ks) / range));
}

enum : u32 { kToneAces = 0u, kToneFilmic = 1u, kToneReinhard = 2u, kToneNeutral = 3u };

FUSE_HOST_DEVICE inline f32 tonemap_channel(f32 v, u32 op) {
    switch (op) {
    case kToneFilmic:
        return tonemap_filmic(v);
    case kToneReinhard:
        return tonemap_reinhard(v);
    case kToneNeutral:
        return std::min(std::max(v, 0.f), 1.f);
    default:
        return tonemap_aces(v);
    }
}

// ---------------------------------------------------------------------------------------------
// 3D LUT sampling

FUSE_HOST_DEVICE inline u32 lut_index(u32 r, u32 g, u32 b, u32 n) { return r + g * n + b * n * n; }

/// Tetrahedral interpolation (the unit cube split into 6 tetrahedra along the neutral diagonal).
/// Exact for LUTs of affine functions and neutral-preserving along the grey axis. `n >= 2`.
FUSE_HOST_DEVICE inline Vec3 lut_sample_tetrahedral(const Vec3* lut, u32 n, const Vec3& c) {
    const f32 scale = static_cast<f32>(n - 1u);
    const f32 fr = saturate(c.x) * scale;
    const f32 fg = saturate(c.y) * scale;
    const f32 fb = saturate(c.z) * scale;
    const u32 r0 = std::min(static_cast<u32>(fr), n - 2u);
    const u32 g0 = std::min(static_cast<u32>(fg), n - 2u);
    const u32 b0 = std::min(static_cast<u32>(fb), n - 2u);
    const f32 dr = fr - static_cast<f32>(r0);
    const f32 dg = fg - static_cast<f32>(g0);
    const f32 db = fb - static_cast<f32>(b0);
    const Vec3 c000 = lut[lut_index(r0, g0, b0, n)];
    const Vec3 c111 = lut[lut_index(r0 + 1u, g0 + 1u, b0 + 1u, n)];
    if (dr > dg) {
        if (dg > db) { // r > g > b
            const Vec3 c100 = lut[lut_index(r0 + 1u, g0, b0, n)];
            const Vec3 c110 = lut[lut_index(r0 + 1u, g0 + 1u, b0, n)];
            return c000 + (c100 - c000) * dr + (c110 - c100) * dg + (c111 - c110) * db;
        }
        if (dr > db) { // r > b >= g
            const Vec3 c100 = lut[lut_index(r0 + 1u, g0, b0, n)];
            const Vec3 c101 = lut[lut_index(r0 + 1u, g0, b0 + 1u, n)];
            return c000 + (c100 - c000) * dr + (c101 - c100) * db + (c111 - c101) * dg;
        }
        // b >= r > g
        const Vec3 c001 = lut[lut_index(r0, g0, b0 + 1u, n)];
        const Vec3 c101 = lut[lut_index(r0 + 1u, g0, b0 + 1u, n)];
        return c000 + (c001 - c000) * db + (c101 - c001) * dr + (c111 - c101) * dg;
    }
    if (db > dg) { // b > g >= r
        const Vec3 c001 = lut[lut_index(r0, g0, b0 + 1u, n)];
        const Vec3 c011 = lut[lut_index(r0, g0 + 1u, b0 + 1u, n)];
        return c000 + (c001 - c000) * db + (c011 - c001) * dg + (c111 - c011) * dr;
    }
    if (db > dr) { // g >= b > r
        const Vec3 c010 = lut[lut_index(r0, g0 + 1u, b0, n)];
        const Vec3 c011 = lut[lut_index(r0, g0 + 1u, b0 + 1u, n)];
        return c000 + (c010 - c000) * dg + (c011 - c010) * db + (c111 - c011) * dr;
    }
    // g >= r >= b
    const Vec3 c010 = lut[lut_index(r0, g0 + 1u, b0, n)];
    const Vec3 c110 = lut[lut_index(r0 + 1u, g0 + 1u, b0, n)];
    return c000 + (c010 - c000) * dg + (c110 - c010) * dr + (c111 - c110) * db;
}

/// Trilinear interpolation (8 corners) — reference for the accuracy comparison.
FUSE_HOST_DEVICE inline Vec3 lut_sample_trilinear(const Vec3* lut, u32 n, const Vec3& c) {
    const f32 scale = static_cast<f32>(n - 1u);
    const f32 fr = saturate(c.x) * scale;
    const f32 fg = saturate(c.y) * scale;
    const f32 fb = saturate(c.z) * scale;
    const u32 r0 = std::min(static_cast<u32>(fr), n - 2u);
    const u32 g0 = std::min(static_cast<u32>(fg), n - 2u);
    const u32 b0 = std::min(static_cast<u32>(fb), n - 2u);
    const f32 dr = fr - static_cast<f32>(r0);
    const f32 dg = fg - static_cast<f32>(g0);
    const f32 db = fb - static_cast<f32>(b0);
    const Vec3 v000 = lut[lut_index(r0, g0, b0, n)];
    const Vec3 v100 = lut[lut_index(r0 + 1u, g0, b0, n)];
    const Vec3 v010 = lut[lut_index(r0, g0 + 1u, b0, n)];
    const Vec3 v110 = lut[lut_index(r0 + 1u, g0 + 1u, b0, n)];
    const Vec3 v001 = lut[lut_index(r0, g0, b0 + 1u, n)];
    const Vec3 v101 = lut[lut_index(r0 + 1u, g0, b0 + 1u, n)];
    const Vec3 v011 = lut[lut_index(r0, g0 + 1u, b0 + 1u, n)];
    const Vec3 v111 = lut[lut_index(r0 + 1u, g0 + 1u, b0 + 1u, n)];
    const Vec3 c00 = v000 + (v100 - v000) * dr;
    const Vec3 c10 = v010 + (v110 - v010) * dr;
    const Vec3 c01 = v001 + (v101 - v001) * dr;
    const Vec3 c11 = v011 + (v111 - v011) * dr;
    const Vec3 c0 = c00 + (c10 - c00) * dg;
    const Vec3 c1 = c01 + (c11 - c01) * dg;
    return c0 + (c1 - c0) * db;
}

// ---------------------------------------------------------------------------------------------
// Colour grading (display-linear in, display-linear out; LUT generation evaluates this per lattice point)

enum : u32 {
    kGradeWhiteBalance = 1u << 0,
    kGradeLiftGain = 1u << 1,
    kGradeGamma = 1u << 2,
    kGradeContrast = 1u << 3,
    kGradeSaturation = 1u << 4,
    kGradeCurves = 1u << 5,
};

/// Host-resolved grading constants (see lut3d.hpp `make_grade_params`). Stages whose flag is clear are
/// skipped entirely, so neutral parameters are an exact identity.
struct GradeParams {
    f32 wb[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; ///< row-major linear-RGB white-balance matrix
    u32 flags = 0;
    Vec3 lift{};
    Vec3 inv_gamma{1.f, 1.f, 1.f};
    Vec3 gain{1.f, 1.f, 1.f};
    f32 contrast = 1.f;
    f32 contrast_pivot = 0.18f;
    f32 saturation = 1.f;
    Vec3 curve_p[5] = {};  ///< per-channel control values at x = 0, .25, .5, .75, 1 (sRGB-encoded domain)
    Vec3 curve_m[5] = {};  ///< Fritsch-Carlson monotone tangents
};

/// Monotone cubic Hermite through 5 evenly spaced points (h = 0.25). `t` in [0, 1].
FUSE_HOST_DEVICE inline f32 curve_eval(const f32 p[5], const f32 m[5], f32 t) {
    t = saturate(t);
    const f32 x = t * 4.f;
    const u32 k = std::min(static_cast<u32>(x), 3u);
    const f32 s = x - static_cast<f32>(k);
    const f32 s2 = s * s;
    const f32 s3 = s2 * s;
    const f32 h = 0.25f;
    return (2.f * s3 - 3.f * s2 + 1.f) * p[k] + (s3 - 2.f * s2 + s) * h * m[k] + (-2.f * s3 + 3.f * s2) * p[k + 1u] +
           (s3 - s2) * h * m[k + 1u];
}

FUSE_HOST_DEVICE inline Vec3 grade_linear(Vec3 c, const GradeParams& g) {
    if (g.flags & kGradeWhiteBalance) {
        c = Vec3{g.wb[0] * c.x + g.wb[1] * c.y + g.wb[2] * c.z, g.wb[3] * c.x + g.wb[4] * c.y + g.wb[5] * c.z,
                 g.wb[6] * c.x + g.wb[7] * c.y + g.wb[8] * c.z};
    }
    if (g.flags & kGradeLiftGain) {
        // out = gain * (c + lift * (1 - c)): lift raises blacks, keeps white; gain scales.
        c = Vec3{g.gain.x * (c.x + g.lift.x * (1.f - c.x)), g.gain.y * (c.y + g.lift.y * (1.f - c.y)),
                 g.gain.z * (c.z + g.lift.z * (1.f - c.z))};
    }
    if (g.flags & kGradeGamma) {
        c = Vec3{std::pow(std::max(c.x, 0.f), g.inv_gamma.x), std::pow(std::max(c.y, 0.f), g.inv_gamma.y),
                 std::pow(std::max(c.z, 0.f), g.inv_gamma.z)};
    }
    if (g.flags & kGradeContrast) {
        // Log-space contrast around the pivot (mid grey stays put).
        const f32 p = g.contrast_pivot;
        c = Vec3{p * std::pow(std::max(c.x, 0.f) / p, g.contrast), p * std::pow(std::max(c.y, 0.f) / p, g.contrast),
                 p * std::pow(std::max(c.z, 0.f) / p, g.contrast)};
    }
    if (g.flags & kGradeSaturation) {
        const f32 l = luminance709(c);
        c = Vec3{l + (c.x - l) * g.saturation, l + (c.y - l) * g.saturation, l + (c.z - l) * g.saturation};
    }
    return c;
}

FUSE_HOST_DEVICE inline Vec3 grade_curves(const Vec3& e, const GradeParams& g) {
    if (!(g.flags & kGradeCurves)) {
        return e;
    }
    f32 p[5], m[5];
    Vec3 out{};
    for (u32 ch = 0; ch < 3u; ++ch) {
        for (u32 i = 0; i < 5u; ++i) {
            p[i] = ch == 0u ? g.curve_p[i].x : (ch == 1u ? g.curve_p[i].y : g.curve_p[i].z);
            m[i] = ch == 0u ? g.curve_m[i].x : (ch == 1u ? g.curve_m[i].y : g.curve_m[i].z);
        }
        const f32 in = ch == 0u ? e.x : (ch == 1u ? e.y : e.z);
        const f32 v = curve_eval(p, m, in);
        if (ch == 0u) {
            out.x = v;
        } else if (ch == 1u) {
            out.y = v;
        } else {
            out.z = v;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Image helpers

/// Bilinear fetch at continuous pixel position `pos` (pixel centres at +0.5), clamp to edge.
FUSE_HOST_DEVICE inline Vec3 sample_bilinear(const Vec3* img, u32 w, u32 h, f32 px, f32 py) {
    const f32 fx = std::min(std::max(px - 0.5f, 0.f), static_cast<f32>(w - 1u));
    const f32 fy = std::min(std::max(py - 0.5f, 0.f), static_cast<f32>(h - 1u));
    const u32 x0 = static_cast<u32>(fx);
    const u32 y0 = static_cast<u32>(fy);
    const u32 x1 = std::min(x0 + 1u, w - 1u);
    const u32 y1 = std::min(y0 + 1u, h - 1u);
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 ty = fy - static_cast<f32>(y0);
    const Vec3 a = img[y0 * w + x0] + (img[y0 * w + x1] - img[y0 * w + x0]) * tx;
    const Vec3 b = img[y1 * w + x0] + (img[y1 * w + x1] - img[y1 * w + x0]) * tx;
    return a + (b - a) * ty;
}

FUSE_HOST_DEVICE inline f32 sample_bilinear_scalar(const f32* img, u32 w, u32 h, f32 px, f32 py) {
    const f32 fx = std::min(std::max(px - 0.5f, 0.f), static_cast<f32>(w - 1u));
    const f32 fy = std::min(std::max(py - 0.5f, 0.f), static_cast<f32>(h - 1u));
    const u32 x0 = static_cast<u32>(fx);
    const u32 y0 = static_cast<u32>(fy);
    const u32 x1 = std::min(x0 + 1u, w - 1u);
    const u32 y1 = std::min(y0 + 1u, h - 1u);
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 ty = fy - static_cast<f32>(y0);
    const f32 a = img[y0 * w + x0] + (img[y0 * w + x1] - img[y0 * w + x0]) * tx;
    const f32 b = img[y1 * w + x0] + (img[y1 * w + x1] - img[y1 * w + x0]) * tx;
    return a + (b - a) * ty;
}

FUSE_HOST_DEVICE inline u32 clamp_index(s64 i, u32 size) {
    return i < 0 ? 0u : (i >= static_cast<s64>(size) ? size - 1u : static_cast<u32>(i));
}

/// 64-bit avalanche hash (same as renderer::film_grain_noise's splitmix64).
FUSE_HOST_DEVICE inline u64 splitmix64(u64 value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

/// Film grain sample in [-1, 1) — bit-identical to renderer::film_grain_noise.
FUSE_HOST_DEVICE inline f32 grain_noise(u64 frame_seed, u32 px, u32 py) {
    const u64 pixel = (static_cast<u64>(py) << 32) | static_cast<u64>(px);
    const u64 hash = splitmix64(splitmix64(frame_seed) ^ pixel);
    const f32 unit = (static_cast<f32>(hash >> 40) + 0.5f) * (1.f / 16777216.f);
    return unit * 2.f - 1.f;
}

/// Vignette gain: r = |(u * mix(1, aspect, roundness), v)| normalised so the frame corner is 1,
/// gain = max(1 - intensity * r^falloff, 0). roundness 0 + falloff 2 reproduces the legacy
/// renderer::vignette_factor (1 - s (u^2 + v^2) / 2).
FUSE_HOST_DEVICE inline f32 vignette_gain(f32 intensity, f32 falloff, f32 roundness, u32 px, u32 py, u32 w, u32 h) {
    const f32 u = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(w) * 2.f - 1.f;
    const f32 v = (static_cast<f32>(py) + 0.5f) / static_cast<f32>(h) * 2.f - 1.f;
    const f32 aspect = static_cast<f32>(w) / static_cast<f32>(h);
    const f32 xs = 1.f + (aspect - 1.f) * roundness;
    const f32 x = u * xs;
    const f32 r = std::sqrt((x * x + v * v) / (xs * xs + 1.f));
    return std::max(1.f - intensity * std::pow(r, falloff), 0.f);
}

// ---------------------------------------------------------------------------------------------
// Kernels. Each has `kName` (profiler / stats / GPU timestamp name) and a POD params struct.

/// Bloom soft-knee prefilter (renderer::Bloom::extractBright).
struct BloomPrefilterParams {
    kernel::Span<const Vec3> src;
    kernel::Span<Vec3> dst;
    f32 threshold = 1.f;
    f32 knee = 0.5f;
};
struct BloomPrefilterKernel {
    static constexpr const char* kName = "look_bloom_prefilter";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BloomPrefilterParams& p) const {
        const Vec3 hdr = p.src[idx.linear];
        const f32 lum = 0.2126f * hdr.x + 0.7152f * hdr.y + 0.0722f * hdr.z;
        const f32 knee = std::max(p.knee, 0.f);
        f32 response = 0.f;
        if (lum <= p.threshold - knee) {
            response = 0.f;
        } else if (knee > 0.f && lum < p.threshold + knee) {
            const f32 soft = lum - p.threshold + knee;
            response = (soft * soft) / (4.f * knee);
        } else {
            response = std::max(lum - p.threshold, 0.f);
        }
        p.dst[idx.linear] = (response <= 0.f || lum <= 0.f) ? Vec3{} : hdr * (response / lum);
    }
};

/// Separable [1 3 3 1] / 8 tent, 2x decimation along one axis (clamp to edge). Grid = dst extent.
struct BloomDownsampleParams {
    kernel::Span<const Vec3> src;
    kernel::Span<Vec3> dst;
    u32 src_w = 0, src_h = 0, dst_w = 0;
    u32 horizontal = 1;
};
struct BloomDownsampleKernel {
    static constexpr const char* kName = "look_bloom_downsample";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BloomDownsampleParams& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const f32 w[4] = {1.f / 8.f, 3.f / 8.f, 3.f / 8.f, 1.f / 8.f};
        Vec3 sum{};
        for (s64 k = 0; k < 4; ++k) {
            if (p.horizontal != 0u) {
                const u32 sx = clamp_index(2 * static_cast<s64>(x) - 1 + k, p.src_w);
                sum = sum + p.src[y * p.src_w + sx] * w[k];
            } else {
                const u32 sy = clamp_index(2 * static_cast<s64>(y) - 1 + k, p.src_h);
                sum = sum + p.src[sy * p.src_w + x] * w[k];
            }
        }
        p.dst[y * p.dst_w + x] = sum;
    }
};

/// Separable bilinear 2x upsample along one axis; the vertical pass optionally combines
/// dst = down * (1 - scatter) + up * scatter (renderer::bloom_image's level combine).
struct BloomUpsampleParams {
    kernel::Span<const Vec3> src;
    kernel::Span<Vec3> dst;
    kernel::Span<const Vec3> combine_down; ///< empty = no combine
    u32 src_w = 0, src_h = 0, dst_w = 0;
    u32 horizontal = 1;
    f32 scatter = 0.7f;
};
struct BloomUpsampleKernel {
    static constexpr const char* kName = "look_bloom_upsample";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BloomUpsampleParams& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const u32 pos = p.horizontal != 0u ? x : y;
        const u32 srcSize = p.horizontal != 0u ? p.src_w : p.src_h;
        const s64 i = static_cast<s64>(pos / 2u);
        const s64 other = (pos % 2u == 0u) ? i - 1 : i + 1;
        const u32 a = clamp_index(i, srcSize);
        const u32 b = clamp_index(other, srcSize);
        const Vec3 va = p.horizontal != 0u ? p.src[y * p.src_w + a] : p.src[a * p.src_w + x];
        const Vec3 vb = p.horizontal != 0u ? p.src[y * p.src_w + b] : p.src[b * p.src_w + x];
        const Vec3 up = va * 0.75f + vb * 0.25f;
        const u32 o = y * p.dst_w + x;
        p.dst[o] = p.combine_down.empty() ? up : p.combine_down[o] * (1.f - p.scatter) + up * p.scatter;
    }
};

/// color += bloom * intensity * tint
struct BloomCompositeParams {
    kernel::Span<Vec3> color;
    kernel::Span<const Vec3> bloom;
    f32 intensity = 0.f;
    Vec3 tint{1.f, 1.f, 1.f};
};
struct BloomCompositeKernel {
    static constexpr const char* kName = "look_bloom_composite";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BloomCompositeParams& p) const {
        const Vec3 b = p.bloom[idx.linear] * p.intensity;
        p.color[idx.linear] = p.color[idx.linear] + mul3(b, p.tint);
    }
};

/// Lens dirt: color += bloom * dirt(uv) * intensity * tint (texture-modulated bloom; any dirt resolution).
struct LensDirtParams {
    kernel::Span<Vec3> color;
    kernel::Span<const Vec3> bloom;
    kernel::Span<const f32> dirt;
    u32 width = 0, height = 0, dirt_w = 0, dirt_h = 0;
    f32 intensity = 0.f;
    Vec3 tint{1.f, 1.f, 1.f};
};
struct LensDirtKernel {
    static constexpr const char* kName = "look_lens_dirt";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const LensDirtParams& p) const {
        const f32 u = (static_cast<f32>(idx.global.x) + 0.5f) / static_cast<f32>(p.width);
        const f32 v = (static_cast<f32>(idx.global.y) + 0.5f) / static_cast<f32>(p.height);
        const f32 d = sample_bilinear_scalar(p.dirt.data, p.dirt_w, p.dirt_h, u * static_cast<f32>(p.dirt_w),
                                             v * static_cast<f32>(p.dirt_h));
        const Vec3 b = p.bloom[idx.linear] * (d * p.intensity);
        p.color[idx.linear] = p.color[idx.linear] + mul3(b, p.tint);
    }
};

/// Screen-space lens flare from the bloom bright pass (half-res pyramid level): `ghost_count` ghosts
/// mirrored through the image centre plus a halo ring, each with a radial chromatic split.
/// Grid = flare buffer extent (== bright-pass extent).
struct LensFlareParams {
    kernel::Span<const Vec3> bright;
    kernel::Span<Vec3> dst;
    u32 w = 0, h = 0;
    u32 ghost_count = 4;
    f32 ghost_spacing = 0.4f;
    f32 threshold = 0.f;
    f32 halo_radius = 0.45f;
    f32 halo_thickness = 0.08f;
    f32 halo_intensity = 0.5f;
    f32 chromatic_shift = 0.005f;
};

FUSE_HOST_DEVICE inline Vec3 flare_fetch(const LensFlareParams& p, f32 u, f32 v, f32 dx, f32 dy) {
    const f32 fw = static_cast<f32>(p.w);
    const f32 fh = static_cast<f32>(p.h);
    const f32 r = sample_bilinear(p.bright.data, p.w, p.h, (u + dx) * fw, (v + dy) * fh).x;
    const f32 g = sample_bilinear(p.bright.data, p.w, p.h, u * fw, v * fh).y;
    const f32 b = sample_bilinear(p.bright.data, p.w, p.h, (u - dx) * fw, (v - dy) * fh).z;
    return max3(Vec3{r, g, b} - Vec3{p.threshold, p.threshold, p.threshold}, 0.f);
}

struct LensFlareKernel {
    static constexpr const char* kName = "look_lens_flare";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const LensFlareParams& p) const {
        const f32 u = (static_cast<f32>(idx.global.x) + 0.5f) / static_cast<f32>(p.w);
        const f32 v = (static_cast<f32>(idx.global.y) + 0.5f) / static_cast<f32>(p.h);
        const f32 gx = (0.5f - u) * p.ghost_spacing;
        const f32 gy = (0.5f - v) * p.ghost_spacing;
        const f32 glen = std::sqrt(gx * gx + gy * gy);
        const f32 dirx = glen > 0.f ? gx / glen : 0.f;
        const f32 diry = glen > 0.f ? gy / glen : 0.f;
        Vec3 acc{};
        for (u32 i = 1u; i <= p.ghost_count; ++i) {
            const f32 su = u + gx * static_cast<f32>(i);
            const f32 sv = v + gy * static_cast<f32>(i);
            if (su < 0.f || su > 1.f || sv < 0.f || sv > 1.f) {
                continue;
            }
            const f32 cu = su - 0.5f;
            const f32 cv = sv - 0.5f;
            const f32 fall = 1.f - saturate(std::sqrt(cu * cu + cv * cv) / 0.70710678f);
            acc = acc + flare_fetch(p, su, sv, dirx * p.chromatic_shift, diry * p.chromatic_shift) * (fall * fall);
        }
        // Halo: ring of radius halo_radius (aspect-corrected) sampling towards the centre.
        const f32 aspect = static_cast<f32>(p.w) / static_cast<f32>(p.h);
        const f32 ax = (0.5f - u) * aspect;
        const f32 ay = 0.5f - v;
        const f32 alen = std::sqrt(ax * ax + ay * ay);
        if (alen > 0.f && p.halo_intensity > 0.f) {
            const f32 hx = ax / alen;
            const f32 hy = ay / alen;
            const f32 su = u + hx * p.halo_radius / aspect;
            const f32 sv = v + hy * p.halo_radius;
            const f32 ring = saturate(1.f - std::fabs(alen - p.halo_radius) / p.halo_thickness);
            if (ring > 0.f && su >= 0.f && su <= 1.f && sv >= 0.f && sv <= 1.f) {
                acc = acc + flare_fetch(p, su, sv, hx * p.chromatic_shift / aspect, hy * p.chromatic_shift) *
                                (ring * p.halo_intensity);
            }
        }
        p.dst[idx.linear] = acc;
    }
};

/// color += bilinear(flare) * intensity * tint (flare buffer at any resolution).
struct FlareCompositeParams {
    kernel::Span<Vec3> color;
    kernel::Span<const Vec3> flare;
    u32 width = 0, height = 0, flare_w = 0, flare_h = 0;
    f32 intensity = 0.f;
    Vec3 tint{1.f, 1.f, 1.f};
};
struct FlareCompositeKernel {
    static constexpr const char* kName = "look_lens_flare_composite";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const FlareCompositeParams& p) const {
        const f32 u = (static_cast<f32>(idx.global.x) + 0.5f) / static_cast<f32>(p.width);
        const f32 v = (static_cast<f32>(idx.global.y) + 0.5f) / static_cast<f32>(p.height);
        const Vec3 f = sample_bilinear(p.flare.data, p.flare_w, p.flare_h, u * static_cast<f32>(p.flare_w),
                                       v * static_cast<f32>(p.flare_h)) *
                       p.intensity;
        p.color[idx.linear] = p.color[idx.linear] + mul3(f, p.tint);
    }
};

/// color *= scale (scale = 2^EV, resolved on the host).
struct ExposureParams {
    kernel::Span<Vec3> color;
    f32 scale = 1.f;
};
struct ExposureKernel {
    static constexpr const char* kName = "look_exposure";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const ExposureParams& p) const {
        p.color[idx.linear] = p.color[idx.linear] * p.scale;
    }
};

/// SDR: per-channel operator into [0, 1]. HDR (`hdr_lmax > 0`): paper-white-relative shoulder to lmax.
struct ToneMapParams {
    kernel::Span<Vec3> color;
    u32 op = kToneAces;
    f32 hdr_lmax = 0.f;
};
struct ToneMapKernel {
    static constexpr const char* kName = "look_tonemap";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const ToneMapParams& p) const {
        const Vec3 c = p.color[idx.linear];
        if (p.hdr_lmax > 0.f) {
            p.color[idx.linear] = Vec3{tonemap_hdr_shoulder(c.x, p.hdr_lmax), tonemap_hdr_shoulder(c.y, p.hdr_lmax),
                                       tonemap_hdr_shoulder(c.z, p.hdr_lmax)};
        } else {
            p.color[idx.linear] = Vec3{tonemap_channel(c.x, p.op), tonemap_channel(c.y, p.op), tonemap_channel(c.z, p.op)};
        }
    }
};

/// Apply the baked grading LUT (sRGB-encoded domain, tetrahedral). The [0, 1] part of each channel goes
/// through the LUT; HDR headroom above paper white passes through additively (continuous at 1).
struct GradeLutParams {
    kernel::Span<Vec3> color;
    kernel::Span<const Vec3> lut;
    u32 n = 0;
};
struct GradeLutKernel {
    static constexpr const char* kName = "look_color_grade_lut";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const GradeLutParams& p) const {
        const Vec3 c = max3(p.color[idx.linear], 0.f);
        const Vec3 base = min3(c, 1.f);
        const Vec3 graded = srgb_decode3(saturate3(lut_sample_tetrahedral(p.lut.data, p.n, srgb_encode3(base))));
        p.color[idx.linear] = graded + (c - base);
    }
};

/// Contrast-adaptive sharpening (FUSE fallback; a registered CAS hook replaces it): 5-tap cross,
/// negative lobe scaled by local headroom so already-contrasty edges are not over-sharpened.
struct SharpenParams {
    kernel::Span<const Vec3> src;
    kernel::Span<Vec3> dst;
    u32 w = 0, h = 0;
    f32 sharpness = 0.5f;
};
struct SharpenKernel {
    static constexpr const char* kName = "look_sharpen";
    FUSE_HOST_DEVICE static f32 channel(f32 m, f32 n, f32 s, f32 e, f32 w, f32 peak) {
        const f32 mn = std::min(std::min(std::min(m, n), std::min(s, e)), w);
        const f32 mx = std::max(std::max(std::max(m, n), std::max(s, e)), w);
        const f32 amp = std::sqrt(saturate(std::min(saturate(mn), 1.f - saturate(mx)) / std::max(mx, 1e-5f)));
        const f32 wt = amp * peak;
        return std::max((m + wt * (n + s + e + w)) / (1.f + 4.f * wt), 0.f);
    }
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const SharpenParams& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const Vec3 m = p.src[y * p.w + x];
        const Vec3 n = p.src[(y > 0u ? y - 1u : y) * p.w + x];
        const Vec3 s = p.src[(y + 1u < p.h ? y + 1u : y) * p.w + x];
        const Vec3 e = p.src[y * p.w + (x + 1u < p.w ? x + 1u : x)];
        const Vec3 w = p.src[y * p.w + (x > 0u ? x - 1u : x)];
        const f32 peak = -1.f / lerpf(8.f, 5.f, saturate(p.sharpness));
        p.dst[idx.linear] = Vec3{channel(m.x, n.x, s.x, e.x, w.x, peak), channel(m.y, n.y, s.y, e.y, w.y, peak),
                                 channel(m.z, n.z, s.z, e.z, w.z, peak)};
    }
};

/// Radial chromatic aberration: red sampled at c + d (1 + k), blue at c + d (1 - k), green unchanged,
/// where d is the pixel's offset from the frame centre c (pixels) and k = intensity.
struct ChromaticAberrationParams {
    kernel::Span<const Vec3> src;
    kernel::Span<Vec3> dst;
    u32 w = 0, h = 0;
    f32 intensity = 0.f;
};
struct ChromaticAberrationKernel {
    static constexpr const char* kName = "look_chromatic_aberration";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const ChromaticAberrationParams& p) const {
        const f32 cx = 0.5f * static_cast<f32>(p.w);
        const f32 cy = 0.5f * static_cast<f32>(p.h);
        const f32 dx = static_cast<f32>(idx.global.x) + 0.5f - cx;
        const f32 dy = static_cast<f32>(idx.global.y) + 0.5f - cy;
        const f32 r = sample_bilinear(p.src.data, p.w, p.h, cx + dx * (1.f + p.intensity), cy + dy * (1.f + p.intensity)).x;
        const f32 b = sample_bilinear(p.src.data, p.w, p.h, cx + dx * (1.f - p.intensity), cy + dy * (1.f - p.intensity)).z;
        p.dst[idx.linear] = Vec3{r, p.src[idx.linear].y, b};
    }
};

/// color = color * gain + tint * (1 - gain)
struct VignetteParams {
    kernel::Span<Vec3> color;
    u32 w = 0, h = 0;
    f32 intensity = 0.f, falloff = 2.f, roundness = 0.f;
    Vec3 tint{};
};
struct VignetteKernel {
    static constexpr const char* kName = "look_vignette";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const VignetteParams& p) const {
        const f32 g = vignette_gain(p.intensity, p.falloff, p.roundness, idx.global.x, idx.global.y, p.w, p.h);
        p.color[idx.linear] = p.color[idx.linear] * g + p.tint * (1.f - g);
    }
};

/// Additive luminance grain, amplitude intensity * (1 - response * saturate(luma)).
struct FilmGrainParams {
    kernel::Span<Vec3> color;
    u64 frame_seed = 0;
    f32 intensity = 0.f;
    f32 response = 0.f;
};
struct FilmGrainKernel {
    static constexpr const char* kName = "look_film_grain";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const FilmGrainParams& p) const {
        const Vec3 c = p.color[idx.linear];
        const f32 amp = p.intensity * (1.f - p.response * saturate(luminance709(c)));
        const f32 g = grain_noise(p.frame_seed, idx.global.x, idx.global.y) * amp;
        p.color[idx.linear] = c + Vec3{g, g, g};
    }
};

enum : u32 { kEncodeSrgb = 0u, kEncodeHdr10Pq = 1u, kEncodeScRgb = 2u };

/// Output transform. Input: display-linear Rec.709, 1.0 = SDR white / HDR paper white.
///  sRGB:   saturate, sRGB OETF.
///  HDR10:  Rec.709 -> Rec.2020, nits = v * paper_white clamped to [0, peak], ST 2084 PQ.
///  scRGB:  linear Rec.709, value = v * paper_white / 80, clamped to peak / 80 (negatives kept).
struct OutputEncodeParams {
    kernel::Span<const Vec3> src;
    kernel::Span<Vec3> dst;
    u32 mode = kEncodeSrgb;
    f32 paper_white_nits = 200.f;
    f32 peak_nits = 1000.f;
};
FUSE_HOST_DEVICE inline Vec3 output_encode(const Vec3& c, u32 mode, f32 paper_white, f32 peak) {
    if (mode == kEncodeHdr10Pq) {
        const Vec3 w = rec709_to_rec2020(c);
        return Vec3{pq_encode_nits(std::min(std::max(w.x * paper_white, 0.f), peak)),
                    pq_encode_nits(std::min(std::max(w.y * paper_white, 0.f), peak)),
                    pq_encode_nits(std::min(std::max(w.z * paper_white, 0.f), peak))};
    }
    if (mode == kEncodeScRgb) {
        const f32 k = paper_white / kScRgbReferenceNits;
        const f32 hi = peak / kScRgbReferenceNits;
        return Vec3{std::min(c.x * k, hi), std::min(c.y * k, hi), std::min(c.z * k, hi)};
    }
    return srgb_encode3(saturate3(c));
}
struct OutputEncodeKernel {
    static constexpr const char* kName = "look_output_encode";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const OutputEncodeParams& p) const {
        p.dst[idx.linear] = output_encode(p.src[idx.linear], p.mode, p.paper_white_nits, p.peak_nits);
    }
};

/// LUT bake: one item per lattice point of an n^3 LUT in the sRGB-encoded domain.
///   lin = decode(x); lin = grade(lin); e = encode(saturate(lin)); e = curves(e);
///   ext = identity_weight * e + sum_k weight_k * tetra(ext_k, e);   out = e + (ext - e) * strength
struct LutBakeParams {
    kernel::Span<Vec3> dst;
    u32 n = 0;
    GradeParams grade{};
    u32 ext_count = 0;
    kernel::Span<const Vec3> ext[kMaxLutBlend] = {};
    f32 ext_weight[kMaxLutBlend] = {};
    f32 identity_weight = 1.f;
    f32 strength = 1.f;
};
FUSE_HOST_DEVICE inline Vec3 lut_bake_point(const LutBakeParams& p, const Vec3& x) {
    Vec3 e = x;
    if (p.grade.flags & (kGradeWhiteBalance | kGradeLiftGain | kGradeGamma | kGradeContrast | kGradeSaturation)) {
        e = srgb_encode3(saturate3(grade_linear(srgb_decode3(x), p.grade)));
    }
    e = grade_curves(e, p.grade);
    if (p.ext_count == 0u) {
        return e;
    }
    Vec3 ext = e * p.identity_weight;
    for (u32 k = 0; k < p.ext_count; ++k) {
        ext = ext + lut_sample_tetrahedral(p.ext[k].data, p.n, e) * p.ext_weight[k];
    }
    return p.strength >= 1.f ? ext : e + (ext - e) * p.strength;
}
struct LutBakeKernel {
    static constexpr const char* kName = "look_lut_bake";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const LutBakeParams& p) const {
        const u32 n = p.n;
        const u32 r = idx.linear % n;
        const u32 g = (idx.linear / n) % n;
        const u32 b = idx.linear / (n * n);
        const f32 inv = 1.f / static_cast<f32>(n - 1u);
        p.dst[idx.linear] = lut_bake_point(p, Vec3{static_cast<f32>(r) * inv, static_cast<f32>(g) * inv,
                                                   static_cast<f32>(b) * inv});
    }
};

/// Weighted LUT blend: dst = sum_k weight_k * src_k (lattice-wise; all LUTs share `n`).
struct LutBlendParams {
    kernel::Span<Vec3> dst;
    u32 count = 0;
    kernel::Span<const Vec3> src[kMaxLutBlend] = {};
    f32 weight[kMaxLutBlend] = {};
};
struct LutBlendKernel {
    static constexpr const char* kName = "look_lut_blend";
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const LutBlendParams& p) const {
        Vec3 acc{};
        for (u32 k = 0; k < p.count; ++k) {
            acc = acc + p.src[k][idx.linear] * p.weight[k];
        }
        p.dst[idx.linear] = acc;
    }
};

} // namespace fuse::renderer::look::kernels
