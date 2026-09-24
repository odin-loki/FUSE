#pragma once

// WP-1.3 instance culling: records shared by the C++ side (InstanceCuller, the CPU reference
// kernels) and the shaders (shaders/culling/cull_common.glsl / .slang). Keep all three in sync;
// the static_asserts pin sizes and offsets, and fuse_rp_culling checks GPU == CPU bit for bit.
//
// Conventions:
//   * matrices are column-major 4x4 (element (row r, column c) at m[c * 4 + r]), Vulkan clip space
//     (0 <= z <= w, y down after the viewport transform), forward depth: near = 0, far = 1,
//     depth test LESS, depth cleared to 1. The Hi-Z pyramid therefore keeps the MAX (farthest)
//     depth of each footprint;
//   * the Hi-Z pyramid is square and power of two: P = nextPow2(max(depthW, depthH)), mip 0 is
//     P/2 x P/2 and each mip-0 texel is the max of a 2x2 depth quad (texels outside the depth
//     image count as far = 1), mip k+1 is the max of a 2x2 quad of mip k (hiz_build_kernel.hpp);
//   * one draw per visible instance: VkDrawIndexedIndirectCommand{3 x mesh.triangleCount, 1, 0,
//     0, instance slot}, so gl_InstanceIndex (SV_InstanceID + SV_StartInstanceLocation) is the
//     GPU-scene slot.
//
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::culling {

/// Hi-Z mip 0 is at most 4096^2 (depth up to 8192^2): 13 levels.
inline constexpr u32 kMaxHizMips = 13u;
inline constexpr u32 kMaxDepthExtent = 8192u;
/// Instance cull workgroup (1D) and Hi-Z build workgroup (16 x 16 threads, 64 x 64 mip-0 tile).
inline constexpr u32 kCullWorkgroup = 64u;
inline constexpr u32 kHizWorkgroup = 256u;
inline constexpr u32 kHizTile = 64u;

/// CullConstants::flags.
enum CullFlag : u32 {
    kCullFrustum = 1u << 0,      ///< frustum test (off: every valid instance passes)
    kCullOcclusion = 1u << 1,    ///< two-phase Hi-Z occlusion (off: phase 1 draws every frustum-visible instance)
    kCullHistoryValid = 1u << 2, ///< last frame's Hi-Z + view exist (off, e.g. camera cut: phase 1 draws nothing)
};

/// Per-instance result word (results buffer, CPU reference). Final values after phase 2:
/// None, FrustumCulled, Phase1Drawn, Phase2Drawn, Occluded.
enum CullResult : u32 {
    kResultNone = 0u,          ///< free slot, hidden, or no mesh
    kResultFrustumCulled = 1u,
    kResultPhase1Drawn = 2u,   ///< visible against last frame's Hi-Z (drawn in phase 1)
    kResultCandidate = 3u,     ///< failed phase 1, re-tested in phase 2 (transient)
    kResultPhase2Drawn = 4u,   ///< disoccluded: visible against this frame's Hi-Z
    kResultOccluded = 5u,
};

/// Words of the counts buffer (u32, 64 bytes). Draw counts feed vkCmdDrawIndexedIndirectCount,
/// words 4..6 are the phase-2 VkDispatchIndirectCommand.
enum CullCount : u32 {
    kCountPhase1Draws = 0u,
    kCountPhase2Draws = 1u,
    kCountCandidates = 2u,
    kCountHizCounter = 3u, ///< single-pass Hi-Z: workgroups done (the last one resets it)
    kCountDispatchX = 4u,
    kCountDispatchY = 5u,
    kCountDispatchZ = 6u,
    kCountWords = 16u,
};
/// Words 0..7 reset at the start of every frame: {0, 0, 0, 0, 0, 1, 1, 0}.
inline constexpr u32 kCountResetWords = 8u;

/// Byte layout of VkDrawIndexedIndirectCommand.
struct DrawIndexedIndirectCommand {
    u32 indexCount = 0;
    u32 instanceCount = 0;
    u32 firstIndex = 0;
    i32 vertexOffset = 0;
    u32 firstInstance = 0;
};
static_assert(sizeof(DrawIndexedIndirectCommand) == 20u, "VkDrawIndexedIndirectCommand is 20 bytes");

/// Per-frame constants, read by both kernels through BDA (push constant `constants`).
struct CullConstants {
    f32 viewProj[16] = {};     ///< this frame, column-major
    f32 prevViewProj[16] = {}; ///< last frame (phase 1 projects last frame's bounds onto last frame's Hi-Z)
    f32 planes[6][4] = {};     ///< this frame's frustum, inward unit normals (geometry::cull_kernel::make_cull_view)
    u32 hizMips[16] = {};      ///< bindless storage-image handle of each Hi-Z mip (r32f, build writes)
    u32 instanceCount = 0;     ///< instance slots to test (GpuScene::instanceHighWater())
    u32 flags = 0;             ///< CullFlag
    u32 hizDim = 0;            ///< Hi-Z mip-0 width == height
    u32 hizMipCount = 0;
    f32 hizScale[2] = {0.f, 0.f}; ///< uv -> mip-0 texel: depthWidth / 2, depthHeight / 2
    u32 maxDraws = 0;          ///< draw slots per phase (args capacity)
    u32 phase2DrawBase = 0;    ///< first draw slot of the phase-2 region (== maxDraws)
    u32 argsBuffer = 0;        ///< bindless storage-buffer handles
    u32 countsBuffer = 0;
    u32 candidatesBuffer = 0;
    u32 resultsBuffer = 0;
    u32 hizTexture = 0;   ///< bindless sampled-image handle of the whole pyramid (all mips; phase tests)
    u32 pointSampler = 0; ///< bindless nearest sampler (GLSL texelFetch needs one)
    u32 reserved[2] = {0u, 0u};
};
static_assert(sizeof(CullConstants) == 352u, "CullConstants layout (cull_common.glsl / .slang)");
static_assert(offsetof(CullConstants, prevViewProj) == 64u && offsetof(CullConstants, planes) == 128u &&
                  offsetof(CullConstants, hizMips) == 224u && offsetof(CullConstants, instanceCount) == 288u &&
                  offsetof(CullConstants, hizScale) == 304u && offsetof(CullConstants, argsBuffer) == 320u,
              "CullConstants offsets (cull_common.glsl / .slang)");

/// Push constants of instance_cull (32 bytes).
struct CullPush {
    u64 constants = 0; ///< device address of this frame's CullConstants
    u32 scene = 0;     ///< GpuScene::headerHandle()
    u32 phase = 1;     ///< 1 or 2
    u32 pad[4] = {0u, 0u, 0u, 0u};
};
static_assert(sizeof(CullPush) == 32u, "CullPush layout");

/// Push constants of hiz_build (32 bytes).
struct HizPush {
    u64 constants = 0;
    u32 depthTexture = 0; ///< bindless sampled-image handle of the depth view (DEPTH aspect)
    u32 depthSampler = 0; ///< bindless sampler handle (nearest; GLSL texelFetch needs one)
    u32 depthWidth = 0;
    u32 depthHeight = 0;
    u32 groupsX = 0; ///< workgroups per row (dispatch is groupsX x groupsX)
    u32 pad = 0;
};
static_assert(sizeof(HizPush) == 32u, "HizPush layout");

} // namespace fuse::renderer::culling
