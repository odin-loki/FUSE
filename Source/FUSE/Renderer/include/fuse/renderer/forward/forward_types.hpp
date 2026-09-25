#pragma once

// WP-2.3 forward transparency: records shared by the C++ side and the shaders
// (shaders/forward/fw_*.{glsl,vert,frag,slang}). Keep them in sync; the static_asserts pin the layouts.
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::forward {

/// Colour target of the forward pass (VK_FORMAT_R16G16B16A16_SFLOAT): the clustered lighting's lit
/// image is copied into it, then the transparent draws blend over it.
inline constexpr u32 kForwardColorFormat = 97u;

/// ForwardFrameConstants::flags: participating media applied to every transparent fragment's radiance before
/// the premultiplied blend (exact for the over operator: out = a (c T + S) + (1 - a) dst, the destination being
/// the already-attenuated opaque scene), in the composer's opaque order (aerial perspective, then fog).
enum ForwardFlag : u32 {
    /// WP-8.2 aerial perspective: rgb x T + S x E_sun, at_aerial(atmosphere, fragment screen uv, |P - camera|)
    kForwardFlagAerial = 1u << 0,
    /// WP-8.1 froxel fog: rgb x T + S, fuse_fog_sample(fog, fragment screen point, view depth)
    kForwardFlagFog = 1u << 1,
};

/// Per-frame constants, read through BDA from a host-visible ring (ForwardTransparency::beginFrame):
/// 128 bytes, std430 (FuseFwFrame in fw_common.glsl, FwFrame in fw_common.slang). The frame's
/// ForwardDraw table follows in the same ring slot.
struct ForwardFrameConstants {
    f32 viewProj[16] = {}; ///< column-major, Vulkan clip space, forward depth (the visibility buffer's)
    u64 lighting = 0;      ///< BDA of this frame's lighting_gpu::LightingFrameConstants (cluster lists, camera)
    u64 draws = 0;         ///< BDA of ForwardDraw[drawCount] (sorted back to front)
    u64 dump = 0;          ///< BDA of ForwardDumpTexel[drawCount * width * height] (parity gates), 0 = none
    u32 scene = 0;         ///< GpuScene::headerHandle()
    u32 sampler = 0;       ///< bindless sampler handle for the material textures
    u32 width = 0;
    u32 height = 0;
    u32 drawCount = 0;
    u32 flags = 0;         ///< ForwardFlag
    u64 atmosphere = 0;    ///< AtParams (AtmosphereGpu::frameAddress()) with kForwardFlagAerial, else 0
    u64 fog = 0;           ///< FogFrameConstants (FroxelFog::frameConstantsAddress()) with kForwardFlagFog, else 0
};
static_assert(sizeof(ForwardFrameConstants) == 128u && offsetof(ForwardFrameConstants, lighting) == 64u &&
                  offsetof(ForwardFrameConstants, scene) == 88u && offsetof(ForwardFrameConstants, drawCount) == 104u &&
                  offsetof(ForwardFrameConstants, atmosphere) == 112u && offsetof(ForwardFrameConstants, fog) == 120u,
              "ForwardFrameConstants layout (fw_common.glsl / .slang)");

/// One transparent draw (gl_InstanceIndex / SV_StartInstanceLocation = its index in the sorted
/// table): 16 bytes.
struct ForwardDraw {
    u32 slot = 0;      ///< GpuScene instance slot
    f32 opacity = 1.f; ///< alpha of every fragment of the instance, [0, 1]
    u32 reserved[2] = {};
};
static_assert(sizeof(ForwardDraw) == 16u, "ForwardDraw layout");

/// Push constants of the forward pipelines: 16 bytes.
struct ForwardPush {
    u64 frame = 0; ///< BDA of this frame's ForwardFrameConstants
    u64 reserved = 0;
};
static_assert(sizeof(ForwardPush) == 16u, "ForwardPush layout");

/// Parity dump of one fragment that passed the depth test (early fragment tests): what the shading saw
/// and returned. Layer d (draw index) of pixel p at [d * width * height + p]. 96 bytes, std430.
/// rt0 / rt1 / rt2 / rt5 are the fragment's surface packed and quantised exactly like the deferred
/// G-buffer (GBufferAttachment RT0 / RT1 / RT2 / RT5, write_gbuffer + the attachment formats), so the
/// CPU shade kernel (lighting_gpu::ShadeKernel) can run on them as on a G-buffer.
struct ForwardDumpTexel {
    f32 radiance[3] = {0.f, 0.f, 0.f}; ///< outgoing radiance (not premultiplied)
    f32 alpha = 0.f;
    f32 rt0[4] = {};
    f32 rt1[4] = {};
    f32 rt2[4] = {};
    f32 rt5[4] = {};
    f32 depth = 1.f;   ///< gl_FragCoord.z (device depth, forward z / w)
    u32 covered = 0u;  ///< 1 when a fragment of this draw was shaded at this pixel
    u32 slot = 0u;     ///< instance slot
    u32 cluster = 0u;  ///< cluster index the fragment used
};
static_assert(sizeof(ForwardDumpTexel) == 96u && offsetof(ForwardDumpTexel, rt0) == 16u &&
                  offsetof(ForwardDumpTexel, depth) == 80u,
              "ForwardDumpTexel layout (fw_common.glsl / .slang)");

} // namespace fuse::renderer::forward
