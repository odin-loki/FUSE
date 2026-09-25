#pragma once

// WP-4.1 temporal: records shared by the C++ side and the shaders (shaders/temporal/*.{glsl,comp,slang}),
// plus the explicit jitter convention. Keep them in sync; the static_asserts pin the layouts.
// Device-safe (only <fuse/types.hpp>).
//
// Jitter convention (the TAAU kernel's, taau_kernel.hpp / UpscaleInputs "jitter"): render pixel (i, j)
// samples the UNJITTERED scene at continuous render coordinate (i + 0.5 + jitter.x, j + 0.5 + jitter.y),
// +x right, +y down, in render pixels. The visibility buffer is therefore drawn with
//     drawViewProj = jitter_view_proj(viewProj, jitter)       (clip.xy -= 2 jitter / size * clip.w)
// in Vulkan clip space (NDC y down, the visbuffer / resolve convention), which moves every projected point by
// -jitter pixels, so the pixel centre (i + 0.5) sees the point whose unjittered position is (i + 0.5 + jitter).
// This equals jitterProjection(viewProj, upscaleJitterNdc(jitter)) = IJitterProvider::offset_ndc
// (upscale_inputs.hpp, NDC offset (-2 jx / w, -2 jy / h); the x sign there was fixed in the WP-4.2 follow-up),
// pinned by gate fuse_rp_fsr3_jitter.
//
// Motion vectors ("temporal.motion"): per render pixel, UV motion current - previous, both from the UNJITTERED
// projections (the UpscaleInputs contract), so no jitter term ever reaches the TAAU:
//   geometry  the visibility sample's triangle, barycentrics at the pixel centre under drawViewProj (the
//             WP-1.4 decode math), object-space point P = sum b_i v_i, uv_cur = proj(viewProj, T_cur P),
//             uv_prev = proj(prevViewProj, T_prev P) with the WP-1.1 previous transform of the instance
//   sky       rotation-only reprojection of the sample direction: [x y w]_prev = H [ndc_x ndc_y 1]_cur with
//             H = A_prev A_cur^-1, A = rows (x, y, w) x columns (0, 1, 2) of the view-projection (translation
//             never moves the sky); ndc of the jittered sample position (i + 0.5 + jitter)
//   depth     linear view depth = clip.w of the unjittered current projection (perspective: w = -z_view);
//             0 marks the sky / no geometry (TAAU convention)

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::temporal {

/// Workgroup of both kernels (8 x 8 pixels).
inline constexpr u32 kTileSize = 8u;

/// MotionFrameConstants::flags.
enum MotionFlag : u32 {
    kMotionSkyValid = 1u << 0, ///< skyReproj holds H (the current view-projection's direction part is invertible)
};

/// Per-frame constants of "temporal.motion", read through BDA from a host-visible ring: 288 bytes, std430
/// (FuseTmMotionFrame in tm_common.glsl, TmMotionFrame in tm_common.slang).
struct MotionFrameConstants {
    f32 drawViewProj[16] = {}; ///< column-major, the (jittered) matrix the visibility buffer was drawn with
    f32 viewProj[16] = {};     ///< column-major, unjittered, this frame
    f32 prevViewProj[16] = {}; ///< column-major, unjittered, last frame
    f32 skyReproj[12] = {};    ///< H columns (x, y, w, pad) x 3
    u64 motion = 0;            ///< BDA: f32 x 2 per render pixel (UV motion)
    u64 depth = 0;             ///< BDA: f32 per render pixel (linear view depth, 0 = sky)
    u32 width = 0;
    u32 height = 0;
    u32 scene = 0; ///< GpuScene::headerHandle()
    u32 vis = 0;   ///< bindless storage-image handle of the R32G32_UINT visibility image
    f32 jitterX = 0.f;
    f32 jitterY = 0.f;
    u32 flags = 0; ///< MotionFlag
    u32 pad = 0;
};
static_assert(sizeof(MotionFrameConstants) == 288u && offsetof(MotionFrameConstants, skyReproj) == 192u &&
                  offsetof(MotionFrameConstants, motion) == 240u && offsetof(MotionFrameConstants, width) == 256u &&
                  offsetof(MotionFrameConstants, jitterX) == 272u,
              "MotionFrameConstants layout (tm_common.glsl / .slang)");

/// Per-frame constants of "taau.resolve": the scalar part of taau_kernel::Params plus the addresses and
/// handles of its spans. 224 bytes, std430 (FuseTmTaauFrame / TmTaauFrame).
struct TaauFrameConstants {
    u64 depth = 0;      ///< BDA f32[render]
    u64 motion = 0;     ///< BDA f32x2[render]
    u64 prevDepth = 0;  ///< BDA f32[render] (read only when hasPrev)
    u64 prevMotion = 0; ///< BDA f32x2[render] (read only when hasPrev)
    u64 historyIn = 0;  ///< BDA f32x4[display]
    u64 historyOut = 0; ///< BDA f32x4[display]
    u32 renderW = 0;
    u32 renderH = 0;
    u32 displayW = 0;
    u32 displayH = 0;
    f32 jitterX = 0.f;
    f32 jitterY = 0.f;
    f32 exposure = 1.f;
    u32 historyValid = 0;
    u32 hasCamera = 0;
    u32 hasPrev = 0; ///< prev_depth / prev_motion spans non-empty
    f32 tanHalfX = 1.f;
    f32 tanHalfY = 1.f;
    f32 curToPrevView[12] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f}; ///< 3x4 row-major
    u32 color = 0;        ///< bindless sampled-image handle (render resolution, .rgb)
    u32 reactive = 0;     ///< bindless sampled-image handle (.r), 0 = none
    u32 transparency = 0; ///< bindless sampled-image handle (.r), 0 = none
    u32 output = 0;       ///< bindless storage-image handle of the RGBA16F display output
    // taau_kernel::Settings, same order.
    f32 maxAccumulation = 10.f;
    f32 accumulationMotionFalloff = 8.f;
    f32 clampGamma = 1.25f;
    f32 depthRejection = 0.05f;
    f32 velocityRejectionPx = 1.f;
    f32 clipFullMotionPx = 0.25f;
    f32 staticClipStrength = 0.f;
    f32 spatialWeight = 0.05f;
    f32 sampleKernelScale = 1.f;
    f32 reactiveStrength = 1.f;
    f32 transparencyClip = 1.f;
    u32 historyFilter = 1u;
    u32 dilateMotion = 1u;
    f32 dilateDepthThreshold = 0.1f;
    u32 pad0 = 0;
    u32 pad1 = 0;
};
static_assert(sizeof(TaauFrameConstants) == 224u && offsetof(TaauFrameConstants, renderW) == 48u &&
                  offsetof(TaauFrameConstants, hasCamera) == 80u && offsetof(TaauFrameConstants, curToPrevView) == 96u &&
                  offsetof(TaauFrameConstants, color) == 144u && offsetof(TaauFrameConstants, maxAccumulation) == 160u &&
                  offsetof(TaauFrameConstants, historyFilter) == 204u,
              "TaauFrameConstants layout (tm_common.glsl / .slang)");

/// Push constants of every WP-4.1 pipeline: 16 bytes.
struct TemporalPush {
    u64 frame = 0; ///< BDA of this frame's constants
    u64 out = 0;   ///< taau.resolve: optional f32x4 dump of the display output (parity gates), else 0
};
static_assert(sizeof(TemporalPush) == 16u, "TemporalPush layout");

/// drawViewProj for a render-pixel jitter (see the header comment): row r (r = 0, 1) -= (2 jitter_r / size_r)
/// * row 3, column-major. Every WP-4.1 producer and gate derives the draw matrix with this one function.
FUSE_HOST_DEVICE inline void jitter_view_proj(const f32 viewProj[16], f32 jitterX, f32 jitterY, u32 width, u32 height,
                                              f32 out[16]) {
    const f32 dx = 2.f * jitterX / static_cast<f32>(width);
    const f32 dy = 2.f * jitterY / static_cast<f32>(height);
    for (u32 c = 0; c < 4u; ++c) {
        out[c * 4u + 0u] = viewProj[c * 4u + 0u] - dx * viewProj[c * 4u + 3u];
        out[c * 4u + 1u] = viewProj[c * 4u + 1u] - dy * viewProj[c * 4u + 3u];
        out[c * 4u + 2u] = viewProj[c * 4u + 2u];
        out[c * 4u + 3u] = viewProj[c * 4u + 3u];
    }
}

/// H = A_prev A_cur^-1 (sky reprojection, see the header comment), in f64, stored as 3 columns padded to 4.
/// False (H = 0) when A_cur is singular (e.g. an orthographic projection: the sky then gets zero motion).
inline bool sky_reprojection(const f32 viewProj[16], const f32 prevViewProj[16], f32 out[12]) {
    for (u32 i = 0; i < 12u; ++i) {
        out[i] = 0.f;
    }
    const u32 rows[3] = {0u, 1u, 3u};
    f64 a[3][3];
    f64 b[3][3];
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 c = 0; c < 3u; ++c) {
            a[r][c] = static_cast<f64>(viewProj[c * 4u + rows[r]]);
            b[r][c] = static_cast<f64>(prevViewProj[c * 4u + rows[r]]);
        }
    }
    const f64 det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                    a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    const f64 scale = a[0][0] * a[0][0] + a[1][1] * a[1][1] + a[2][2] * a[2][2] + a[2][0] * a[2][0] + a[2][1] * a[2][1];
    if (!(det != 0.0) || det * det <= 1e-24 * scale * scale * scale) {
        return false;
    }
    f64 inv[3][3];
    inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 c = 0; c < 3u; ++c) {
            f64 s = 0.0;
            for (u32 k = 0; k < 3u; ++k) {
                s += b[r][k] * inv[k][c];
            }
            out[c * 4u + r] = static_cast<f32>(s);
        }
    }
    return true;
}

} // namespace fuse::renderer::temporal
