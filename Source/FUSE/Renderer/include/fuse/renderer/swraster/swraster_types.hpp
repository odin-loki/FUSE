#pragma once

// WP-5.4 compute software rasteriser: records shared by the C++ side (SwRasterizer, the CPU
// reference kernels in swraster_kernel.hpp) and the shaders (shaders/swraster/swraster_common.
// {glsl,slang}). Keep all three in sync; the static_asserts pin sizes and offsets, and
// fuse_rp_swraster checks GPU == CPU bit for bit.
//
// Work decomposition per cull phase (see swraster.hpp for the frame):
//   "swraster.expand<p>"   one invocation per WP-1.3 culler draw slot of phase p: the drawn instance's
//                          meshlets become ceil(meshletCount / 32) SwGroup records of region p - 1,
//                          and the region's classify dispatch x = records;
//   "swraster.classify<p>" one invocation per meshlet (32 per record, dispatch indirect): frustum
//                          test and SW / HW decision (swraster_kernel.hpp classify_cluster); the
//                          meshlet is appended as an SwCluster to the region's SW or HW list, the SW
//                          dispatch x / HW draw instanceCount follow the lists;
//   "swraster.sw<p>"       one 64-invocation workgroup per SW cluster (dispatch indirect): vertices to
//                          fixed point, then one invocation per triangle, 64-bit atomicMin of the
//                          WP-1.4 word into the Atomic64 target (clusters it cannot rasterise exactly
//                          are demoted to the region's HW list);
//   "swraster.hw<p>"       one instanced non-indexed draw (instance = HW cluster, 372 vertices = 124
//                          triangle slots, unused slots degenerate) through the same vertex transform,
//                          fragment atomicMin of the same word into the same target.
//
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::swraster {

/// Sub-pixel precision of the rasteriser: 8 fractional bits (1/256 px; == Vulkan's minimum
/// viewportSubPixelBits of Lavapipe and most desktop GPUs).
inline constexpr u32 kSwSubpixelBits = 8u;
inline constexpr s32 kSwSubpixelOne = 1 << kSwSubpixelBits;
inline constexpr s32 kSwSubpixelHalf = kSwSubpixelOne / 2;

/// Meshlets per classify record (one invocation each).
inline constexpr u32 kSwGroupSize = 32u;
/// Rasteriser workgroup (vertices, then triangles strided over it).
inline constexpr u32 kSwRasterThreads = 64u;
/// Expand workgroup (one invocation per culler draw slot).
inline constexpr u32 kSwExpandWorkgroup = 64u;
/// Meshlet limits of both paths (the WP-1.2 cook limits); larger meshlets are not drawn (counted).
inline constexpr u32 kSwMaxVertices = 64u;
inline constexpr u32 kSwMaxTriangles = 124u;
/// Vertices of one HW cluster instance (every triangle slot; slots past the meshlet's count emit a
/// degenerate, clipped triangle).
inline constexpr u32 kSwHwVertices = kSwMaxTriangles * 3u;

/// Largest cluster screen extent (px) the classifier may send to the SW path (a hard cap on
/// SwRasterFrameDesc::maxClusterPixels): with the demotion bound below it keeps every edge function
/// and bounding-box step inside 32-bit integers.
inline constexpr u32 kSwMaxClusterPixels = 32u;
/// The SW kernel demotes a cluster to HW when its snapped vertices span more than this (px) in x or
/// y, or when a vertex is not strictly in front / inside the depth range (the classifier's corner
/// bound already guarantees neither happens up to f32 rounding; the gate counts demotions).
/// Edge-function magnitudes: |d| <= span, |p - a| <= span + 1 px, so |E| < 2 * (49 * 256)^2 < 2^29.
inline constexpr u32 kSwMaxSpanPixels = 48u;
inline constexpr s32 kSwMaxSpanSubpixels = static_cast<s32>(kSwMaxSpanPixels) << kSwSubpixelBits;
/// Projected coordinates beyond +-2^30 sub-pixels are "not representable" (vertex invalid).
inline constexpr s64 kSwCoordLimit = s64{1} << 30;

/// Per-meshlet classification (results buffer and the CPU reference).
enum SwResult : u32 {
    kSwResultNone = 0u,           ///< lane past the mesh's meshlet count
    kSwResultCulled = 1u,         ///< outside the view (all AABB corners beyond one clip plane / off-screen rect)
    kSwResultSoftware = 2u,       ///< software rasteriser
    kSwResultHardwareSize = 3u,   ///< estimated triangle size above the threshold
    kSwResultHardwareExtent = 4u, ///< cluster rect larger than maxClusterPixels
    kSwResultHardwareClip = 5u,   ///< a corner at / behind the near plane, beyond the far plane or unprojectable
    kSwResultHardwareForced = 6u, ///< SwRasterMode::ForceHardware
    kSwResultOversize = 7u,       ///< above kSwMaxVertices / kSwMaxTriangles: not drawn
    kSwResultCount = 8u,
};

/// SwRasterConstants::mode.
enum SwRasterModeValue : u32 {
    kSwModeClassify = 0u,      ///< SW iff the estimated triangle size <= threshold (and SW-safe)
    kSwModeForceSoftware = 1u, ///< every SW-safe cluster (in front, extent <= maxClusterPixels) -> SW
    kSwModeForceHardware = 2u, ///< every cluster -> HW
};

/// SwRasterConstants::flags.
enum SwRasterFlag : u32 {
    kSwFlagWriteResults = 1u << 0, ///< write an SwResult per meshlet slot (debug / parity)
    kSwFlagSkipSoftware = 1u << 1, ///< debug: the SW pass rasterises nothing (lists still built)
    kSwFlagSkipHardware = 1u << 2, ///< debug: the HW pass draws nothing (lists still built)
};

/// Words of the counts buffer (u32, 128 bytes).
enum SwCount : u32 {
    kSwCountClassify0 = 0u,   ///< region 0 classify VkDispatchIndirectCommand {records, 1, 1}
    kSwCountClassify1 = 3u,   ///< region 1
    kSwCountSoftware0 = 6u,   ///< region 0 SW VkDispatchIndirectCommand {clusters, 1, 1}
    kSwCountSoftware1 = 9u,
    kSwCountHardware0 = 12u,  ///< region 0 HW VkDrawIndirectCommand {kSwHwVertices, clusters, 0, 0}
    kSwCountHardware1 = 16u,
    kSwCountGroupsRequested0 = 20u, ///< + region
    kSwCountSwRequested0 = 22u,     ///< + region (SW list appends, may exceed the capacity)
    kSwCountHwRequested0 = 24u,     ///< + region (HW list appends incl. demotions)
    kSwCountOverflow = 26u,   ///< records / clusters dropped for lack of capacity (all lists)
    kSwCountSkipped = 27u,    ///< drawn instances whose mesh has no meshlet data (not drawn)
    kSwCountDemoted = 28u,    ///< SW clusters the SW kernel moved to the HW list
    kSwCountOversize = 29u,   ///< meshlets above the limits (not drawn)
    kSwCountSwTriangles = 30u, ///< triangles the SW kernel set up (statistics)
    kSwCountWords = 32u,
};

/// One classify record: kSwGroupSize meshlets of one instance starting at firstMeshlet.
struct SwGroup {
    u32 instance = 0;
    u32 firstMeshlet = 0;
};
static_assert(sizeof(SwGroup) == 8u, "SwGroup layout");

/// One cluster of the SW or HW list.
struct SwCluster {
    u32 instance = 0;
    u32 meshlet = 0;
};
static_assert(sizeof(SwCluster) == 8u, "SwCluster layout");

/// Per-frame constants (host-visible ring, read through BDA): 144 bytes.
struct SwRasterConstants {
    f32 viewProj[16] = {};       ///< == the VisBuffer's (vis_common fuse_vis_clip), column-major
    u32 scene = 0;               ///< GpuScene::headerHandle()
    u32 width = 0;               ///< target extent (buffer target: row pitch in words)
    u32 height = 0;
    u32 flags = 0;               ///< SwRasterFlag
    u32 mode = kSwModeClassify;  ///< SwRasterModeValue
    u32 triangleThreshold = 0;   ///< sub-pixels: SW iff rectW * rectH <= threshold^2 * triangles
    u32 maxClusterExtent = 0;    ///< sub-pixels (<= kSwMaxClusterPixels * 256)
    u32 capacity = 0;            ///< records / clusters per region of each list
    u32 groupsBuffer = 0;        ///< bindless storage-buffer handles
    u32 swBuffer = 0;
    u32 hwBuffer = 0;
    u32 countsBuffer = 0;
    u32 resultsBuffer = 0;       ///< 0 without kSwFlagWriteResults
    u32 target64 = 0;            ///< VisBuffer::target64Handle() (storage image or storage buffer)
    u32 cullArgsBuffer = 0;      ///< WP-1.3 culler: draw args (5 words per slot, word 4 = instance slot)
    u32 cullCountsBuffer = 0;    ///< WP-1.3 culler: counts (kCountPhase1Draws / kCountPhase2Draws)
    u32 phase2DrawBase = 0;      ///< first phase-2 draw slot
    u32 maxDraws = 0;            ///< draw slots per phase
    u32 reserved[2] = {0u, 0u};
};
static_assert(sizeof(SwRasterConstants) == 144u, "SwRasterConstants layout (swraster_common.glsl / .slang)");
static_assert(offsetof(SwRasterConstants, scene) == 64u && offsetof(SwRasterConstants, capacity) == 92u &&
                  offsetof(SwRasterConstants, groupsBuffer) == 96u && offsetof(SwRasterConstants, target64) == 116u &&
                  offsetof(SwRasterConstants, maxDraws) == 132u,
              "SwRasterConstants offsets (swraster_common.glsl / .slang)");

/// Push constants of every swraster pipeline: 16 bytes.
struct SwRasterPush {
    u64 constants = 0; ///< device address of this frame's SwRasterConstants
    u32 region = 0;    ///< 0 = culler phase 1, 1 = phase 2
    u32 pad = 0;
};
static_assert(sizeof(SwRasterPush) == 16u, "SwRasterPush layout");

} // namespace fuse::renderer::swraster
