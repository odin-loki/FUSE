#pragma once

// Single-source NVIDIA Image Scaling (NVScaler), float32 SDR path.
//
// Transcribed from the vendored NIS SDK v1.0.3 shader Engine/lib/nvidia-nis/NIS/NIS_Scaler.h (MIT,
// Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES; see Engine/lib/nvidia-nis/licence.txt), with
// NIS_SCALER=1, NIS_HDR_MODE_NONE, NIS_USE_HALF_PRECISION=0, NIS_VIEWPORT_SUPPORT=0, no output clamp.
//
// The shader stages a luma tile, a 2x2-batched edge map and the filter banks in groupshared memory; every
// value it stages depends only on absolute source coordinates, so the port evaluates the same quantities
// per output pixel (no tile) and is numerically the same computation:
//   * luma window p[6][6] = getY of the clamp-to-edge source texels at rows floor(srcY)-2 .. +3 and
//     columns floor(srcX)-2 .. +3 (the tile loads use a linear-clamp sampler at texel centres), including the
//     shader's unsigned tile-origin wrap at the top / left image border when upscaling (see nis_pixel);
//   * edge[i][j] = GetEdgeMap of the 3x3 luma neighbourhood centred on source texel
//     (floor(srcY)+i, floor(srcX)+j);
//   * FilterNormal / AddDirFilters / EvalPoly6 / CalcLTI verbatim, 64-phase coefficient banks from
//     NIS_Config.h (coef_scale / coef_usm, 8 floats per phase, 6 used);
//   * chroma = bilinear tap at srcX + 0.5 (texel space), corrected by the filtered luma difference.
// NisConstants carries the NVScalerUpdateConfig values (host side: upscale_passes.cpp includes the vendored
// NIS_Config.h). NVScaler supports scale factors 0.5..1 (up to 2x upscaling).

#include <fuse/renderer/upscale/upscale_kernel_common.hpp>

namespace fuse::renderer::upscale::kernels {

inline constexpr const char* kNisKernelName = "upscale_nis";
inline constexpr u32 kNisPhaseCount = 64u;
/// Coefficient row stride in floats (NIS_Config.h kFilterSize; the shader uses the first 6).
inline constexpr u32 kNisCoefStride = 8u;
inline constexpr u32 kNisCoefCount = kNisPhaseCount * kNisCoefStride;

/// The NISConfig fields the NVScaler float path reads (see NIS_Config.h NVScalerUpdateConfig).
struct NisConstants {
    f32 detect_ratio = 0.f;
    f32 detect_thres = 0.f;
    f32 min_contrast_ratio = 0.f;
    f32 ratio_norm = 0.f;
    f32 contrast_boost = 0.f;
    f32 eps = 0.f;
    f32 sharp_start_y = 0.f;
    f32 sharp_scale_y = 0.f;
    f32 sharp_strength_min = 0.f;
    f32 sharp_strength_scale = 0.f;
    f32 sharp_limit_min = 0.f;
    f32 sharp_limit_scale = 0.f;
    f32 scale_x = 1.f; ///< input / output (0.5 .. 1)
    f32 scale_y = 1.f;
};

namespace nis_detail {

FUSE_HOST_DEVICE inline f32 get_y(const math::Vec4& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

struct Weights {
    f32 x = 0.f; ///< 0 deg
    f32 y = 0.f; ///< 90 deg
    f32 z = 0.f; ///< 45 deg
    f32 w = 0.f; ///< 135 deg
};

/// GetEdgeMap(p, i, j) for the 3x3 window whose top-left is p[r0][c0] (p is the 6x6 luma window).
FUSE_HOST_DEVICE inline Weights edge_map(const f32 (&p)[6][6], u32 r0, u32 c0, const NisConstants& k) {
    const u32 i = r0;
    const u32 j = c0;
    const f32 g_0 = std::fabs(p[0 + i][0 + j] + p[0 + i][1 + j] + p[0 + i][2 + j] - p[2 + i][0 + j] - p[2 + i][1 + j] -
                              p[2 + i][2 + j]);
    const f32 g_45 = std::fabs(p[1 + i][0 + j] + p[0 + i][0 + j] + p[0 + i][1 + j] - p[2 + i][1 + j] -
                               p[2 + i][2 + j] - p[1 + i][2 + j]);
    const f32 g_90 = std::fabs(p[0 + i][0 + j] + p[1 + i][0 + j] + p[2 + i][0 + j] - p[0 + i][2 + j] -
                               p[1 + i][2 + j] - p[2 + i][2 + j]);
    const f32 g_135 = std::fabs(p[1 + i][0 + j] + p[2 + i][0 + j] + p[2 + i][1 + j] - p[0 + i][1 + j] -
                                p[0 + i][2 + j] - p[1 + i][2 + j]);

    const f32 g_0_90_max = gpu_max(g_0, g_90);
    const f32 g_0_90_min = gpu_min(g_0, g_90);
    const f32 g_45_135_max = gpu_max(g_45, g_135);
    const f32 g_45_135_min = gpu_min(g_45, g_135);

    if (g_0_90_max + g_45_135_max == 0.f) {
        return Weights{};
    }

    const f32 e_0_90 = gpu_min(g_0_90_max / (g_0_90_max + g_45_135_max), 1.0f);
    const f32 e_45_135 = 1.0f - e_0_90;

    const bool c_0_90 =
        (g_0_90_max > (g_0_90_min * k.detect_ratio)) && (g_0_90_max > k.detect_thres) && (g_0_90_max > g_45_135_min);
    const bool c_45_135 = (g_45_135_max > (g_45_135_min * k.detect_ratio)) && (g_45_135_max > k.detect_thres) &&
                          (g_45_135_max > g_0_90_min);
    const bool c_g_0_90 = g_0_90_max == g_0;
    const bool c_g_45_135 = g_45_135_max == g_45;

    const f32 f_e_0_90 = (c_0_90 && c_45_135) ? e_0_90 : 1.0f;
    const f32 f_e_45_135 = (c_0_90 && c_45_135) ? e_45_135 : 1.0f;

    Weights w;
    w.x = (c_0_90 && c_g_0_90) ? f_e_0_90 : 0.0f;
    w.y = (c_0_90 && !c_g_0_90) ? f_e_0_90 : 0.0f;
    w.z = (c_45_135 && c_g_45_135) ? f_e_45_135 : 0.0f;
    w.w = (c_45_135 && !c_g_45_135) ? f_e_45_135 : 0.0f;
    return w;
}

FUSE_HOST_DEVICE inline f32 calc_lti(f32 p0, f32 p1, f32 p2, f32 p3, f32 p4, f32 p5, s32 phase_index,
                                     const NisConstants& k) {
    const bool selector = (phase_index <= static_cast<s32>(kNisPhaseCount / 2u));
    f32 sel = selector ? p0 : p3;
    const f32 a_min = gpu_min(gpu_min(p1, p2), sel);
    const f32 a_max = gpu_max(gpu_max(p1, p2), sel);
    sel = selector ? p2 : p5;
    const f32 b_min = gpu_min(gpu_min(p3, p4), sel);
    const f32 b_max = gpu_max(gpu_max(p3, p4), sel);

    const f32 a_cont = a_max - a_min;
    const f32 b_cont = b_max - b_min;

    const f32 cont_ratio = gpu_max(a_cont, b_cont) / (gpu_min(a_cont, b_cont) + k.eps);
    return (1.0f - saturate((cont_ratio - k.min_contrast_ratio) * k.ratio_norm)) * k.contrast_boost;
}

FUSE_HOST_DEVICE inline f32 eval_poly6(const f32 (&pxl)[6], s32 phase_int, const f32* coef_scale,
                                       const f32* coef_usm, const NisConstants& k) {
    const f32* cs = coef_scale + static_cast<u32>(phase_int) * kNisCoefStride;
    const f32* cu = coef_usm + static_cast<u32>(phase_int) * kNisCoefStride;
    f32 y = 0.f;
    for (u32 i = 0; i < 6u; ++i) {
        y += cs[i] * pxl[i];
    }
    f32 y_usm = 0.f;
    for (u32 i = 0; i < 6u; ++i) {
        y_usm += cu[i] * pxl[i];
    }

    // Piece-wise ramp based on luma (NIS_SCALE_FLOAT == 1).
    const f32 y_scale = 1.0f - saturate((y * (1.0f / 1.f) - k.sharp_start_y) * k.sharp_scale_y);
    // Scale the ramp to sharpen as a function of luma.
    const f32 y_sharpness = y_scale * k.sharp_strength_scale + k.sharp_strength_min;
    y_usm *= y_sharpness;
    // Scale the ramp to limit USM as a function of luma.
    const f32 y_sharpness_limit = (y_scale * k.sharp_limit_scale + k.sharp_limit_min) * y;
    y_usm = gpu_min(y_sharpness_limit, gpu_max(-y_sharpness_limit, y_usm));
    // Reduce ringing.
    y_usm *= calc_lti(pxl[0], pxl[1], pxl[2], pxl[3], pxl[4], pxl[5], phase_int, k);
    return y + y_usm;
}

FUSE_HOST_DEVICE inline f32 filter_normal(const f32 (&p)[6][6], s32 phase_x_frac_int, s32 phase_y_frac_int,
                                          const f32* coef_scale) {
    const f32* cx = coef_scale + static_cast<u32>(phase_x_frac_int) * kNisCoefStride;
    const f32* cy = coef_scale + static_cast<u32>(phase_y_frac_int) * kNisCoefStride;
    f32 h_acc = 0.0f;
    for (u32 j = 0; j < 6u; ++j) {
        f32 v_acc = 0.0f;
        for (u32 i = 0; i < 6u; ++i) {
            v_acc += p[i][j] * cy[i];
        }
        h_acc += v_acc * cx[j];
    }
    return h_acc;
}

FUSE_HOST_DEVICE inline f32 add_dir_filters(const f32 (&p)[6][6], f32 phase_x_frac, f32 phase_y_frac,
                                            s32 phase_x_frac_int, s32 phase_y_frac_int, const Weights& w,
                                            const f32* coef_scale, const f32* coef_usm, const NisConstants& k) {
    f32 f = 0.f;
    if (w.x > 0.0f) {
        // 0 deg filter
        f32 interp0Deg[6];
        for (u32 i = 0; i < 6u; ++i) {
            interp0Deg[i] = lerp(p[i][2], p[i][3], phase_x_frac);
        }
        f += eval_poly6(interp0Deg, phase_y_frac_int, coef_scale, coef_usm, k) * w.x;
    }
    if (w.y > 0.0f) {
        // 90 deg filter
        f32 interp90Deg[6];
        for (u32 i = 0; i < 6u; ++i) {
            interp90Deg[i] = lerp(p[2][i], p[3][i], phase_y_frac);
        }
        f += eval_poly6(interp90Deg, phase_x_frac_int, coef_scale, coef_usm, k) * w.y;
    }
    if (w.z > 0.0f) {
        // 45 deg filter
        f32 pphase_b45 = 0.5f + 0.5f * (phase_x_frac - phase_y_frac);

        f32 temp_interp45Deg[7];
        temp_interp45Deg[1] = lerp(p[2][1], p[1][2], pphase_b45);
        temp_interp45Deg[3] = lerp(p[3][2], p[2][3], pphase_b45);
        temp_interp45Deg[5] = lerp(p[4][3], p[3][4], pphase_b45);
        {
            pphase_b45 = pphase_b45 - 0.5f;
            const f32 a = (pphase_b45 >= 0.f) ? p[0][2] : p[2][0];
            const f32 b = (pphase_b45 >= 0.f) ? p[1][3] : p[3][1];
            const f32 c = (pphase_b45 >= 0.f) ? p[2][4] : p[4][2];
            const f32 d = (pphase_b45 >= 0.f) ? p[3][5] : p[5][3];
            temp_interp45Deg[0] = lerp(p[1][1], a, std::fabs(pphase_b45));
            temp_interp45Deg[2] = lerp(p[2][2], b, std::fabs(pphase_b45));
            temp_interp45Deg[4] = lerp(p[3][3], c, std::fabs(pphase_b45));
            temp_interp45Deg[6] = lerp(p[4][4], d, std::fabs(pphase_b45));
        }

        f32 interp45Deg[6];
        f32 pphase_p45 = phase_x_frac + phase_y_frac;
        if (pphase_p45 >= 1.f) {
            for (u32 i = 0; i < 6u; i++) {
                interp45Deg[i] = temp_interp45Deg[i + 1u];
            }
            pphase_p45 = pphase_p45 - 1.f;
        } else {
            for (u32 i = 0; i < 6u; i++) {
                interp45Deg[i] = temp_interp45Deg[i];
            }
        }
        f += eval_poly6(interp45Deg, static_cast<s32>(pphase_p45 * 64.f), coef_scale, coef_usm, k) * w.z;
    }
    if (w.w > 0.0f) {
        // 135 deg filter
        f32 pphase_b135 = 0.5f * (phase_x_frac + phase_y_frac);

        f32 temp_interp135Deg[7];
        temp_interp135Deg[1] = lerp(p[3][1], p[4][2], pphase_b135);
        temp_interp135Deg[3] = lerp(p[2][2], p[3][3], pphase_b135);
        temp_interp135Deg[5] = lerp(p[1][3], p[2][4], pphase_b135);
        {
            pphase_b135 = pphase_b135 - 0.5f;
            const f32 a = (pphase_b135 >= 0.f) ? p[5][2] : p[3][0];
            const f32 b = (pphase_b135 >= 0.f) ? p[4][3] : p[2][1];
            const f32 c = (pphase_b135 >= 0.f) ? p[3][4] : p[1][2];
            const f32 d = (pphase_b135 >= 0.f) ? p[2][5] : p[0][3];
            temp_interp135Deg[0] = lerp(p[4][1], a, std::fabs(pphase_b135));
            temp_interp135Deg[2] = lerp(p[3][2], b, std::fabs(pphase_b135));
            temp_interp135Deg[4] = lerp(p[2][3], c, std::fabs(pphase_b135));
            temp_interp135Deg[6] = lerp(p[1][4], d, std::fabs(pphase_b135));
        }

        f32 interp135Deg[6];
        f32 pphase_p135 = 1.f + (phase_x_frac - phase_y_frac);
        if (pphase_p135 >= 1.f) {
            for (u32 i = 0; i < 6u; ++i) {
                interp135Deg[i] = temp_interp135Deg[i + 1u];
            }
            pphase_p135 = pphase_p135 - 1.f;
        } else {
            for (u32 i = 0; i < 6u; ++i) {
                interp135Deg[i] = temp_interp135Deg[i];
            }
        }
        f += eval_poly6(interp135Deg, static_cast<s32>(pphase_p135 * 64.f), coef_scale, coef_usm, k) * w.w;
    }
    return f;
}

} // namespace nis_detail

/// NVScaler output pixel (dst_x, dst_y). `coef_scale` / `coef_usm` are kNisCoefCount floats each.
FUSE_HOST_DEVICE inline math::Vec4 nis_pixel(const RgbaSurface& src, const NisConstants& k, const f32* coef_scale,
                                             const f32* coef_usm, u32 dst_x, u32 dst_y) {
    using namespace nis_detail;
    // x / y coordinates inside the input image.
    const f32 srcX = (0.5f + static_cast<f32>(dst_x)) * k.scale_x - 0.5f;
    const f32 srcY = (0.5f + static_cast<f32>(dst_y)) * k.scale_y - 0.5f;
    const f32 floorX = std::floor(srcX);
    const f32 floorY = std::floor(srcY);
    const s32 ix = static_cast<s32>(floorX);
    const s32 iy = static_cast<s32>(floorY);
    // Fractional parts and discretized phases.
    const f32 fx = srcX - floorX;
    const f32 fy = srcY - floorY;
    const s32 fx_int = static_cast<s32>(fx * static_cast<f32>(kNisPhaseCount));
    const s32 fy_int = static_cast<s32>(fy * static_cast<f32>(kNisPhaseCount));

    // 6x6 luma support. Reproduces the shader's tile-origin arithmetic: the tile load computes
    // `srcBlockStartX + px` as int + uint, which GLSL and HLSL evaluate in uint. When upscaling, the first
    // block starts at srcBlockStart = -1, so the 2x2 load batch at tile offset 0 wraps to ~4.3e9 and the sampler
    // clamps it to the far edge: source rows (columns) -3 and -2 read row H-1 (column W-1) instead of row 0.
    // Every later batch and every other block is unaffected (the sum stays >= 0).
    const bool wrapX = k.scale_x < 1.f;
    const bool wrapY = k.scale_y < 1.f;
    f32 p[6][6];
    for (s32 i = 0; i < 6; ++i) {
        s32 row = iy - 2 + i;
        row = (wrapY && row < -1) ? static_cast<s32>(src.height) - 1 : row;
        for (s32 j = 0; j < 6; ++j) {
            s32 col = ix - 2 + j;
            col = (wrapX && col < -1) ? static_cast<s32>(src.width) - 1 : col;
            p[i][j] = get_y(load_clamped(src, col, row));
        }
    }

    // Weights for the directional filters (edge map interpolated at the sub-texel phase).
    Weights edge[2][2];
    for (u32 i = 0; i < 2u; ++i) {
        for (u32 j = 0; j < 2u; ++j) {
            edge[i][j] = edge_map(p, i + 1u, j + 1u, k);
        }
    }
    Weights w;
    w.x = lerp(lerp(edge[0][0].x, edge[0][1].x, fx), lerp(edge[1][0].x, edge[1][1].x, fx), fy);
    w.y = lerp(lerp(edge[0][0].y, edge[0][1].y, fx), lerp(edge[1][0].y, edge[1][1].y, fx), fy);
    w.z = lerp(lerp(edge[0][0].z, edge[0][1].z, fx), lerp(edge[1][0].z, edge[1][1].z, fx), fy);
    w.w = lerp(lerp(edge[0][0].w, edge[0][1].w, fx), lerp(edge[1][0].w, edge[1][1].w, fx), fy);

    // Final luma is a weighted product of directional & normal filters.
    const f32 baseWeight = 1.f - w.x - w.y - w.z - w.w;
    f32 opY = 0.f;
    opY += filter_normal(p, fx_int, fy_int, coef_scale) * baseWeight;
    opY += add_dir_filters(p, fx, fy, fx_int, fy_int, w, coef_scale, coef_usm, k);

    // Bilinear tap for chroma, corrected so it carries the filtered luma.
    math::Vec4 op = sample_bilinear(src, srcX + 0.5f, srcY + 0.5f);
    const f32 y = get_y(op);
    const f32 corr = opY * (1.0f / 1.f) - y;
    op.x += corr;
    op.y += corr;
    op.z += corr;
    return op;
}

struct NisParams {
    RgbaSurface src{};
    RgbaTarget dst{};
    NisConstants k{};
    const f32* coef_scale = nullptr; ///< kNisCoefCount floats (NIS_Config.h coef_scale).
    const f32* coef_usm = nullptr;   ///< kNisCoefCount floats (NIS_Config.h coef_usm).
};

struct NisKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const NisParams& p) const {
        p.dst.data[idx.global.y * p.dst.width + idx.global.x] =
            nis_pixel(p.src, p.k, p.coef_scale, p.coef_usm, idx.global.x, idx.global.y);
    }
};

} // namespace fuse::renderer::upscale::kernels
