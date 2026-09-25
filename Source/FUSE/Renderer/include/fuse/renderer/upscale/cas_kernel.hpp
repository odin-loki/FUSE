#pragma once

// Single-source AMD FidelityFX Contrast Adaptive Sharpening (sharpen-only), float32 path.
//
// Transcribed from the vendored FidelityFX SDK v1.1.4 header
// Engine/lib/fidelityfx/include/FidelityFX/gpu/cas/ffx_cas.h (MIT, Copyright (C) 2024 Advanced Micro
// Devices, Inc.; see Engine/lib/fidelityfx/LICENSE.txt): cas_pixel == casFilterNoScaling, i.e. the
// FFX_CAS_OPTION_SHARPEN_ONLY=1 permutation of ffx_cas_sharpen_pass with FFX_CAS_COLOR_SPACE_CONVERSION=0.
// `better_diagonals` / `precise_math` select the FFX_CAS_BETTER_DIAGONALS / FFX_CAS_USE_PRECISE_MATH
// variants (both off in the SDK's shipped permutations). CAS's own scaling mode (casFilterWithScaling) is
// not ported: FUSE upscales with EASU / NIS / TAAU and uses CAS as a post-process sharpener.
// CasConstants holds the ffxCasSetup bit patterns (host side: upscale_passes.cpp).

#include <fuse/renderer/upscale/upscale_kernel_common.hpp>

namespace fuse::renderer::upscale::kernels {

inline constexpr const char* kCasKernelName = "upscale_cas";

/// ffxCasSetup output. const1[0] = asuint(-1 / lerp(8, 5, saturate(sharpness))) is the only value the
/// sharpen-only float path reads.
struct CasConstants {
    u32 const0[4] = {};
    u32 const1[4] = {};
};

FUSE_HOST_DEVICE inline math::Vec4 cas_pixel(const RgbaSurface& src, const CasConstants& con, s32 x, s32 y,
                                             bool better_diagonals, bool precise_math) {
    // a b c
    // d e f
    // g h i
    const math::Vec4 a = load_clamped(src, x - 1, y - 1);
    const math::Vec4 b = load_clamped(src, x, y - 1);
    const math::Vec4 c = load_clamped(src, x + 1, y - 1);
    const math::Vec4 d = load_clamped(src, x - 1, y);
    const math::Vec4 e = load_clamped(src, x, y);
    const math::Vec4 f = load_clamped(src, x + 1, y);
    const math::Vec4 g = load_clamped(src, x - 1, y + 1);
    const math::Vec4 h = load_clamped(src, x, y + 1);
    const math::Vec4 i = load_clamped(src, x + 1, y + 1);

    // Soft min and max (2.0x bigger with better diagonals, the extra multiply is factored out).
    f32 mnR = gpu_min3(gpu_min3(d.x, e.x, f.x), b.x, h.x);
    f32 mnG = gpu_min3(gpu_min3(d.y, e.y, f.y), b.y, h.y);
    f32 mnB = gpu_min3(gpu_min3(d.z, e.z, f.z), b.z, h.z);
    if (better_diagonals) {
        mnR = mnR + gpu_min3(gpu_min3(mnR, a.x, c.x), g.x, i.x);
        mnG = mnG + gpu_min3(gpu_min3(mnG, a.y, c.y), g.y, i.y);
        mnB = mnB + gpu_min3(gpu_min3(mnB, a.z, c.z), g.z, i.z);
    }
    f32 mxR = gpu_max3(gpu_max3(d.x, e.x, f.x), b.x, h.x);
    f32 mxG = gpu_max3(gpu_max3(d.y, e.y, f.y), b.y, h.y);
    f32 mxB = gpu_max3(gpu_max3(d.z, e.z, f.z), b.z, h.z);
    if (better_diagonals) {
        mxR = mxR + gpu_max3(gpu_max3(mxR, a.x, c.x), g.x, i.x);
        mxG = mxG + gpu_max3(gpu_max3(mxG, a.y, c.y), g.y, i.y);
        mxB = mxB + gpu_max3(gpu_max3(mxB, a.z, c.z), g.z, i.z);
    }

    // Smooth minimum distance to signal limit divided by smooth max.
    const f32 rcpMR = precise_math ? 1.f / mxR : ffx_rcp_approx(mxR);
    const f32 rcpMG = precise_math ? 1.f / mxG : ffx_rcp_approx(mxG);
    const f32 rcpMB = precise_math ? 1.f / mxB : ffx_rcp_approx(mxB);
    const f32 limit = precise_math ? 2.f : 1.f;
    f32 ampR = saturate(gpu_min(mnR, limit - mxR) * rcpMR);
    f32 ampG = saturate(gpu_min(mnG, limit - mxG) * rcpMG);
    f32 ampB = saturate(gpu_min(mnB, limit - mxB) * rcpMB);

    // Shaping amount of sharpening.
    ampR = precise_math ? std::sqrt(ampR) : ffx_sqrt_approx(ampR);
    ampG = precise_math ? std::sqrt(ampG) : ffx_sqrt_approx(ampG);
    ampB = precise_math ? std::sqrt(ampB) : ffx_sqrt_approx(ampB);
    (void)ampR;
    (void)ampB;

    // Filter shape:  0 w 0 / w 1 w / 0 w 0. Filter using the green coefficient only (as the SDK does).
    const f32 peak = as_f32(con.const1[0]);
    const f32 wG = ampG * peak;
    const f32 rcpW = precise_math ? 1.f / (1.f + 4.f * wG) : ffx_rcp_medium(1.f + 4.f * wG);

    return math::Vec4(saturate((b.x * wG + d.x * wG + f.x * wG + h.x * wG + e.x) * rcpW),
                      saturate((b.y * wG + d.y * wG + f.y * wG + h.y * wG + e.y) * rcpW),
                      saturate((b.z * wG + d.z * wG + f.z * wG + h.z * wG + e.z) * rcpW), 1.f);
}

struct CasParams {
    RgbaSurface src{};
    RgbaTarget dst{};
    CasConstants con{};
    u32 better_diagonals = 0u;
    u32 precise_math = 0u;
};

struct CasKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const CasParams& p) const {
        p.dst.data[idx.global.y * p.dst.width + idx.global.x] =
            cas_pixel(p.src, p.con, static_cast<s32>(idx.global.x), static_cast<s32>(idx.global.y),
                      p.better_diagonals != 0u, p.precise_math != 0u);
    }
};

} // namespace fuse::renderer::upscale::kernels
