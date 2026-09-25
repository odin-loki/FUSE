#pragma once

// WP-5.1 mesh-shader path (T1): records shared by the C++ side (MeshletPath, the CPU reference
// kernel) and the shaders (shaders/meshlet/meshlet_common.{glsl,slang}). Keep all three in sync; the
// static_asserts pin sizes and offsets, and fuse_rp_meshlet_path checks GPU == CPU per meshlet.
//
// Work decomposition (see meshlet_path.hpp for the frame):
//   * the WP-1.3 culler decides per INSTANCE (two-phase Hi-Z) and writes its draw args;
//   * "meshlet.expand<p>" turns the culler's phase-p draw list into task-group records
//     MeshletGroup{instance slot, first meshlet}: one per kMeshletTaskGroup meshlets of each drawn
//     instance, appended with atomics into region p of the groups buffer, and writes the
//     VkDrawMeshTasksIndirectCommandEXT {groups, 1, 1} of that region (counts words 0..5);
//   * the task shader (one workgroup per record, one invocation per meshlet) culls each meshlet
//     (frustum, backface cone, Hi-Z) and emits one mesh workgroup per surviving meshlet, compacted
//     in meshlet order; the mesh shader emits the meshlet's vertices and triangles, and the WP-1.4
//     fragment shaders write the visibility buffer (gl_PrimitiveID = the mesh triangle = MTRI index).
//
// Device-safe (only <fuse/types.hpp> and the device-safe WP-1.3 records).

#include <fuse/renderer/culling/cull_types.hpp>
#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::meshlet {

/// Meshlets per task workgroup (one invocation each) == bits of a deferral mask word.
inline constexpr u32 kMeshletTaskGroup = 32u;
/// Mesh workgroup size (the meshlet's vertices and triangles are strided over it).
inline constexpr u32 kMeshletMeshThreads = 64u;
/// Output limits of the mesh pipeline: the WP-1.2 cook limits (geometry::kMeshletMaxVertices /
/// kMeshletMaxTriangles). Meshlets above them are not drawn (counted in kMeshletCountOversize).
inline constexpr u32 kMeshletMaxVertices = 64u;
inline constexpr u32 kMeshletMaxTriangles = 124u;
/// Expand workgroup (one invocation per culler draw slot).
inline constexpr u32 kMeshletExpandWorkgroup = 64u;

/// Relative tolerance of the "linear part is a similarity" test that gates the cone test
/// (the cone of a non-uniformly scaled or sheared meshlet is not transformed; it is skipped).
inline constexpr f32 kMeshletSimilarityTolerance = 1.0e-4f;

/// MeshletConstants::flags.
enum MeshletFlag : u32 {
    kMeshletCullFrustum = 1u << 0,   ///< per-meshlet sphere vs frustum
    kMeshletCullCone = 1u << 1,      ///< per-meshlet backface cone (similarity transforms only)
    kMeshletCullOcclusion = 1u << 2, ///< per-meshlet Hi-Z (needs the culler's kCullOcclusion too)
    kMeshletWriteResults = 1u << 3,  ///< write a MeshletResult per meshlet slot (debug / parity)
};

/// Task shader modes (MeshletDrawPush::mode).
enum MeshletMode : u32 {
    kMeshletModeEarly = 0u,  ///< region 0 (culler phase-1 instances): frustum, cone, LAST frame's Hi-Z
    kMeshletModeLate = 1u,   ///< region 0 again: only the meshlets the early pass deferred, THIS frame's Hi-Z
    kMeshletModePhase2 = 2u, ///< region 1 (culler phase-2 instances): frustum, cone, THIS frame's Hi-Z
};

/// Per-meshlet result word (results buffer, CPU reference), indexed by
/// (region * groupCapacity + group) * kMeshletTaskGroup + lane.
enum MeshletResult : u32 {
    kMeshletResultNone = 0u,          ///< lane past the mesh's meshlet count
    kMeshletResultFrustumCulled = 1u,
    kMeshletResultConeCulled = 2u,
    kMeshletResultPhase1Drawn = 3u,   ///< drawn by the early pass
    kMeshletResultDeferred = 4u,      ///< failed LAST frame's Hi-Z: re-tested by the late pass (transient)
    kMeshletResultPhase2Drawn = 5u,   ///< drawn by the late or the phase-2 pass
    kMeshletResultOccluded = 6u,
    kMeshletResultOversize = 7u,      ///< exceeds kMeshletMaxVertices / kMeshletMaxTriangles: not drawn
};

/// Words of the counts buffer (u32, 64 bytes). Words 0..2 and 3..5 are the
/// VkDrawMeshTasksIndirectCommandEXT of regions 0 and 1.
enum MeshletCount : u32 {
    kMeshletCountTasks0 = 0u,     ///< region 0 task groups (min(requested, capacity))
    kMeshletCountTasks1 = 3u,     ///< region 1 task groups
    kMeshletCountRequested0 = 6u, ///< region 0 groups requested (may exceed the capacity)
    kMeshletCountRequested1 = 7u,
    kMeshletCountOverflow = 8u,   ///< groups dropped for lack of capacity (both regions)
    kMeshletCountSkipped = 9u,    ///< drawn instances whose mesh has no meshlet data (not drawn)
    kMeshletCountOversize = 10u,  ///< meshlets above the pipeline's output limits (not drawn)
    kMeshletCountWords = 16u,
};

/// One task workgroup's work: kMeshletTaskGroup meshlets of one instance starting at firstMeshlet.
struct MeshletGroup {
    u32 instance = 0;
    u32 firstMeshlet = 0;
};
static_assert(sizeof(MeshletGroup) == 8u, "MeshletGroup layout");

/// Per-frame constants (BDA, push constant `constants`): a copy of the culler's CullConstants (same
/// view, planes, Hi-Z handles and flags as the instance cull) plus the meshlet path's own words.
struct MeshletConstants {
    culling::CullConstants cull{};
    f32 camera[3] = {0.f, 0.f, 0.f}; ///< world-space eye (cone test)
    u32 flags = 0;                   ///< MeshletFlag
    u32 groupsBuffer = 0;            ///< bindless storage-buffer handles
    u32 masksBuffer = 0;
    u32 countsBuffer = 0;
    u32 resultsBuffer = 0;           ///< 0 without kMeshletWriteResults
    u32 groupCapacity = 0;           ///< records per region (<= maxTaskWorkGroupCount[0])
    u32 scene = 0;                   ///< GpuScene::headerHandle()
    u32 reserved[2] = {0u, 0u};
};
static_assert(sizeof(MeshletConstants) == 400u, "MeshletConstants layout (meshlet_common.glsl / .slang)");
static_assert(offsetof(MeshletConstants, camera) == 352u && offsetof(MeshletConstants, groupsBuffer) == 368u &&
                  offsetof(MeshletConstants, groupCapacity) == 384u,
              "MeshletConstants offsets (meshlet_common.glsl / .slang)");

/// Push constants of the task / mesh / fragment pipeline: 96 bytes. The first 80 bytes are
/// visbuffer::VisRasterPush (the WP-1.4 fragment shaders read target64 / width from it).
struct MeshletDrawPush {
    f32 viewProj[16] = {}; ///< column-major, == VisBuffer::beginFrame's (the vertex transform)
    u32 scene = 0;
    u32 target64 = 0;      ///< VisBuffer::target64Handle() (Atomic64)
    u32 width = 0;
    u32 height = 0;
    u64 constants = 0;     ///< device address of this frame's MeshletConstants
    u32 mode = kMeshletModeEarly;
    u32 pad = 0;
};
static_assert(sizeof(MeshletDrawPush) == 96u && offsetof(MeshletDrawPush, scene) == 64u &&
                  offsetof(MeshletDrawPush, constants) == 80u,
              "MeshletDrawPush layout");

/// Push constants of meshlet_expand: 16 bytes.
struct MeshletExpandPush {
    u64 constants = 0;
    u32 phase = 1u;    ///< 1 or 2 (culler phase / groups region + 1)
    u32 maxDraws = 0;  ///< culler draw slots per phase (dispatch covers them)
};
static_assert(sizeof(MeshletExpandPush) == 16u, "MeshletExpandPush layout");

} // namespace fuse::renderer::meshlet
