#pragma once

// Single-source image-quality kernels (docs/compute-kernels.md): the per-pixel work of PSNR, SSIM, LDR-FLIP
// and the temporal (flicker) metric. Every function here is FUSE_HOST_DEVICE and is the ONLY implementation;
// the host API (image_metrics.hpp) launches these bodies and reduces their per-pixel maps in a fixed serial
// order (double precision), so CpuReference and CpuParallel give bit-identical scores.
//
// References:
//   * SSIM — Z. Wang, A. C. Bovik, H. R. Sheikh, E. P. Simoncelli, "Image Quality Assessment: From Error
//     Visibility to Structural Similarity", IEEE TIP 13(4), 2004. K1 = 0.01, K2 = 0.03, L = 1; 11x11
//     circular-symmetric Gaussian window (sigma 1.5) or the 8x8 square window of the paper's first
//     experiments; statistics over windows fully inside the image (MATLAB `ssim_index` 'valid').
//   * LDR-FLIP — P. Andersson, J. Nilsson, T. Akenine-Moller, M. Oskarsson, K. Astrom, M. D. Fairchild,
//     "FLIP: A Difference Evaluator for Alternating Images", Proc. ACM Comput. Graph. Interact. Tech. (HPG)
//     3(2), 2020. Implemented from the paper and the BSD-3-Clause reference implementation
//     (https://github.com/NVlabs/flip, src/cpp/FLIP.h, Copyright (c) 2020-2025 NVIDIA CORPORATION &
//     AFFILIATES): constants, the separated CSF / feature filters ("separatedConvolutions.pdf") and the
//     clamp-to-edge borders follow that implementation, so mean errors match the official `flip_evaluator`
//     to ~1e-5 (see test_render_quality_metrics.cpp).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::quality {

inline constexpr const char* kPrepareName = "quality_prepare";
inline constexpr const char* kSsimName = "quality_ssim";
inline constexpr const char* kTemporalName = "quality_temporal";
inline constexpr const char* kFlipConvertName = "flip_convert";
inline constexpr const char* kFlipFilterXName = "flip_filter_x";
inline constexpr const char* kFlipFilterYName = "flip_filter_y";
inline constexpr kernel::Dim3 kWorkgroup{8u, 8u, 1u};

/// Largest SSIM window (11x11 Gaussian).
inline constexpr u32 kMaxSsimWindow = 11u;
/// Largest FLIP filter radius (spatial radius = ceil(0.135 ppd), feature radius = ceil(0.123 ppd)): ppd <= 230.
inline constexpr i32 kMaxFlipRadius = 32;
inline constexpr i32 kMaxFlipTaps = 2 * kMaxFlipRadius + 1;

// ---------------------------------------------------------------------------------------------------------
// Colour helpers
// ---------------------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline f32 clamp01(f32 v) { return std::min(std::max(v, 0.f), 1.f); }

/// sRGB OETF (IEC 61966-2-1).
FUSE_HOST_DEVICE inline f32 srgb_encode(f32 l) {
    return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;
}
FUSE_HOST_DEVICE inline f32 srgb_decode(f32 s) {
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

/// Display-referred [0, 1] value used by every metric: `linearInput` images are exposed, clamped and
/// sRGB-encoded; display images are only clamped.
FUSE_HOST_DEVICE inline math::Vec3 to_display(const math::Vec3& c, bool linearInput, f32 exposure) {
    if (!linearInput) {
        return {clamp01(c.x), clamp01(c.y), clamp01(c.z)};
    }
    return {srgb_encode(clamp01(c.x * exposure)), srgb_encode(clamp01(c.y * exposure)),
            srgb_encode(clamp01(c.z * exposure))};
}

/// Rec. 601 luma of display-encoded RGB (MATLAB rgb2gray; the grey image SSIM is computed on).
FUSE_HOST_DEVICE inline f32 luma601(const math::Vec3& c) { return 0.299f * c.x + 0.587f * c.y + 0.114f * c.z; }

// ---------------------------------------------------------------------------------------------------------
// Prepare: display encoding, luma, per-pixel squared error (PSNR) and absolute luma error (masked metrics)
// ---------------------------------------------------------------------------------------------------------

struct PrepareParams {
    kernel::Span<const math::Vec3> a{};
    kernel::Span<const math::Vec3> b{};
    kernel::Span<f32> luma_a{};
    kernel::Span<f32> luma_b{};
    kernel::Span<f32> sq_error{};  ///< mean over RGB of (a - b)^2 (display values)
    kernel::Span<f32> abs_luma{};  ///< |luma(a) - luma(b)|
    u32 width = 0;
    u32 height = 0;
    f32 exposure = 1.f;
    u32 linear_input = 0;
};

struct PrepareKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const PrepareParams& p) const {
        const u32 i = idx.global.y * p.width + idx.global.x;
        const bool lin = p.linear_input != 0u;
        const math::Vec3 a = to_display(p.a[i], lin, p.exposure);
        const math::Vec3 b = to_display(p.b[i], lin, p.exposure);
        const math::Vec3 d = a - b;
        const f32 la = luma601(a);
        const f32 lb = luma601(b);
        p.luma_a[i] = la;
        p.luma_b[i] = lb;
        p.sq_error[i] = (d.x * d.x + d.y * d.y + d.z * d.z) * (1.f / 3.f);
        p.abs_luma[i] = std::fabs(la - lb);
    }
};

// ---------------------------------------------------------------------------------------------------------
// SSIM (one item per valid window position; map is (width - win + 1) x (height - win + 1))
// ---------------------------------------------------------------------------------------------------------

struct SsimParams {
    kernel::Span<const f32> x{};
    kernel::Span<const f32> y{};
    kernel::Span<f32> map{};
    u32 width = 0;
    u32 height = 0;
    u32 window = kMaxSsimWindow;
    f32 weights[kMaxSsimWindow] = {}; ///< Separable 1D weights (sum 1): Gaussian or box.
    f32 c1 = 0.0001f;                 ///< (K1 L)^2
    f32 c2 = 0.0009f;                 ///< (K2 L)^2
};

FUSE_HOST_DEVICE inline f32 ssim_from_moments(f32 mx, f32 my, f32 sxx, f32 syy, f32 sxy, f32 c1, f32 c2) {
    return ((2.f * mx * my + c1) * (2.f * sxy + c2)) / ((mx * mx + my * my + c1) * (sxx + syy + c2));
}

struct SsimKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const SsimParams& p) const {
        const u32 ox = idx.global.x;
        const u32 oy = idx.global.y;
        // Shifted moments (relative to the window's centre sample): E[x^2] - E[x]^2 in f32 cancels catastrophically
        // on flat regions (errors ~1e-6 against C2 = 9e-4); shifting by a sample of the window keeps it exact there.
        const u32 centre = (oy + p.window / 2u) * p.width + ox + p.window / 2u;
        const f32 kx = p.x[centre];
        const f32 ky = p.y[centre];
        f32 mx = 0.f;
        f32 my = 0.f;
        f32 mxx = 0.f;
        f32 myy = 0.f;
        f32 mxy = 0.f;
        f32 wsum = 0.f;
        for (u32 j = 0; j < p.window; ++j) {
            const u32 row = (oy + j) * p.width + ox;
            for (u32 i = 0; i < p.window; ++i) {
                const f32 w = p.weights[i] * p.weights[j];
                const f32 a = p.x[row + i] - kx;
                const f32 b = p.y[row + i] - ky;
                wsum += w;
                mx += w * a;
                my += w * b;
                mxx += w * a * a;
                myy += w * b * b;
                mxy += w * a * b;
            }
        }
        const f32 inv = 1.f / wsum;
        mx *= inv;
        my *= inv;
        const f32 sxx = mxx * inv - mx * mx;
        const f32 syy = myy * inv - my * my;
        const f32 sxy = mxy * inv - mx * my;
        p.map[idx.linear] = ssim_from_moments(mx + kx, my + ky, sxx, syy, sxy, p.c1, p.c2);
    }
};

// ---------------------------------------------------------------------------------------------------------
// Temporal: per-pixel squared difference of the frame-to-frame change of test vs reference (tPSNR)
// ---------------------------------------------------------------------------------------------------------

struct TemporalParams {
    kernel::Span<const math::Vec3> test_prev{};
    kernel::Span<const math::Vec3> test_cur{};
    kernel::Span<const math::Vec3> ref_prev{};
    kernel::Span<const math::Vec3> ref_cur{};
    kernel::Span<f32> sq_error{};
    u32 width = 0;
    u32 height = 0;
    f32 exposure = 1.f;
    u32 linear_input = 0;
};

struct TemporalKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const TemporalParams& p) const {
        const u32 i = idx.global.y * p.width + idx.global.x;
        const bool lin = p.linear_input != 0u;
        const math::Vec3 dt = to_display(p.test_cur[i], lin, p.exposure) - to_display(p.test_prev[i], lin, p.exposure);
        const math::Vec3 dr = to_display(p.ref_cur[i], lin, p.exposure) - to_display(p.ref_prev[i], lin, p.exposure);
        const math::Vec3 e = dt - dr;
        p.sq_error[i] = (e.x * e.x + e.y * e.y + e.z * e.z) * (1.f / 3.f);
    }
};

// ---------------------------------------------------------------------------------------------------------
// LDR-FLIP (colour pipeline + feature pipeline), after FLIP.h (BSD-3-Clause, NVIDIA)
// ---------------------------------------------------------------------------------------------------------

inline constexpr f32 kFlipQc = 0.7f;
inline constexpr f32 kFlipPc = 0.4f;
inline constexpr f32 kFlipPt = 0.95f;
inline constexpr f32 kFlipGw = 0.082f; ///< Feature filter width in degrees.
inline constexpr f32 kFlipQf = 0.5f;

/// D65 reference white and its inverse (FLIP.h DEFAULT_ILLUMINANT / INV_DEFAULT_ILLUMINANT).
inline constexpr f32 kD65X = 0.950428545f;
inline constexpr f32 kD65Z = 1.088900371f;
inline constexpr f32 kInvD65X = 1.052156925f;
inline constexpr f32 kInvD65Z = 0.918357670f;

FUSE_HOST_DEVICE inline math::Vec3 flip_linear_rgb_to_xyz(const math::Vec3& c) {
    const f32 a11 = 10135552.0f / 24577794.0f;
    const f32 a12 = 8788810.0f / 24577794.0f;
    const f32 a13 = 4435075.0f / 24577794.0f;
    const f32 a21 = 2613072.0f / 12288897.0f;
    const f32 a22 = 8788810.0f / 12288897.0f;
    const f32 a23 = 887015.0f / 12288897.0f;
    const f32 a31 = 1425312.0f / 73733382.0f;
    const f32 a32 = 8788810.0f / 73733382.0f;
    const f32 a33 = 70074185.0f / 73733382.0f;
    return {a11 * c.x + a12 * c.y + a13 * c.z, a21 * c.x + a22 * c.y + a23 * c.z, a31 * c.x + a32 * c.y + a33 * c.z};
}

FUSE_HOST_DEVICE inline math::Vec3 flip_xyz_to_linear_rgb(const math::Vec3& c) {
    return {3.241003275f * c.x - 1.537398934f * c.y - 0.498615861f * c.z,
            -0.969224334f * c.x + 1.875930071f * c.y + 0.041554224f * c.z,
            0.055639423f * c.x - 0.204011202f * c.y + 1.057148933f * c.z};
}

FUSE_HOST_DEVICE inline math::Vec3 flip_xyz_to_ycxcz(const math::Vec3& xyz) {
    const f32 x = xyz.x * kInvD65X;
    const f32 y = xyz.y;
    const f32 z = xyz.z * kInvD65Z;
    return {116.f * y - 16.f, 500.f * (x - y), 200.f * (y - z)};
}

FUSE_HOST_DEVICE inline math::Vec3 flip_ycxcz_to_xyz(const math::Vec3& c) {
    const f32 y = (c.x + 16.f) / 116.f;
    const f32 cx = c.y / 500.f;
    const f32 cz = c.z / 200.f;
    return {(y + cx) * kD65X, y, (y - cz) * kD65Z};
}

FUSE_HOST_DEVICE inline math::Vec3 flip_xyz_to_lab(const math::Vec3& xyzIn) {
    const f32 delta = 6.0f / 29.0f;
    const f32 deltaSquare = delta * delta;
    const f32 deltaCube = delta * deltaSquare;
    const f32 factor = 1.0f / (3.0f * deltaSquare);
    const f32 term = 4.0f / 29.0f;
    f32 x = xyzIn.x * kInvD65X;
    f32 y = xyzIn.y;
    f32 z = xyzIn.z * kInvD65Z;
    x = x > deltaCube ? std::pow(x, 1.0f / 3.0f) : factor * x + term;
    y = y > deltaCube ? std::pow(y, 1.0f / 3.0f) : factor * y + term;
    z = z > deltaCube ? std::pow(z, 1.0f / 3.0f) : factor * z + term;
    return {116.0f * y - 16.0f, 500.0f * (x - y), 200.0f * (y - z)};
}

/// Hunt-adjusted CIELAB of linear RGB.
FUSE_HOST_DEVICE inline math::Vec3 flip_hunt_lab(const math::Vec3& linearRgb) {
    const math::Vec3 lab = flip_xyz_to_lab(flip_linear_rgb_to_xyz(linearRgb));
    return {lab.x, 0.01f * lab.x * lab.y, 0.01f * lab.x * lab.z};
}

FUSE_HOST_DEVICE inline f32 flip_hyab(const math::Vec3& a, const math::Vec3& b) {
    const f32 dy = a.y - b.y;
    const f32 dz = a.z - b.z;
    return std::fabs(a.x - b.x) + std::sqrt(dy * dy + dz * dz);
}

/// Separated FLIP filters for one pixels-per-degree value (built on the host by `make_flip_filters`).
struct FlipFilters {
    i32 spatial_radius = 0;
    i32 feature_radius = 0;
    f32 y[kMaxFlipTaps] = {};   ///< Achromatic CSF (normalised).
    f32 cx[kMaxFlipTaps] = {};  ///< Red-green CSF (normalised).
    f32 cz1[kMaxFlipTaps] = {}; ///< Blue-yellow CSF, first Gaussian (sqrt-separated).
    f32 cz2[kMaxFlipTaps] = {}; ///< Blue-yellow CSF, second Gaussian (sqrt-separated).
    f32 g[kMaxFlipTaps] = {};   ///< Feature Gaussian.
    f32 dg[kMaxFlipTaps] = {};  ///< 1st derivative (edges).
    f32 ddg[kMaxFlipTaps] = {}; ///< 2nd derivative (points).
    f32 cmax = 1.f;
    f32 pccmax = 0.4f;
};

/// Builds the filters exactly as FLIP.h setSpatialFilters / setFeatureFilter (host side, once per ppd).
inline FlipFilters make_flip_filters(f32 ppd) {
    constexpr f32 kPi = 3.14159265358979f;
    const f32 piSq = kPi * kPi;
    FlipFilters f{};
    // Spatial (CSF) filters. Gaussian constants: a1 = {1, 1, 34.1}, b1 = {0.0047, 0.0053, 0.04},
    // a2 = {0, 0, 13.5}, b2 = {1e-5, 1e-5, 0.025}; radius from the largest scale parameter (0.04).
    const f32 maxB = 0.04f;
    f.spatial_radius = std::min(kMaxFlipRadius, static_cast<i32>(std::ceil(3.0f * std::sqrt(maxB / (2.0f * piSq)) * ppd)));
    const f32 deltaX = 1.0f / ppd;
    const auto gauss = [&](f32 x2, f32 a, f32 b) { return a * std::sqrt(kPi / b) * std::exp(-piSq * x2 / b); };
    const auto gaussSqrt = [&](f32 x2, f32 a, f32 b) { return std::sqrt(a * std::sqrt(kPi / b)) * std::exp(-piSq * x2 / b); };
    f32 sumY = 0.f;
    f32 sumCx = 0.f;
    f32 sumCz1 = 0.f;
    f32 sumCz2 = 0.f;
    const i32 sw = 2 * f.spatial_radius + 1;
    for (i32 x = 0; x < sw; ++x) {
        const f32 ix = static_cast<f32>(x - f.spatial_radius) * deltaX;
        const f32 ix2 = ix * ix;
        f.y[x] = gauss(ix2, 1.0f, 0.0047f);
        f.cx[x] = gauss(ix2, 1.0f, 0.0053f);
        f.cz1[x] = gaussSqrt(ix2, 34.1f, 0.04f);
        f.cz2[x] = gaussSqrt(ix2, 13.5f, 0.025f);
        sumY += f.y[x];
        sumCx += f.cx[x];
        sumCz1 += f.cz1[x];
        sumCz2 += f.cz2[x];
    }
    const f32 normY = 1.0f / sumY;
    const f32 normCx = 1.0f / sumCx;
    const f32 normCz = 1.0f / std::sqrt(sumCz1 * sumCz1 + sumCz2 * sumCz2);
    for (i32 x = 0; x < sw; ++x) {
        f.y[x] *= normY;
        f.cx[x] *= normCx;
        f.cz1[x] *= normCz;
        f.cz2[x] *= normCz;
    }

    // Feature filters: Gaussian of std dev 0.5 * gw * ppd and its 1st / 2nd derivatives, positive and negative
    // lobes normalised separately (to 1 and -1).
    const f32 stdDev = 0.5f * kFlipGw * ppd;
    f.feature_radius = std::min(kMaxFlipRadius, static_cast<i32>(std::ceil(3.0f * stdDev)));
    const i32 fw = 2 * f.feature_radius + 1;
    f32 gSum = 0.f;
    f32 dgPos = 0.f;
    f32 dgNeg = 0.f;
    f32 ddgPos = 0.f;
    f32 ddgNeg = 0.f;
    for (i32 x = 0; x < fw; ++x) {
        const f32 xx = static_cast<f32>(x - f.feature_radius);
        const f32 g = std::exp(-(xx * xx) / (2.0f * stdDev * stdDev));
        gSum += g;
        const f32 dg = -xx * g;
        if (dg > 0.f) {
            dgPos += dg;
        } else {
            dgNeg -= dg;
        }
        const f32 ddg = (xx * xx / (stdDev * stdDev) - 1.0f) * g;
        if (ddg > 0.f) {
            ddgPos += ddg;
        } else {
            ddgNeg -= ddg;
        }
        f.g[x] = g;
        f.dg[x] = dg;
        f.ddg[x] = ddg;
    }
    for (i32 x = 0; x < fw; ++x) {
        f.g[x] /= gSum;
        f.dg[x] /= (f.dg[x] > 0.f ? dgPos : dgNeg);
        f.ddg[x] /= (f.ddg[x] > 0.f ? ddgPos : ddgNeg);
    }

    // Colour-difference normalisation: HyAB distance of Hunt-adjusted green and blue, raised to qc.
    f.cmax = std::pow(flip_hyab(flip_hunt_lab({0.f, 1.f, 0.f}), flip_hunt_lab({0.f, 0.f, 1.f})), kFlipQc);
    f.pccmax = kFlipPc * f.cmax;
    return f;
}

/// Display sRGB (clamped) -> linear -> YCxCz for reference and test.
struct FlipConvertParams {
    kernel::Span<const math::Vec3> ref{};
    kernel::Span<const math::Vec3> test{};
    kernel::Span<math::Vec3> ref_ycxcz{};
    kernel::Span<math::Vec3> test_ycxcz{};
    u32 width = 0;
    u32 height = 0;
    f32 exposure = 1.f;
    u32 linear_input = 0;
};

FUSE_HOST_DEVICE inline math::Vec3 flip_display_to_ycxcz(const math::Vec3& display) {
    const math::Vec3 lin{srgb_decode(display.x), srgb_decode(display.y), srgb_decode(display.z)};
    return flip_xyz_to_ycxcz(flip_linear_rgb_to_xyz(lin));
}

struct FlipConvertKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const FlipConvertParams& p) const {
        const u32 i = idx.global.y * p.width + idx.global.x;
        const bool lin = p.linear_input != 0u;
        p.ref_ycxcz[i] = flip_display_to_ycxcz(to_display(p.ref[i], lin, p.exposure));
        p.test_ycxcz[i] = flip_display_to_ycxcz(to_display(p.test[i], lin, p.exposure));
    }
};

/// Horizontal pass: CSF (Y, Cx, Cz1, Cz2) and features (dx, ddx, g) of normalised Y, for both images.
struct FlipFilterXParams {
    kernel::Span<const math::Vec3> ref_ycxcz{};
    kernel::Span<const math::Vec3> test_ycxcz{};
    kernel::Span<math::Vec4> ref_color{};
    kernel::Span<math::Vec4> test_color{};
    kernel::Span<math::Vec3> ref_feature{};
    kernel::Span<math::Vec3> test_feature{};
    u32 width = 0;
    u32 height = 0;
    FlipFilters filters{}; ///< By value: kernel params are copied to the device (about 1.8 KiB, under the 4 KiB CUDA limit).
};

struct FlipFilterXKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const FlipFilterXParams& p) const {
        const FlipFilters& f = p.filters;
        const i32 x = static_cast<i32>(idx.global.x);
        const u32 row = idx.global.y * p.width;
        const i32 maxX = static_cast<i32>(p.width) - 1;
        math::Vec4 cr{};
        math::Vec4 ct{};
        for (i32 ix = -f.spatial_radius; ix <= f.spatial_radius; ++ix) {
            const i32 xx = std::min(std::max(0, x + ix), maxX);
            const i32 k = ix + f.spatial_radius;
            const math::Vec3 r = p.ref_ycxcz[row + static_cast<u32>(xx)];
            const math::Vec3 t = p.test_ycxcz[row + static_cast<u32>(xx)];
            cr.x += f.y[k] * r.x;
            cr.y += f.cx[k] * r.y;
            cr.z += f.cz1[k] * r.z;
            cr.w += f.cz2[k] * r.z;
            ct.x += f.y[k] * t.x;
            ct.y += f.cx[k] * t.y;
            ct.z += f.cz1[k] * t.z;
            ct.w += f.cz2[k] * t.z;
        }
        const f32 oneOver116 = 1.0f / 116.0f;
        const f32 sixteenOver116 = 16.0f / 116.0f;
        math::Vec3 fr{};
        math::Vec3 ft{};
        for (i32 ix = -f.feature_radius; ix <= f.feature_radius; ++ix) {
            const i32 xx = std::min(std::max(0, x + ix), maxX);
            const i32 k = ix + f.feature_radius;
            const f32 yr = p.ref_ycxcz[row + static_cast<u32>(xx)].x * oneOver116 + sixteenOver116;
            const f32 yt = p.test_ycxcz[row + static_cast<u32>(xx)].x * oneOver116 + sixteenOver116;
            fr.x += f.dg[k] * yr;
            fr.y += f.ddg[k] * yr;
            fr.z += f.g[k] * yr;
            ft.x += f.dg[k] * yt;
            ft.y += f.ddg[k] * yt;
            ft.z += f.g[k] * yt;
        }
        const u32 i = row + idx.global.x;
        p.ref_color[i] = cr;
        p.test_color[i] = ct;
        p.ref_feature[i] = fr;
        p.test_feature[i] = ft;
    }
};

/// Vertical pass + colour difference + feature difference + final per-pixel FLIP error.
struct FlipFilterYParams {
    kernel::Span<const math::Vec4> ref_color{};
    kernel::Span<const math::Vec4> test_color{};
    kernel::Span<const math::Vec3> ref_feature{};
    kernel::Span<const math::Vec3> test_feature{};
    kernel::Span<f32> error{};
    u32 width = 0;
    u32 height = 0;
    FlipFilters filters{}; ///< By value: kernel params are copied to the device (about 1.8 KiB, under the 4 KiB CUDA limit).
};

FUSE_HOST_DEVICE inline math::Vec3 flip_filtered_to_hunt_lab(const math::Vec4& filtered) {
    const math::Vec3 ycxcz{filtered.x, filtered.y, filtered.z + filtered.w};
    const math::Vec3 rgb = flip_xyz_to_linear_rgb(flip_ycxcz_to_xyz(ycxcz));
    return flip_hunt_lab({clamp01(rgb.x), clamp01(rgb.y), clamp01(rgb.z)});
}

struct FlipFilterYKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const FlipFilterYParams& p) const {
        const FlipFilters& f = p.filters;
        const i32 y = static_cast<i32>(idx.global.y);
        const u32 x = idx.global.x;
        const i32 maxY = static_cast<i32>(p.height) - 1;

        math::Vec4 cr{};
        math::Vec4 ct{};
        for (i32 iy = -f.spatial_radius; iy <= f.spatial_radius; ++iy) {
            const u32 yy = static_cast<u32>(std::min(std::max(0, y + iy), maxY));
            const i32 k = iy + f.spatial_radius;
            const math::Vec4 r = p.ref_color[yy * p.width + x];
            const math::Vec4 t = p.test_color[yy * p.width + x];
            cr.x += f.y[k] * r.x;
            cr.y += f.cx[k] * r.y;
            cr.z += f.cz1[k] * r.z;
            cr.w += f.cz2[k] * r.w;
            ct.x += f.y[k] * t.x;
            ct.y += f.cx[k] * t.y;
            ct.z += f.cz1[k] * t.z;
            ct.w += f.cz2[k] * t.w;
        }
        f32 colorDifference = std::pow(flip_hyab(flip_filtered_to_hunt_lab(cr), flip_filtered_to_hunt_lab(ct)), kFlipQc);
        if (colorDifference < f.pccmax) {
            colorDifference *= kFlipPt / f.pccmax;
        } else {
            colorDifference = kFlipPt + ((colorDifference - f.pccmax) / (f.cmax - f.pccmax)) * (1.0f - kFlipPt);
        }

        f32 dxR = 0.f, dxT = 0.f, ddxR = 0.f, ddxT = 0.f, dyR = 0.f, dyT = 0.f, ddyR = 0.f, ddyT = 0.f;
        for (i32 iy = -f.feature_radius; iy <= f.feature_radius; ++iy) {
            const u32 yy = static_cast<u32>(std::min(std::max(0, y + iy), maxY));
            const i32 k = iy + f.feature_radius;
            const math::Vec3 r = p.ref_feature[yy * p.width + x];
            const math::Vec3 t = p.test_feature[yy * p.width + x];
            dxR += f.g[k] * r.x;
            dxT += f.g[k] * t.x;
            ddxR += f.g[k] * r.y;
            ddxT += f.g[k] * t.y;
            dyR += f.dg[k] * r.z;
            dyT += f.dg[k] * t.z;
            ddyR += f.ddg[k] * r.z;
            ddyT += f.ddg[k] * t.z;
        }
        const f32 edgeR = std::sqrt(dxR * dxR + dyR * dyR);
        const f32 edgeT = std::sqrt(dxT * dxT + dyT * dyT);
        const f32 pointR = std::sqrt(ddxR * ddxR + ddyR * ddyR);
        const f32 pointT = std::sqrt(ddxT * ddxT + ddyT * ddyT);
        const f32 featureDifference =
            std::pow((1.0f / std::sqrt(2.0f)) * std::max(std::fabs(edgeR - edgeT), std::fabs(pointR - pointT)), kFlipQf);
        p.error[idx.global.y * p.width + x] = std::pow(colorDifference, 1.0f - featureDifference);
    }
};

} // namespace fuse::renderer::quality
