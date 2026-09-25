#pragma once

// WP-6.4 SVGF / A-SVGF, CPU reference as single-source kernels (docs/compute-kernels.md), one pixel (or one
// 3 x 3 gradient stratum) per item. shaders/denoise/dn_*.{comp,slang} (+ dn_common.{glsl,slang}) are
// line-for-line twins: same operations in the same order (GLSL `precise`, Slang -fp-mode precise), so the only
// GPU / CPU differences come from exp / sqrt / division rounding (fuse_rp_denoise documents the tolerance).
// Records and constants: denoise_types.hpp. Papers: [SVGF] Schied et al. HPG 2017, [A-SVGF] Schied, Peters,
// Dachsbacher HPG 2018 (full references in denoise_types.hpp).
//
// Passes of one frame (svgf_reference.hpp runs them in this order, SvgfDenoiser records them on the graph):
//   guide            normal + linear depth -> guide (n.xyz, z) and the screen-space depth gradient
//   gradient.prepare (A-SVGF) producer samples -> (delta = cur - prev, max(|cur|, |prev|), valid)
//   gradient.atrous  (A-SVGF) edge-aware a-trous on the stratum grid (relative depth + normal stopping); delta and the
//                    normaliser are filtered separately and lambda = min(1, scale |delta| / max) at the end
//   temporal         bilinear reprojection through the motion vectors; each of the 4 taps must lie inside the
//                    image and pass the depth / normal consistency tests (disocclusion = no valid tap); colour
//                    and luminance moments are blended with alpha = max(alpha_min, 1 / length) ([SVGF] 4.1),
//                    and with A-SVGF alpha' = (1 - lambda) alpha + lambda, length' = max(1, (1 - lambda) length)
//   variance         length < 4: 7 x 7 bilateral estimate of colour and moments, variance boosted by 4 / length;
//                    else variance = max(0, m2 - m1^2) ([SVGF] 4.2)
//   atrous (x N)     5 x 5 B3-spline a-trous with step 2^i; weight = h(dx) h(dy) max(0, n_p.n_q)^sigma_n
//                    exp(-|z_p - z_q| / (sigma_z |grad z . (p - q)| + eps) - |l_p - l_q| / (sigma_l sqrt(g3x3(var))
//                    + 1e-6)); variance filtered with squared weights ([SVGF] 4.3, 4.4). The result of iteration
//                    historyTap becomes the colour history ([SVGF] 4.1: "the first filtered iteration").

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/denoise/denoise_types.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer::denoise::svgf_kernel {

/// std430 records (16 / 8 bytes, as the GPU buffers hold them).
struct F4 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 0.f;
};
struct F2 {
    f32 x = 0.f;
    f32 y = 0.f;
};
static_assert(sizeof(F4) == 16 && sizeof(F2) == 8, "svgf records");

inline constexpr const char* kNameGuide = "denoise_guide";
inline constexpr const char* kNameGradientPrepare = "denoise_gradient_prepare";
inline constexpr const char* kNameGradientAtrous = "denoise_gradient_atrous";
inline constexpr const char* kNameTemporal = "denoise_temporal";
inline constexpr const char* kNameVariance = "denoise_variance";
inline constexpr const char* kNameAtrous = "denoise_atrous";

/// 1D B3-spline a-trous taps by |offset| (exact binary fractions: 3/8, 1/4, 1/16).
FUSE_HOST_DEVICE inline f32 atrous_tap(s32 d) {
    const s32 a = d < 0 ? -d : d;
    return a == 0 ? 0.375f : (a == 1 ? 0.25f : 0.0625f);
}
/// 1D 3-tap Gaussian (1/4, 1/2, 1/4) by |offset|.
FUSE_HOST_DEVICE inline f32 gauss_tap(s32 d) { return d == 0 ? 0.5f : 0.25f; }

FUSE_HOST_DEVICE inline f32 luminance(const F4& c, u32 flags) {
    if ((flags & kDenoiseFlagScalar) != 0u) {
        return c.x;
    }
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

/// b^e by squaring (exact operation sequence on both sides; e = sigma_n).
FUSE_HOST_DEVICE inline f32 pow_int(f32 b, u32 e) {
    f32 r = 1.f;
    while (e != 0u) {
        if ((e & 1u) != 0u) {
            r = r * b;
        }
        b = b * b;
        e = e >> 1u;
    }
    return r;
}

FUSE_HOST_DEVICE inline f32 dot3(const F4& a, const F4& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

/// Normal weight max(0, n_p.n_q)^sigma_n.
FUSE_HOST_DEVICE inline f32 normal_weight(const F4& p, const F4& q, u32 sigmaNormal) {
    const f32 d = dot3(p, q);
    return pow_int(d > 0.f ? d : 0.f, sigmaNormal);
}

/// Depth term |z_p - z_q| / (sigma_z |grad z . offset| + eps) (the exponent, not the weight).
FUSE_HOST_DEVICE inline f32 depth_term(const DenoiseFrameConstants& c, f32 zp, f32 zq, const F2& g, f32 ox, f32 oy) {
    const f32 proj = g.x * ox + g.y * oy;
    return std::fabs(zp - zq) / (c.sigmaDepth * std::fabs(proj) + c.depthEpsilon);
}

/// Signed octahedral decode + normalisation (the gbuffer.glsl oct_decode_signed operations).
FUSE_HOST_DEVICE inline F4 oct_decode(f32 ox, f32 oy) {
    f32 nx = ox;
    f32 ny = oy;
    const f32 nz = 1.f - std::fabs(ox) - std::fabs(oy);
    if (nz < 0.f) {
        nx = (1.f - std::fabs(oy)) * (ox >= 0.f ? 1.f : -1.f);
        ny = (1.f - std::fabs(ox)) * (oy >= 0.f ? 1.f : -1.f);
    }
    const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    return F4{nx / len, ny / len, nz / len, 0.f};
}

// --- guide -------------------------------------------------------------------------------------------
struct GuideParams {
    DenoiseFrameConstants c{};
    kernel::Span<const f32> depth;  ///< per pixel, linear view depth (0 = sky)
    kernel::Span<const F4> normals; ///< per pixel: xyz unit normal, or oct xy (kDenoiseFlagNormalOct)
    kernel::Span<F4> guide;         ///< out: (n.xyz, z)
    kernel::Span<F2> gradZ;         ///< out: (dz/dx, dz/dy)
};

/// One-sided depth difference of smaller magnitude (sky neighbours skipped; none -> 0).
FUSE_HOST_DEVICE inline f32 depth_slope(f32 z, f32 zm, bool hasM, f32 zp, bool hasP) {
    const f32 dm = z - zm;
    const f32 dp = zp - z;
    if (hasM && hasP) {
        return std::fabs(dm) <= std::fabs(dp) ? dm : dp;
    }
    if (hasM) {
        return dm;
    }
    if (hasP) {
        return dp;
    }
    return 0.f;
}

FUSE_HOST_DEVICE inline void guide_pixel(const GuideParams& p, u32 x, u32 y) {
    const DenoiseFrameConstants& c = p.c;
    const u32 w = c.width;
    const u32 i = y * w + x;
    const f32 z = p.depth[i];
    if (!(z > 0.f)) {
        p.guide[i] = F4{};
        p.gradZ[i] = F2{};
        return;
    }
    F4 n = p.normals[i];
    if ((c.flags & kDenoiseFlagNormalOct) != 0u) {
        n = oct_decode(n.x, n.y);
    }
    n.w = z;
    const f32 zl = x > 0u ? p.depth[i - 1u] : 0.f;
    const f32 zr = x + 1u < w ? p.depth[i + 1u] : 0.f;
    const f32 zu = y > 0u ? p.depth[i - w] : 0.f;
    const f32 zd = y + 1u < c.height ? p.depth[i + w] : 0.f;
    p.guide[i] = n;
    p.gradZ[i] = F2{depth_slope(z, zl, zl > 0.f, zr, zr > 0.f), depth_slope(z, zu, zu > 0.f, zd, zd > 0.f)};
}

struct GuideKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const GuideParams& p) const {
        guide_pixel(p, idx.global.x, idx.global.y);
    }
};

// --- A-SVGF gradients ---------------------------------------------------------------------------------
struct GradientParams {
    DenoiseFrameConstants c{};
    kernel::Span<const F4> guide; ///< current guide (per pixel)
    kernel::Span<const F4> src;   ///< per stratum: producer samples (prepare) or the previous iteration
    kernel::Span<F4> dst;         ///< per stratum
    u32 mode = kDenoiseGradientPrepare;
    u32 step = 1u;
};

/// Representative pixel of a stratum: its centre, clamped into the image.
FUSE_HOST_DEVICE inline u32 stratum_pixel(const DenoiseFrameConstants& c, u32 sx, u32 sy) {
    const u32 px = sx * kDenoiseStratum + 1u < c.width ? sx * kDenoiseStratum + 1u : c.width - 1u;
    const u32 py = sy * kDenoiseStratum + 1u < c.height ? sy * kDenoiseStratum + 1u : c.height - 1u;
    return py * c.width + px;
}

FUSE_HOST_DEVICE inline void gradient_stratum(const GradientParams& p, u32 sx, u32 sy) {
    const DenoiseFrameConstants& c = p.c;
    const u32 s = sy * c.strataW + sx;
    const u32 pi = stratum_pixel(c, sx, sy);
    const F4 g = p.guide[pi];
    const F4 in = p.src[s];
    if (p.mode == kDenoiseGradientPrepare) {
        if (!(in.z > 0.f) || !(g.w > 0.f)) {
            p.dst[s] = F4{};
            return;
        }
        const f32 a = std::fabs(in.x);
        const f32 b = std::fabs(in.y);
        p.dst[s] = F4{in.x - in.y, a > b ? a : b, 1.f, 0.f};
        return;
    }
    if (!(g.w > 0.f)) {
        p.dst[s] = in;
        return;
    }
    // No per-stratum depth gradient: the depth weight is relative, |z_p - z_q| / (sigma_z reprojDepth z_p + eps)
    // (strata are 3 x step pixels apart, a screen-space slope would be as coarse).
    f32 sumW = 0.f;
    f32 sd = 0.f;
    f32 sm = 0.f;
    f32 sv = 0.f;
    for (s32 dy = -2; dy <= 2; ++dy) {
        for (s32 dx = -2; dx <= 2; ++dx) {
            const s32 qx = static_cast<s32>(sx) + dx * static_cast<s32>(p.step);
            const s32 qy = static_cast<s32>(sy) + dy * static_cast<s32>(p.step);
            if (qx < 0 || qy < 0 || qx >= static_cast<s32>(c.strataW) || qy >= static_cast<s32>(c.strataH)) {
                continue;
            }
            const u32 q = static_cast<u32>(qy) * c.strataW + static_cast<u32>(qx);
            const u32 qi = stratum_pixel(c, static_cast<u32>(qx), static_cast<u32>(qy));
            const F4 gq = p.guide[qi];
            if (!(gq.w > 0.f)) {
                continue;
            }
            const f32 h = atrous_tap(dx) * atrous_tap(dy);
            const f32 wz = std::fabs(g.w - gq.w) / (c.sigmaDepth * g.w * c.reprojDepth + c.depthEpsilon);
            const f32 wn = normal_weight(g, gq, c.sigmaNormal);
            const f32 wgt = h * wn * std::exp(-wz);
            const F4 v = p.src[q];
            sumW = sumW + wgt;
            sd = sd + wgt * v.x;
            sm = sm + wgt * v.y;
            sv = sv + wgt * v.z;
        }
    }
    p.dst[s] = F4{sd / sumW, sm / sumW, sv / sumW, 0.f};
}

struct GradientKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const GradientParams& p) const {
        gradient_stratum(p, idx.global.x, idx.global.y);
    }
};

/// lambda of a filtered gradient record ([A-SVGF] eq. 5, clamped).
FUSE_HOST_DEVICE inline f32 gradient_lambda(const DenoiseFrameConstants& c, const F4& g) {
    if (!(g.y > c.gradientEpsilon)) {
        return 0.f;
    }
    const f32 l = c.gradientScale * std::fabs(g.x) / g.y;
    return l < 1.f ? l : 1.f;
}

// --- temporal accumulation ------------------------------------------------------------------------------
struct TemporalParams {
    DenoiseFrameConstants c{};
    kernel::Span<const f32> signal;  ///< signalStride floats per pixel
    kernel::Span<const F2> motion;   ///< UV motion (current - previous)
    kernel::Span<const F4> guideCur;
    kernel::Span<const F4> guidePrev;
    kernel::Span<const F2> gradZ;
    kernel::Span<const F4> histPrev;
    kernel::Span<const F4> momPrev;
    kernel::Span<const F4> lambda;   ///< per stratum (kDenoiseFlagGradients)
    kernel::Span<F4> accum;          ///< out: (rgb, 0)
    kernel::Span<F4> momCur;         ///< out: (m1, m2, length, 0)
};

FUSE_HOST_DEVICE inline F4 read_signal(const DenoiseFrameConstants& c, kernel::Span<const f32> signal, u32 i) {
    if (c.signalStride == 1u) {
        return F4{signal[i], 0.f, 0.f, 0.f};
    }
    const u32 b = i * c.signalStride;
    return F4{signal[b], signal[b + 1u], signal[b + 2u], 0.f};
}

FUSE_HOST_DEVICE inline void temporal_pixel(const TemporalParams& p, u32 x, u32 y) {
    const DenoiseFrameConstants& c = p.c;
    const u32 w = c.width;
    const u32 h = c.height;
    const u32 i = y * w + x;
    const F4 sig = read_signal(c, p.signal, i);
    const f32 l = luminance(sig, c.flags);
    const F4 g = p.guideCur[i];
    if (!(g.w > 0.f)) {
        p.accum[i] = sig;
        p.momCur[i] = F4{l, l * l, 0.f, 0.f};
        return;
    }
    f32 sumW = 0.f;
    f32 hr = 0.f;
    f32 hg = 0.f;
    f32 hb = 0.f;
    f32 m1 = 0.f;
    f32 m2 = 0.f;
    f32 len = 0.f;
    if ((c.flags & kDenoiseFlagHistory) != 0u) {
        const F2 m = p.motion[i];
        f32 px = (static_cast<f32>(x) + 0.5f) - m.x * static_cast<f32>(w);
        f32 py = (static_cast<f32>(y) + 0.5f) - m.y * static_cast<f32>(h);
        px = px - 0.5f;
        py = py - 0.5f;
        // Far outside (or NaN): no tap can be inside; the clamp keeps the int conversion defined.
        px = px > -2.f ? (px < static_cast<f32>(w) + 1.f ? px : static_cast<f32>(w) + 1.f) : -2.f;
        py = py > -2.f ? (py < static_cast<f32>(h) + 1.f ? py : static_cast<f32>(h) + 1.f) : -2.f;
        const f32 x0 = std::floor(px);
        const f32 y0 = std::floor(py);
        const f32 fx = px - x0;
        const f32 fy = py - y0;
        const s32 ix = static_cast<s32>(x0);
        const s32 iy = static_cast<s32>(y0);
        const F2 gz = p.gradZ[i];
        const f32 tol = c.reprojDepth * g.w + (std::fabs(gz.x) + std::fabs(gz.y));
        for (u32 t = 0; t < 4u; ++t) {
            const s32 tx = ix + static_cast<s32>(t & 1u);
            const s32 ty = iy + static_cast<s32>(t >> 1u);
            const f32 wx = (t & 1u) != 0u ? fx : 1.f - fx;
            const f32 wy = (t >> 1u) != 0u ? fy : 1.f - fy;
            const f32 wt = wx * wy;
            if (tx < 0 || ty < 0 || tx >= static_cast<s32>(w) || ty >= static_cast<s32>(h) || !(wt > 0.f)) {
                continue;
            }
            const u32 j = static_cast<u32>(ty) * w + static_cast<u32>(tx);
            const F4 gp = p.guidePrev[j];
            if (!(gp.w > 0.f) || std::fabs(gp.w - g.w) > tol || dot3(gp, g) < c.reprojNormal) {
                continue;
            }
            const F4 hc = p.histPrev[j];
            const F4 hm = p.momPrev[j];
            sumW = sumW + wt;
            hr = hr + wt * hc.x;
            hg = hg + wt * hc.y;
            hb = hb + wt * hc.z;
            m1 = m1 + wt * hm.x;
            m2 = m2 + wt * hm.y;
            len = len + wt * hm.z;
        }
    }
    const bool valid = sumW >= c.minReprojWeight && sumW > 0.f;
    f32 aC = 1.f;
    f32 aM = 1.f;
    f32 newLen = 1.f;
    if (valid) {
        hr = hr / sumW;
        hg = hg / sumW;
        hb = hb / sumW;
        m1 = m1 / sumW;
        m2 = m2 / sumW;
        len = len / sumW;
        newLen = len + 1.f;
        newLen = newLen < c.maxHistory ? newLen : c.maxHistory;
        f32 lambda = 0.f;
        if ((c.flags & kDenoiseFlagGradients) != 0u) {
            lambda = gradient_lambda(c, p.lambda[(y / kDenoiseStratum) * c.strataW + x / kDenoiseStratum]);
            newLen = newLen * (1.f - lambda);
            newLen = newLen > 1.f ? newLen : 1.f;
        }
        const f32 inv = 1.f / newLen;
        aC = c.alphaColor > inv ? c.alphaColor : inv;
        aM = c.alphaMoments > inv ? c.alphaMoments : inv;
        if ((c.flags & kDenoiseFlagGradients) != 0u) {
            aC = aC * (1.f - lambda) + lambda;
            aM = aM * (1.f - lambda) + lambda;
        }
    } else {
        hr = hg = hb = m1 = m2 = 0.f;
    }
    const f32 bC = 1.f - aC;
    const f32 bM = 1.f - aM;
    p.accum[i] = F4{hr * bC + sig.x * aC, hg * bC + sig.y * aC, hb * bC + sig.z * aC, 0.f};
    p.momCur[i] = F4{m1 * bM + l * aM, m2 * bM + (l * l) * aM, newLen, 0.f};
}

struct TemporalKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const TemporalParams& p) const {
        temporal_pixel(p, idx.global.x, idx.global.y);
    }
};

// --- variance estimation ----------------------------------------------------------------------------
struct VarianceParams {
    DenoiseFrameConstants c{};
    kernel::Span<const F4> guide;
    kernel::Span<const F2> gradZ;
    kernel::Span<const F4> accum;
    kernel::Span<const F4> moments;
    kernel::Span<F4> dst; ///< out: (rgb, variance)
};

FUSE_HOST_DEVICE inline void variance_pixel(const VarianceParams& p, u32 x, u32 y) {
    const DenoiseFrameConstants& c = p.c;
    const u32 w = c.width;
    const u32 i = y * w + x;
    const F4 g = p.guide[i];
    const F4 a = p.accum[i];
    const F4 mo = p.moments[i];
    if (!(g.w > 0.f)) {
        p.dst[i] = F4{a.x, a.y, a.z, 0.f};
        return;
    }
    const f32 len = mo.z;
    if ((c.flags & kDenoiseFlagSpatialVariance) == 0u || !(len < c.varianceHistory)) {
        const f32 v = mo.y - mo.x * mo.x;
        p.dst[i] = F4{a.x, a.y, a.z, v > 0.f ? v : 0.f};
        return;
    }
    const F2 gz = p.gradZ[i];
    const f32 lp = luminance(a, c.flags);
    f32 sumW = 0.f;
    f32 sr = 0.f;
    f32 sg = 0.f;
    f32 sb = 0.f;
    f32 s1 = 0.f;
    f32 s2 = 0.f;
    for (s32 dy = -3; dy <= 3; ++dy) {
        for (s32 dx = -3; dx <= 3; ++dx) {
            const s32 qx = static_cast<s32>(x) + dx;
            const s32 qy = static_cast<s32>(y) + dy;
            if (qx < 0 || qy < 0 || qx >= static_cast<s32>(w) || qy >= static_cast<s32>(c.height)) {
                continue;
            }
            const u32 q = static_cast<u32>(qy) * w + static_cast<u32>(qx);
            const F4 gq = p.guide[q];
            if (!(gq.w > 0.f)) {
                continue;
            }
            const F4 aq = p.accum[q];
            const F4 mq = p.moments[q];
            const f32 wz = depth_term(c, g.w, gq.w, gz, static_cast<f32>(dx), static_cast<f32>(dy));
            const f32 wn = normal_weight(g, gq, c.sigmaNormal);
            const f32 wl = std::fabs(lp - luminance(aq, c.flags)) / c.sigmaVarianceLum;
            const f32 wgt = wn * std::exp(-(wz + wl));
            sumW = sumW + wgt;
            sr = sr + wgt * aq.x;
            sg = sg + wgt * aq.y;
            sb = sb + wgt * aq.z;
            s1 = s1 + wgt * mq.x;
            s2 = s2 + wgt * mq.y;
        }
    }
    if (!(sumW > 0.f)) {
        const f32 v = mo.y - mo.x * mo.x;
        p.dst[i] = F4{a.x, a.y, a.z, v > 0.f ? v : 0.f};
        return;
    }
    const f32 m1 = s1 / sumW;
    const f32 m2 = s2 / sumW;
    const f32 v = m2 - m1 * m1;
    const f32 boost = c.varianceBoost / (len > 1.f ? len : 1.f);
    p.dst[i] = F4{sr / sumW, sg / sumW, sb / sumW, (v > 0.f ? v : 0.f) * boost};
}

struct VarianceKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const VarianceParams& p) const {
        variance_pixel(p, idx.global.x, idx.global.y);
    }
};

// --- a-trous iteration --------------------------------------------------------------------------------
struct AtrousParams {
    DenoiseFrameConstants c{};
    kernel::Span<const F4> guide;
    kernel::Span<const F2> gradZ;
    kernel::Span<const F4> src; ///< (rgb, variance)
    kernel::Span<F4> dst;       ///< out: (rgb, variance)
    kernel::Span<F4> history;   ///< optional out: (rgb, 0) (the colour-history tap)
    u32 step = 1u;
};

FUSE_HOST_DEVICE inline void atrous_pixel(const AtrousParams& p, u32 x, u32 y) {
    const DenoiseFrameConstants& c = p.c;
    const u32 w = c.width;
    const u32 h = c.height;
    const u32 i = y * w + x;
    const F4 g = p.guide[i];
    const F4 in = p.src[i];
    F4 out = in;
    if (g.w > 0.f) {
        // 3 x 3 Gaussian of the variance (sky / outside taps skipped and the weights renormalised).
        f32 gs = 0.f;
        f32 gw = 0.f;
        for (s32 dy = -1; dy <= 1; ++dy) {
            for (s32 dx = -1; dx <= 1; ++dx) {
                const s32 qx = static_cast<s32>(x) + dx;
                const s32 qy = static_cast<s32>(y) + dy;
                if (qx < 0 || qy < 0 || qx >= static_cast<s32>(w) || qy >= static_cast<s32>(h)) {
                    continue;
                }
                const u32 q = static_cast<u32>(qy) * w + static_cast<u32>(qx);
                if (!(p.guide[q].w > 0.f)) {
                    continue;
                }
                const f32 k = gauss_tap(dx) * gauss_tap(dy);
                gs = gs + k * p.src[q].w;
                gw = gw + k;
            }
        }
        const f32 gvar = gs / gw;
        const f32 phiL = c.sigmaLuminance * std::sqrt(gvar > 0.f ? gvar : 0.f) + 1e-6f;
        const F2 gz = p.gradZ[i];
        const f32 lp = luminance(in, c.flags);
        const s32 step = static_cast<s32>(p.step);
        f32 sumW = 0.f;
        f32 sr = 0.f;
        f32 sg = 0.f;
        f32 sb = 0.f;
        f32 sv = 0.f;
        for (s32 dy = -2; dy <= 2; ++dy) {
            for (s32 dx = -2; dx <= 2; ++dx) {
                const s32 ox = dx * step;
                const s32 oy = dy * step;
                const s32 qx = static_cast<s32>(x) + ox;
                const s32 qy = static_cast<s32>(y) + oy;
                if (qx < 0 || qy < 0 || qx >= static_cast<s32>(w) || qy >= static_cast<s32>(h)) {
                    continue;
                }
                const u32 q = static_cast<u32>(qy) * w + static_cast<u32>(qx);
                const F4 gq = p.guide[q];
                if (!(gq.w > 0.f)) {
                    continue;
                }
                const F4 sq = p.src[q];
                const f32 k = atrous_tap(dx) * atrous_tap(dy);
                const f32 wz = depth_term(c, g.w, gq.w, gz, static_cast<f32>(ox), static_cast<f32>(oy));
                const f32 wn = normal_weight(g, gq, c.sigmaNormal);
                const f32 wl = std::fabs(lp - luminance(sq, c.flags)) / phiL;
                const f32 wgt = k * wn * std::exp(-(wz + wl));
                sumW = sumW + wgt;
                sr = sr + wgt * sq.x;
                sg = sg + wgt * sq.y;
                sb = sb + wgt * sq.z;
                sv = sv + (wgt * wgt) * sq.w;
            }
        }
        if (sumW > 0.f) {
            out = F4{sr / sumW, sg / sumW, sb / sumW, sv / (sumW * sumW)};
        }
    }
    p.dst[i] = out;
    if (!p.history.empty()) {
        p.history[i] = F4{out.x, out.y, out.z, 0.f};
    }
}

struct AtrousKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const AtrousParams& p) const {
        atrous_pixel(p, idx.global.x, idx.global.y);
    }
};

inline kernel::KernelLaunch make_launch(const char* name, u32 width, u32 height) {
    return kernel::KernelLaunch{name, kernel::extent2(width, height), {kDenoiseTile, kDenoiseTile, 1u}};
}

} // namespace fuse::renderer::denoise::svgf_kernel
