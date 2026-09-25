#pragma once
// Frame composer (docs/unification/RENDERER-EXECUTION.md, "Frame integration"): the GPU records of the
// composer's own kernel (shaders/frame/fc_frame.{comp,slang}, fc_common.{glsl,slang}) and its CPU
// reference. The composer's kernel only glues the packages together; every effect is the package's own:
//
//   mode Sky          frame.sky        background pixels (RT4 depth == 1, forward z) take the WP-8.2 sky
//                                      radiance (+ sun disk); geometry pixels take the aerial perspective
//                                      colour x T + S x E_sun (at_aerial); an off flag keeps the input bits
//   mode Gather       frame.gather     HDR image -> f32x4 background + f32 view distance (the WP-8.3
//                                      clouds' CloudFrame::backgroundAddress / depthAddress)
//   mode Resolve      frame.resolve    render-resolution scene colour: clouds result (or the HDR image),
//                                      then the WP-9.2 splats (rgb x T + c), alpha 1
//   mode ShadowPack   frame.shadow_pack the WP-6.4 denoised shadow (f32x4, .x) -> the f32 visibility plane
//                                      light.shade reads through an RtfxShadowView (WP-6.2 layout)
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::frame {

inline constexpr u32 kFrameTile = 8u; ///< 8 x 8 threads per workgroup (every mode)

enum FrameKernelMode : u32 {
    kFrameModeSky = 0,
    kFrameModeGather = 1,
    kFrameModeResolve = 2,
    kFrameModeShadowPack = 3,
    kFrameModeCount = 4,
};

/// FrameConstants::flags
enum FrameFlag : u32 {
    kFrameFlagSky = 1u << 0,     ///< Sky: background pixels take the sky radiance
    kFrameFlagAerial = 1u << 1,  ///< Sky: geometry pixels take the aerial perspective
    kFrameFlagSunDisk = 1u << 2, ///< Sky: + the transmitted sun disk
    kFrameFlagClouds = 1u << 3,  ///< Resolve: read the clouds result instead of the HDR image
    kFrameFlagSplats = 1u << 4,  ///< Resolve: composite the splats (rgb x T + c)
};

/// Per-frame constants of the composer's kernel, read through BDA from a host-visible ring: 192 bytes.
/// Mirrors: shaders/frame/fc_common.glsl / .slang (checked by fuse_rp_frame_layout).
struct FrameConstants {
    u64 atmosphere = 0; ///< AtmosphereGpu::frameAddress() (Sky)
    u64 background = 0; ///< f32x4 per pixel (Gather out)
    u64 distance = 0;   ///< f32 per pixel (Gather out)
    u64 clouds = 0;     ///< f32x4 per pixel: VolumetricClouds::resultAddress() (Resolve in)
    u64 splats = 0;     ///< f32x4 (sum c alpha T, T) per pixel: GsplatRenderer::outputAddress() (Resolve in)
    u64 denoised = 0;   ///< f32x4 per pixel: SvgfDenoiser::outputAddress() (ShadowPack in)
    u64 visibility = 0; ///< f32 per pixel (ShadowPack out)
    u64 reserved0 = 0;
    u32 width = 0;
    u32 height = 0;
    u32 inColor = 0;    ///< bindless sampled handle (Sky / Gather / Resolve)
    u32 inDepth = 0;    ///< bindless sampled handle of RT4 (R32F forward device depth)
    u32 outSky = 0;     ///< bindless storage handle (RGBA16F)
    u32 outResolve = 0; ///< bindless storage handle (RGBA16F)
    u32 flags = 0;
    u32 reserved1 = 0;
    f32 invViewProj[16] = {}; ///< column-major clip -> world of the depth's (jittered) projection
    f32 cameraPos[4] = {0.f, 0.f, 0.f, 0.f};
    f32 skyDistance = 1.0e30f; ///< Gather: view distance written for background pixels
    f32 reserved2[3] = {0.f, 0.f, 0.f};
};
static_assert(sizeof(FrameConstants) == 192u, "FrameConstants layout (fc_common.glsl / .slang)");

/// Push constants of every mode: 16 bytes.
struct FramePush {
    u64 frame = 0; ///< BDA of this frame's FrameConstants
    u32 mode = 0;  ///< FrameKernelMode
    u32 reserved = 0;
};
static_assert(sizeof(FramePush) == 16u, "FramePush layout");

// --- CPU reference (the kernels' twins, same f32 operations and order) ------------------------------------

/// World point of pixel (x, y) at device depth `depth` (clip = ((x + .5) / w * 2 - 1, (y + .5) / h * 2 - 1,
/// depth, 1) through invViewProj, divided by w).
inline void frame_unproject(const FrameConstants& c, u32 x, u32 y, f32 depth, f32 (&out)[3]) {
    const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(c.width) * 2.f - 1.f;
    const f32 ny = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(c.height) * 2.f - 1.f;
    const f32* m = c.invViewProj;
    const f32 px = m[0] * nx + m[4] * ny + m[8] * depth + m[12];
    const f32 py = m[1] * nx + m[5] * ny + m[9] * depth + m[13];
    const f32 pz = m[2] * nx + m[6] * ny + m[10] * depth + m[14];
    const f32 pw = m[3] * nx + m[7] * ny + m[11] * depth + m[15];
    out[0] = px / pw;
    out[1] = py / pw;
    out[2] = pz / pw;
}

/// Gather: the view distance of pixel (x, y) (skyDistance for background pixels).
inline f32 frame_view_distance(const FrameConstants& c, u32 x, u32 y, f32 depth) {
    if (!(depth < 1.f)) {
        return c.skyDistance;
    }
    f32 p[3];
    frame_unproject(c, x, y, depth, p);
    const f32 dx = p[0] - c.cameraPos[0];
    const f32 dy = p[1] - c.cameraPos[1];
    const f32 dz = p[2] - c.cameraPos[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// Resolve: scene colour from the base colour (clouds result or HDR image) and the splat texel.
inline void frame_resolve_texel(const FrameConstants& c, const f32 (&base)[4], const f32 (&splat)[4], f32 (&out)[4]) {
    f32 r = base[0];
    f32 g = base[1];
    f32 b = base[2];
    if ((c.flags & kFrameFlagSplats) != 0u) {
        r = r * splat[3] + splat[0];
        g = g * splat[3] + splat[1];
        b = b * splat[3] + splat[2];
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = 1.f;
}

/// ShadowPack: the visibility light.shade reads from the denoised texel.
inline f32 frame_pack_visibility(const f32 (&denoised)[4]) { return std::min(1.f, std::max(0.f, denoised[0])); }

} // namespace fuse::renderer::frame
