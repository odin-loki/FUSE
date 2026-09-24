#pragma once

// WP-2.1 GPU clustered lighting: records shared by the C++ side and the kernels
// (shaders/lighting/lc_*.{glsl,comp,slang}). Keep them in sync; the static_asserts pin the layouts.
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::lighting_gpu {

/// Grid limits (ClusterDesc::kMax*; the frame constants carry tables of these sizes).
inline constexpr u32 kMaxTilesX = 32u;
inline constexpr u32 kMaxTilesY = 18u;
inline constexpr u32 kMaxSlicesZ = 64u;
/// Per-cluster list capacity limit (ClusterDesc::kMaxLightsPerCluster).
inline constexpr u32 kMaxLightsPerCluster = 256u;

/// Workgroup sizes of the kernels (lc_common.glsl FUSE_LC_*).
inline constexpr u32 kLinearGroup = 64u; ///< light.bounds, light.bin, light.cull, light.compact
inline constexpr u32 kScanGroup = 256u;  ///< light.scan (one workgroup)
inline constexpr u32 kShadeTile = 8u;    ///< light.shade (8 x 8 pixels per workgroup)

/// LightingFrameConstants::flags.
enum LightingFrameFlag : u32 {
    kFlagReversedZ = 1u << 0, ///< device depth is reversed-Z infinite far (else forward [0,1], 1 = empty)
};

/// Per-frame constants, read through BDA from a host-visible ring (ClusteredLighting::beginFrame):
/// 752 bytes, std430 (FuseLcFrame in lc_common.glsl, LcFrame in lc_common.slang).
///
/// The camera basis, the projection tangents and the three edge tables are resolved on the host with
/// the CPU oracle's own functions (clustered_kernel::make_camera / slice_near_z / slice_far_z and the
/// tile NDC expressions of build_cluster_aabb), so the GPU cluster AABBs and light spheres are the
/// oracle's bit for bit (only IEEE multiplies, adds, min / max on the GPU side; `precise`).
struct LightingFrameConstants {
    // BDA of every section of the two work buffers (ClusteredLighting::Buffers).
    u64 aabbs = 0;          ///< GpuClusterAabb[clusterCount]
    u64 bounds = 0;         ///< GpuLightBounds[lightCapacity]
    u64 sliceCounts = 0;    ///< u32[kMaxSlicesZ]
    u64 sliceLights = 0;    ///< u32[slicesZ * lightCapacity]: slice s owns [s * lightCapacity, + sliceCounts[s])
    u64 clusterCounts = 0;  ///< u32[clusterCount]: lights stored per cluster (<= capacity)
    u64 clusterDropped = 0; ///< u32[clusterCount]: intersecting lights dropped at capacity
    u64 clusterSlots = 0;   ///< u32[clusterCount * capacity]: fixed-capacity lists (ascending light slot)
    u64 listHeader = 0;     ///< LightListHeader
    u64 grid = 0;           ///< u32x2[clusterCount]: (offset, count) into lightList == ClusterGridEntry
    u64 directional = 0;    ///< u32[lightCapacity]: directional light slots, ascending
    u64 lightList = 0;      ///< u32[clusterCount * capacity]: flat light list (ClusterGridSoA::lightList)
    u64 reserved64 = 0;
    f32 cameraPosition[3] = {0.f, 0.f, 0.f};
    f32 nearPlane = 0.1f;
    f32 right[3] = {1.f, 0.f, 0.f};
    f32 farPlane = 1000.f;
    f32 up[3] = {0.f, 1.f, 0.f};
    f32 tanX = 1.f;
    f32 back[3] = {0.f, 0.f, 1.f};
    f32 tanY = 1.f;
    f32 ambient[3] = {0.f, 0.f, 0.f}; ///< ambient radiance (x albedo x AO)
    u32 flags = 0;                    ///< LightingFrameFlag
    u32 width = 0;
    u32 height = 0;
    f32 invWidth = 0.f;
    f32 invHeight = 0.f;
    u32 tilesX = 0;
    u32 tilesY = 0;
    u32 slicesZ = 0;
    u32 capacity = 0;      ///< per-cluster list capacity (1..kMaxLightsPerCluster)
    u32 clusterCount = 0;
    u32 lightCount = 0;    ///< scene light slots to process (GpuScene::lightHighWater())
    u32 lightCapacity = 0; ///< rows of the per-light sections
    u32 scene = 0;         ///< GpuScene::headerHandle()
    u32 output = 0;        ///< bindless storage-image handle of the RGBA16F lit target
    u32 gbufferNormalAo = 0;   ///< bindless sampled-image handles of the G-buffer (GBufferAttachment RT0,
    u32 gbufferAlbedo = 0;     ///< RT1, RT2, RT4 and RT5), read with texelFetch
    u32 gbufferRoughMetal = 0;
    u32 gbufferDepth = 0;
    u32 gbufferEmissive = 0;
    u32 reserved0 = 0;
    u32 reserved1 = 0;
    f32 sliceDepth[68] = {}; ///< [s] = near depth of slice s, [slicesZ] = far depth of the last slice
    f32 ndcX[36] = {};       ///< [x] = NDC x of tile column edge x (0..tilesX)
    f32 ndcY[20] = {};       ///< [y] = NDC y of tile row edge y (0..tilesY; row 0 = top, ndc +1)
};
static_assert(sizeof(LightingFrameConstants) == 752u && offsetof(LightingFrameConstants, cameraPosition) == 96u &&
                  offsetof(LightingFrameConstants, ambient) == 160u && offsetof(LightingFrameConstants, width) == 176u &&
                  offsetof(LightingFrameConstants, tilesX) == 192u && offsetof(LightingFrameConstants, clusterCount) == 208u &&
                  offsetof(LightingFrameConstants, output) == 224u && offsetof(LightingFrameConstants, gbufferDepth) == 240u &&
                  offsetof(LightingFrameConstants, sliceDepth) == 256u && offsetof(LightingFrameConstants, ndcX) == 528u &&
                  offsetof(LightingFrameConstants, ndcY) == 672u,
              "LightingFrameConstants layout (lc_common.glsl / .slang)");

/// Push constants of every WP-2.1 pipeline: 16 bytes.
struct LightingPush {
    u64 frame = 0; ///< BDA of this frame's LightingFrameConstants
    u64 out = 0;   ///< light.shade: BDA of an f32x4[width * height] radiance dump (parity), 0 = none
};
static_assert(sizeof(LightingPush) == 16u, "LightingPush layout");

/// View-space AABB of one cluster (ClusterAABB with std430 padding): 32 bytes.
struct GpuClusterAabb {
    f32 minP[3] = {0.f, 0.f, 0.f};
    f32 pad0 = 0.f;
    f32 maxP[3] = {0.f, 0.f, 0.f};
    f32 pad1 = 0.f;
};
static_assert(sizeof(GpuClusterAabb) == 32u, "GpuClusterAabb layout");

/// View-space sphere of one light slot and its candidate depth-slice window (ClusterLightBounds):
/// `sliceLo > sliceHi` = the slot occupies no cluster (free slot, directional light, no range).
/// 32 bytes.
struct GpuLightBounds {
    f32 center[3] = {0.f, 0.f, 0.f};
    f32 radius = 0.f;
    u32 sliceLo = 1u;
    u32 sliceHi = 0u;
    u32 type = 0u; ///< GpuLightType
    u32 pad = 0u;
};
static_assert(sizeof(GpuLightBounds) == 32u, "GpuLightBounds layout");

/// Head of the lists buffer (light.scan / light.bin): 64 bytes.
struct LightListHeader {
    u32 totalEntries = 0;       ///< sum of the per-cluster counts (flat list length)
    u32 clustersAtCapacity = 0; ///< clusters that dropped lights (ClusterCullResult::clustersAtCapacity)
    u32 droppedTotal = 0;       ///< (cluster, light) pairs dropped at capacity
    u32 nonEmptyClusters = 0;
    u32 directionalCount = 0;   ///< entries of the directional list (light.bin)
    u32 clusterCount = 0;
    u32 capacity = 0;
    u32 lightCount = 0;
    u32 reserved[8] = {};
};
static_assert(sizeof(LightListHeader) == 64u, "LightListHeader layout");

/// Candidate slice window widening of the GPU light bounds. The CPU oracle widens the log-mapped
/// window by one slice so float rounding can never skip a cluster, and the exact sphere / AABB test
/// decides membership; the GPU's log differs from libm by a few ulp, so it widens by two. The window
/// is only a candidate filter: a wider window changes no list (every cluster in the extra slices
/// fails the exact test the oracle's window already excluded it by a whole slice).
inline constexpr u32 kSliceWindowPad = 2u;

} // namespace fuse::renderer::lighting_gpu
