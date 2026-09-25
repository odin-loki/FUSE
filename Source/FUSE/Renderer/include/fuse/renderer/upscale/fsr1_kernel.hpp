#pragma once

// Single-source AMD FidelityFX Super Resolution 1 (EASU + RCAS), float32 path.
//
// Transcribed from the vendored FidelityFX SDK v1.1.4 header
// Engine/lib/fidelityfx/include/FidelityFX/gpu/fsr1/ffx_fsr1.h (MIT, Copyright (C) 2024 Advanced Micro
// Devices, Inc.; see Engine/lib/fidelityfx/LICENSE.txt):
//   * easu_pixel  == ffxFsrEasuFloat + fsrEasuSetFloat + fsrEasuTapFloat (FFX_FSR_EASU_FLOAT). The four
//     textureGather calls of the shader are replaced by the twelve clamp-to-edge texel loads they return.
//   * rcas_pixel  == FsrRcasF (FSR_RCAS_F) with FSR_RCAS_DENOISE, as ffx_fsr1_rcas.h compiles it.
//   * EasuConstants / RcasConstants hold the same u32 bit patterns ffxFsrPopulateEasuConstants /
//     FsrRcasCon produce; the host side (upscale_passes.cpp) computes them with the same float operations.
// The fuse_upscale_gpu_reference gate runs the vendored GLSL passes on Lavapipe and compares.
//
// FSR1 expects anti-aliased, perceptual (tone-mapped, gamma-encoded) colour in [0, 1]: run it after
// tonemapping and before grain / UI (docs/research/upscaling-framegen-and-post-injectors.md §3.1).

#include <fuse/renderer/upscale/upscale_kernel_common.hpp>

namespace fuse::renderer::upscale::kernels {

inline constexpr const char* kEasuKernelName = "upscale_fsr1_easu";
inline constexpr const char* kRcasKernelName = "upscale_fsr1_rcas";

/// ffxFsrPopulateEasuConstants output (float values stored as u32 bit patterns, like the SDK cbuffer).
struct EasuConstants {
    u32 con0[4] = {};
    u32 con1[4] = {};
    u32 con2[4] = {};
    u32 con3[4] = {};
};

/// FsrRcasCon output: con[0] = asuint(exp2(-sharpness_stops)), con[1] = packed half2 (unused by the f32 path).
struct RcasConstants {
    u32 con[4] = {};
};

namespace fsr1_detail {

/// fsrEasuTapFloat.
FUSE_HOST_DEVICE inline void easu_tap(f32& acR, f32& acG, f32& acB, f32& aw, f32 offX, f32 offY, f32 dirX,
                                      f32 dirY, f32 lenX, f32 lenY, f32 lob, f32 clp, const math::Vec4& c) {
    f32 vx = (offX * dirX) + (offY * dirY);
    f32 vy = (offX * (-dirY)) + (offY * dirX);
    vx *= lenX;
    vy *= lenY;
    f32 d2 = vx * vx + vy * vy;
    d2 = gpu_min(d2, clp);
    f32 wB = static_cast<f32>(2.0 / 5.0) * d2 + -1.f;
    f32 wA = lob * d2 + -1.f;
    wB *= wB;
    wA *= wA;
    wB = static_cast<f32>(25.0 / 16.0) * wB + static_cast<f32>(-(25.0 / 16.0 - 1.0));
    const f32 w = wB * wA;
    acR += c.x * w;
    acG += c.y * w;
    acB += c.z * w;
    aw += w;
}

/// fsrEasuSetFloat for one of the four bilinear corners (`corner`: 0 = s, 1 = t, 2 = u, 3 = v).
FUSE_HOST_DEVICE inline void easu_set(f32& dirX, f32& dirY, f32& len, f32 ppx, f32 ppy, u32 corner, f32 lA, f32 lB,
                                      f32 lC, f32 lD, f32 lE) {
    f32 w = 0.f;
    if (corner == 0u) {
        w = (1.f - ppx) * (1.f - ppy);
    }
    if (corner == 1u) {
        w = ppx * (1.f - ppy);
    }
    if (corner == 2u) {
        w = (1.f - ppx) * ppy;
    }
    if (corner == 3u) {
        w = ppx * ppy;
    }
    const f32 dc = lD - lC;
    const f32 cb = lC - lB;
    f32 lenX = gpu_max(std::fabs(dc), std::fabs(cb));
    lenX = ffx_rcp_approx(lenX);
    const f32 dx = lD - lB;
    dirX += dx * w;
    lenX = saturate(std::fabs(dx) * lenX);
    lenX *= lenX;
    len += lenX * w;

    const f32 ec = lE - lC;
    const f32 ca = lC - lA;
    f32 lenY = gpu_max(std::fabs(ec), std::fabs(ca));
    lenY = ffx_rcp_approx(lenY);
    const f32 dy = lE - lA;
    dirY += dy * w;
    lenY = saturate(std::fabs(dy) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

/// "Simplest multi-channel approximate luma possible (luma times 2, in 2 FMA/MAD)".
FUSE_HOST_DEVICE inline f32 luma2(const math::Vec4& c) { return c.z * 0.5f + (c.x * 0.5f + c.y); }

} // namespace fsr1_detail

/// EASU for output pixel (ipx, ipy): ffxFsrEasuFloat. Returns (rgb, 1) like the SDK pass's store.
FUSE_HOST_DEVICE inline math::Vec4 easu_pixel(const RgbaSurface& src, const EasuConstants& con, u32 ipx, u32 ipy) {
    using namespace fsr1_detail;
    // Position of 'f'.
    f32 ppx = static_cast<f32>(ipx) * as_f32(con.con0[0]) + as_f32(con.con0[2]);
    f32 ppy = static_cast<f32>(ipy) * as_f32(con.con0[1]) + as_f32(con.con0[3]);
    const f32 fpx = std::floor(ppx);
    const f32 fpy = std::floor(ppy);
    ppx -= fpx;
    ppy -= fpy;
    const s32 fx = static_cast<s32>(fpx);
    const s32 fy = static_cast<s32>(fpy);

    // 12-tap kernel (the texels the shader's four gathers return).
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    const math::Vec4 b = load_clamped(src, fx, fy - 1);
    const math::Vec4 c = load_clamped(src, fx + 1, fy - 1);
    const math::Vec4 e = load_clamped(src, fx - 1, fy);
    const math::Vec4 f = load_clamped(src, fx, fy);
    const math::Vec4 g = load_clamped(src, fx + 1, fy);
    const math::Vec4 h = load_clamped(src, fx + 2, fy);
    const math::Vec4 i = load_clamped(src, fx - 1, fy + 1);
    const math::Vec4 j = load_clamped(src, fx, fy + 1);
    const math::Vec4 k = load_clamped(src, fx + 1, fy + 1);
    const math::Vec4 l = load_clamped(src, fx + 2, fy + 1);
    const math::Vec4 n = load_clamped(src, fx, fy + 2);
    const math::Vec4 o = load_clamped(src, fx + 1, fy + 2);

    const f32 bL = luma2(b);
    const f32 cL = luma2(c);
    const f32 iL = luma2(i);
    const f32 jL = luma2(j);
    const f32 fL = luma2(f);
    const f32 eL = luma2(e);
    const f32 kL = luma2(k);
    const f32 lL = luma2(l);
    const f32 hL = luma2(h);
    const f32 gL = luma2(g);
    const f32 oL = luma2(o);
    const f32 nL = luma2(n);

    // Accumulate for bilinear interpolation.
    f32 dirX = 0.f;
    f32 dirY = 0.f;
    f32 len = 0.f;
    easu_set(dirX, dirY, len, ppx, ppy, 0u, bL, eL, fL, gL, jL);
    easu_set(dirX, dirY, len, ppx, ppy, 1u, cL, fL, gL, hL, kL);
    easu_set(dirX, dirY, len, ppx, ppy, 2u, fL, iL, jL, kL, nL);
    easu_set(dirX, dirY, len, ppx, ppy, 3u, gL, jL, kL, lL, oL);

    // Normalize with approximation, and cleanup close to zero.
    const f32 dir2x = dirX * dirX;
    const f32 dir2y = dirY * dirY;
    f32 dirR = dir2x + dir2y;
    const bool zro = dirR < static_cast<f32>(1.0 / 32768.0);
    dirR = ffx_rsqrt_approx(dirR);
    dirR = zro ? 1.f : dirR;
    dirX = zro ? 1.f : dirX;
    dirX *= dirR;
    dirY *= dirR;

    // Transform from {0 to 2} to {0 to 1} range, and shape with square.
    len = len * 0.5f;
    len *= len;

    // Stretch kernel {1.0 vert|horz, to sqrt(2.0) on diagonal}.
    const f32 stretch = (dirX * dirX + dirY * dirY) * ffx_rcp_approx(gpu_max(std::fabs(dirX), std::fabs(dirY)));

    // Anisotropic length after rotation.
    const f32 len2x = 1.f + (stretch - 1.f) * len;
    const f32 len2y = 1.f + -0.5f * len;

    // Based on the amount of 'edge', the window shifts from +/-{sqrt(2.0) to slightly beyond 2.0}.
    const f32 lob = 0.5f + static_cast<f32>((1.0 / 4.0 - 0.04) - 0.5) * len;
    // Set distance^2 clipping point to the end of the adjustable window.
    const f32 clp = ffx_rcp_approx(lob);

    // Accumulation mixed with min/max of 4 nearest (f, g, j, k).
    const f32 min4R = gpu_min(gpu_min3(f.x, g.x, j.x), k.x);
    const f32 min4G = gpu_min(gpu_min3(f.y, g.y, j.y), k.y);
    const f32 min4B = gpu_min(gpu_min3(f.z, g.z, j.z), k.z);
    const f32 max4R = gpu_max(gpu_max3(f.x, g.x, j.x), k.x);
    const f32 max4G = gpu_max(gpu_max3(f.y, g.y, j.y), k.y);
    const f32 max4B = gpu_max(gpu_max3(f.z, g.z, j.z), k.z);

    f32 aR = 0.f;
    f32 aG = 0.f;
    f32 aB = 0.f;
    f32 aW = 0.f;
    easu_tap(aR, aG, aB, aW, 0.f - ppx, -1.f - ppy, dirX, dirY, len2x, len2y, lob, clp, b);
    easu_tap(aR, aG, aB, aW, 1.f - ppx, -1.f - ppy, dirX, dirY, len2x, len2y, lob, clp, c);
    easu_tap(aR, aG, aB, aW, -1.f - ppx, 1.f - ppy, dirX, dirY, len2x, len2y, lob, clp, i);
    easu_tap(aR, aG, aB, aW, 0.f - ppx, 1.f - ppy, dirX, dirY, len2x, len2y, lob, clp, j);
    easu_tap(aR, aG, aB, aW, 0.f - ppx, 0.f - ppy, dirX, dirY, len2x, len2y, lob, clp, f);
    easu_tap(aR, aG, aB, aW, -1.f - ppx, 0.f - ppy, dirX, dirY, len2x, len2y, lob, clp, e);
    easu_tap(aR, aG, aB, aW, 1.f - ppx, 1.f - ppy, dirX, dirY, len2x, len2y, lob, clp, k);
    easu_tap(aR, aG, aB, aW, 2.f - ppx, 1.f - ppy, dirX, dirY, len2x, len2y, lob, clp, l);
    easu_tap(aR, aG, aB, aW, 2.f - ppx, 0.f - ppy, dirX, dirY, len2x, len2y, lob, clp, h);
    easu_tap(aR, aG, aB, aW, 1.f - ppx, 0.f - ppy, dirX, dirY, len2x, len2y, lob, clp, g);
    easu_tap(aR, aG, aB, aW, 1.f - ppx, 2.f - ppy, dirX, dirY, len2x, len2y, lob, clp, o);
    easu_tap(aR, aG, aB, aW, 0.f - ppx, 2.f - ppy, dirX, dirY, len2x, len2y, lob, clp, n);

    // Normalize and dering.
    const f32 rcpW = 1.f / aW;
    return math::Vec4(gpu_min(max4R, gpu_max(min4R, aR * rcpW)), gpu_min(max4G, gpu_max(min4G, aG * rcpW)),
                      gpu_min(max4B, gpu_max(min4B, aB * rcpW)), 1.f);
}

/// FSR_RCAS_LIMIT: "set at the limit of providing unnatural results for sharpening".
inline constexpr f32 kRcasLimit = static_cast<f32>(0.25 - (1.0 / 16.0));

/// RCAS for pixel (x, y): FsrRcasF with FSR_RCAS_DENOISE when `denoise` (the SDK pass compiles it on).
/// Alpha passes through when `passthrough_alpha` (FSR_RCAS_PASSTHROUGH_ALPHA), else 1 like the SDK store.
FUSE_HOST_DEVICE inline math::Vec4 rcas_pixel(const RgbaSurface& src, const RcasConstants& con, s32 x, s32 y,
                                              bool denoise, bool passthrough_alpha) {
    // Algorithm uses minimal 3x3 pixel neighborhood.
    //    b
    //  d e f
    //    h
    const math::Vec4 b = load_clamped(src, x, y - 1);
    const math::Vec4 d = load_clamped(src, x - 1, y);
    const math::Vec4 e = load_clamped(src, x, y);
    const math::Vec4 f = load_clamped(src, x + 1, y);
    const math::Vec4 h = load_clamped(src, x, y + 1);

    // Luma times 2.
    const f32 bL = b.z * 0.5f + (b.x * 0.5f + b.y);
    const f32 dL = d.z * 0.5f + (d.x * 0.5f + d.y);
    const f32 eL = e.z * 0.5f + (e.x * 0.5f + e.y);
    const f32 fL = f.z * 0.5f + (f.x * 0.5f + f.y);
    const f32 hL = h.z * 0.5f + (h.x * 0.5f + h.y);

    // Noise detection.
    f32 nz = 0.25f * bL + 0.25f * dL + 0.25f * fL + 0.25f * hL - eL;
    nz = saturate(std::fabs(nz) *
                  ffx_rcp_medium(gpu_max3(gpu_max3(bL, dL, eL), fL, hL) - gpu_min3(gpu_min3(bL, dL, eL), fL, hL)));
    nz = -0.5f * nz + 1.f;

    // Min and max of ring.
    const f32 mn4R = gpu_min(gpu_min3(b.x, d.x, f.x), h.x);
    const f32 mn4G = gpu_min(gpu_min3(b.y, d.y, f.y), h.y);
    const f32 mn4B = gpu_min(gpu_min3(b.z, d.z, f.z), h.z);
    const f32 mx4R = gpu_max(gpu_max3(b.x, d.x, f.x), h.x);
    const f32 mx4G = gpu_max(gpu_max3(b.y, d.y, f.y), h.y);
    const f32 mx4B = gpu_max(gpu_max3(b.z, d.z, f.z), h.z);

    // Immediate constants for peak range.
    const f32 peakCx = 1.f;
    const f32 peakCy = -1.f * 4.f;
    // Limiters, these need to be high precision RCPs.
    const f32 hitMinR = mn4R * (1.f / (4.f * mx4R));
    const f32 hitMinG = mn4G * (1.f / (4.f * mx4G));
    const f32 hitMinB = mn4B * (1.f / (4.f * mx4B));
    const f32 hitMaxR = (peakCx - mx4R) * (1.f / (4.f * mn4R + peakCy));
    const f32 hitMaxG = (peakCx - mx4G) * (1.f / (4.f * mn4G + peakCy));
    const f32 hitMaxB = (peakCx - mx4B) * (1.f / (4.f * mn4B + peakCy));
    const f32 lobeR = gpu_max(-hitMinR, hitMaxR);
    const f32 lobeG = gpu_max(-hitMinG, hitMaxG);
    const f32 lobeB = gpu_max(-hitMinB, hitMaxB);
    f32 lobe = gpu_max(-kRcasLimit, gpu_min(gpu_max3(lobeR, lobeG, lobeB), 0.f)) * as_f32(con.con[0]);

    // Apply noise removal.
    if (denoise) {
        lobe *= nz;
    }

    // Resolve, which needs the medium precision rcp approximation to avoid visible tonality changes.
    const f32 rcpL = ffx_rcp_medium(4.f * lobe + 1.f);
    return math::Vec4((lobe * b.x + lobe * d.x + lobe * h.x + lobe * f.x + e.x) * rcpL,
                      (lobe * b.y + lobe * d.y + lobe * h.y + lobe * f.y + e.y) * rcpL,
                      (lobe * b.z + lobe * d.z + lobe * h.z + lobe * f.z + e.z) * rcpL,
                      passthrough_alpha ? e.w : 1.f);
}

struct EasuParams {
    RgbaSurface src{};
    RgbaTarget dst{};
    EasuConstants con{};
};

struct EasuKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const EasuParams& p) const {
        p.dst.data[idx.global.y * p.dst.width + idx.global.x] = easu_pixel(p.src, p.con, idx.global.x, idx.global.y);
    }
};

struct RcasParams {
    RgbaSurface src{};
    RgbaTarget dst{};
    RcasConstants con{};
    u32 denoise = 1u;
    u32 passthrough_alpha = 0u;
};

struct RcasKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const RcasParams& p) const {
        p.dst.data[idx.global.y * p.dst.width + idx.global.x] =
            rcas_pixel(p.src, p.con, static_cast<s32>(idx.global.x), static_cast<s32>(idx.global.y), p.denoise != 0u,
                       p.passthrough_alpha != 0u);
    }
};

} // namespace fuse::renderer::upscale::kernels
