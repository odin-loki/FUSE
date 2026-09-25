#pragma once

// WP-3.1 virtual shadow maps: record layouts shared by the C++ side (VirtualShadowMap, the CPU
// reference kernels) and the shaders (vsm_common.{glsl,slang} in this directory). Keep all three in
// sync; every record uses 4-byte scalars, scalar arrays and 8-byte addresses only (no implicit
// padding, pinned by the static_asserts), so std430 / Slang pointers and C++ agree byte for byte.
//
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::vsm {

/// Pages per clipmap-level axis and texels per page axis: 128 x 128 pages of 128 x 128 texels =
/// a 16k x 16k virtual shadow map per level.
inline constexpr u32 kPagesPerAxis = 128u;
inline constexpr u32 kPageTexels = 128u;
inline constexpr u32 kVirtualTexels = kPagesPerAxis * kPageTexels;
inline constexpr u32 kPagesPerLevel = kPagesPerAxis * kPagesPerAxis;
inline constexpr u32 kMaxLevels = 16u;
inline constexpr u32 kMaxVirtualPages = kPagesPerLevel * kMaxLevels;
/// Physical pool: at most 64 x 64 pages (an 8k x 8k R32_UINT atlas).
inline constexpr u32 kMaxPoolPagesPerAxis = 64u;
inline constexpr u32 kMaxPhysPages = kMaxPoolPagesPerAxis * kMaxPoolPagesPerAxis;

/// Page-table entry (u32), identical to core_logic/vsm_pages.hpp.
inline constexpr u32 kPteMapped = 0x80000000u;
inline constexpr u32 kPteCached = 0x40000000u;
inline constexpr u32 kPtePhysMask = 0x00FFFFFFu;
inline constexpr u32 kPageNone = 0xFFFFFFFFu;

/// Counters at the start of the work buffer (u32 words). [0..2] are the VkDispatchIndirectCommand
/// of the render list (one workgroup per page to render): WP-3.2 page rendering and vsm.clear
/// dispatch indirectly from it.
enum VsmCounter : u32 {
    kCounterRenderCount = 0,   ///< render-list length == dispatch x
    kCounterDispatchY = 1,     ///< 1
    kCounterDispatchZ = 2,     ///< 1
    kCounterRequested = 3,
    kCounterAlreadyMapped = 4,
    kCounterNeeded = 5,
    kCounterCandidates = 6,
    kCounterAllocated = 7,
    kCounterEvicted = 8,
    kCounterFailed = 9,
    kCounterInvalidated = 10,  ///< cached bits dropped by vsm.invalidate (GPU-scene bounds changes)
    kCounterScrolled = 11,     ///< cached bits dropped by vsm.update (window moves, level invalidation)
    kCounterCount = 16,
};

/// VsmFrameConstants::flags.
enum VsmFrameFlag : u32 {
    kFrameInvalidateAll = 1u << 0, ///< light rotation changed (or forced): every cached bit is dropped
};

/// One clipmap level for a frame. Level L covers kPagesPerAxis pages of pageWorld = extent0 / 128 * 2^L
/// light-space units; the window is centred on the camera's page (originX, originY), and the table
/// holds absolute page a in slot a mod 128 (toroidal).
struct VsmLevelConstants {
    s32 originX = 0;     ///< window centre page, absolute (floor(cameraLight.x * invPageWorld))
    s32 originY = 0;
    s32 prevOriginX = 0; ///< last frame's (== origin on the first frame)
    s32 prevOriginY = 0;
    f32 invPageWorld = 0.f; ///< 1 / pageWorld (exact power-of-two multiple of level 0's)
    f32 pageWorld = 0.f;
    s32 depthKey = 0;       ///< floor(cameraLight.z / (pageWorld * 32)); the light-space depth range
    s32 prevDepthKey = 0;   ///< of the level is centred on depthKey * depthStep (WP-3.2 raster)
    f32 depthCenter = 0.f;  ///< depthKey * depthStep (light-space z)
    f32 depthStep = 0.f;
    u32 flags = 0;          ///< bit 0: the whole level is invalid this frame (depth key or invalidate-all)
    u32 reserved = 0;
};
static_assert(sizeof(VsmLevelConstants) == 48u, "VsmLevelConstants layout (vsm_common.glsl / .slang)");

/// Per-frame constants (host-visible ring, read through BDA). Buffers are reached through their
/// bindless storage-buffer handles (u32 words), the pool through a storage-image handle.
struct VsmFrameConstants {
    u64 pageTable = 0;   ///< BDA of the page table (u32 PTE per virtual page, level-major, then y, x)
    u64 physMeta = 0;    ///< BDA of the physical metadata (owner, lastUsed) per physical page
    u64 work = 0;        ///< BDA of the work buffer (counters, request / need bits, render list, candidates)
    u64 bounds = 0;      ///< BDA of the instance-bounds ping-pong buffer
    /// Light-space position from a depth-buffer texel: h = depthToLight * (ndcX, ndcY, depth, 1)
    /// (column-major 4x4 = lightRotation * inverse(viewProj)), light = h.xyz / h.w.
    f32 depthToLight[16] = {};
    /// World -> light rotation, row-major 3x4 (translation 0; the stabilised light basis of
    /// CascadeLightSpaceLayout::buildStableLightView): x right, y up, z against the light direction.
    f32 lightRotation[12] = {};
    f32 cameraLight[4] = {}; ///< camera position in light space (w unused)
    f32 levelSelectScale = 0.f; ///< containment: 1 / ((kPagesPerAxis / 2 - 1) * pageWorld of level 0)
    f32 markRadiusPages = 0.f;  ///< marking footprint half-size in pages (radius texels / kPageTexels)
    f32 ndcScaleX = 0.f;        ///< 2 / depthWidth
    f32 ndcScaleY = 0.f;        ///< 2 / depthHeight
    u32 levels = 0;
    u32 pagesPerAxis = kPagesPerAxis;
    u32 pageTexels = kPageTexels;
    u32 physPages = 0;
    u32 poolPagesX = 0;
    u32 frame = 0;       ///< VSM frame counter (>= 1, +1 per beginFrame)
    u32 flags = 0;       ///< VsmFrameFlag
    u32 depthWidth = 0;
    u32 depthHeight = 0;
    u32 depthTexture = 0;   ///< bindless sampled handle of the depth (VisBuffer::depthSampledHandle())
    u32 depthSampler = 0;   ///< bindless nearest sampler (GLSL texelFetch needs a combined sampler)
    u32 instanceCount = 0;  ///< GpuScene::instanceHighWater()
    u32 scene = 0;          ///< GpuScene::headerHandle()
    s32 lodBias = 0;        ///< added to the density level (never finer than the containment level)
    u32 pageTableHandle = 0;
    u32 physMetaHandle = 0;
    u32 workHandle = 0;
    u32 boundsHandle = 0;
    u32 poolHandle = 0;       ///< bindless storage-image handle of the physical pool (R32_UINT)
    u32 clearValue = 0;       ///< vsm.clear value (float bits of the far depth by default)
    u32 virtualPages = 0;     ///< levels * kPagesPerLevel
    u32 requestWords = 0;     ///< (virtualPages + 31) / 32
    u32 offRequest = 0;       ///< work-buffer word offsets
    u32 offNeed = 0;
    u32 offRenderList = 0;    ///< 2 words per entry: virtual page, physical page
    u32 offCandidates = 0;
    u32 boundsRead = 0;       ///< record offset (in records) of last frame's bounds half
    u32 boundsWrite = 0;      ///< record offset of this frame's half
    u32 boundsCapacity = 0;   ///< records per half
    /// Screen-density level selection: q = d * densityScale (= pixelSpread / (texelsPerPixel * level-0
    /// texel size)); the level whose texel is the first >= the pixel footprint / texelsPerPixel. 0 = off.
    f32 densityScale = 0.f;
    u32 reserved[2] = {};
    VsmLevelConstants level[kMaxLevels] = {};
};
static_assert(offsetof(VsmFrameConstants, depthToLight) == 32u && offsetof(VsmFrameConstants, lightRotation) == 96u &&
                  offsetof(VsmFrameConstants, cameraLight) == 144u && offsetof(VsmFrameConstants, levels) == 176u &&
                  offsetof(VsmFrameConstants, level) == 304u,
              "VsmFrameConstants offsets (vsm_common.glsl / .slang)");
static_assert(sizeof(VsmFrameConstants) == 304u + 48u * kMaxLevels, "VsmFrameConstants layout");

/// World-space bounds of one instance slot as the invalidation pass tracks it (32 bytes, compared
/// bit for bit with last frame's record). An untracked slot (free, not a shadow caster, no mesh) is
/// all zero.
struct VsmBoundsRecord {
    f32 center[3] = {0.f, 0.f, 0.f};
    u32 tracked = 0;
    f32 extent[3] = {0.f, 0.f, 0.f};
    u32 reserved = 0;
};
static_assert(sizeof(VsmBoundsRecord) == 32u, "VsmBoundsRecord layout");

/// Push constants of every vsm.* kernel.
struct VsmPush {
    u64 constants = 0; ///< BDA of this frame's VsmFrameConstants
    u32 kernelArg = 0; ///< vsm.clear: unused; reserved
    u32 pad = 0;
};
static_assert(sizeof(VsmPush) == 16u, "VsmPush layout");

/// Workgroup sizes (src/shadow/vsm/shaders/*).
inline constexpr u32 kLinearGroup = 64u;    ///< vsm.update / vsm.render / vsm.invalidate (x)
inline constexpr u32 kMarkTile = 8u;         ///< vsm.mark: 8 x 8 pixels
inline constexpr u32 kAllocThreads = 256u;   ///< vsm.alloc: one workgroup
inline constexpr u32 kClearTile = 16u;       ///< vsm.clear: 16 x 16 threads, 8 x 8 texels each

} // namespace fuse::renderer::vsm
