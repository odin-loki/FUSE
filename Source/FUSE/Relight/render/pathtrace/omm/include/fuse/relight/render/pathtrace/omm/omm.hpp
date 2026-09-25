// FUSE Relight RL-5.6: opacity micromaps (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.1 "AS build/refit (OMM on HW)", §5.7
// "Opacity micromaps. VK_EXT_opacity_micromap when present ... HW-only with an any-hit fallback").
//
// FUSE's own implementation from the public Vulkan specification of VK_EXT_opacity_micromap (the micromap data layout,
// the special indices, the 2- / 4-state lookup tables of "Ray Opacity Micromap" in the ray traversal chapter) and the
// prose definition of its micro-triangle ordering: the base triangle is split recursively into four, visited "into the
// sub-triangle nearest vertex 0, then the middle triangle with ordering flipped, then nearest vertex 1, then nearest
// vertex 2 with ordering flipped". The NVIDIA OMM SDK is not used (its licence is proprietary).
//
//   index      ommIndexFromBarycentrics (hit barycentrics -> micro-triangle index: quantise to the level's lattice,
//              then descend the recursive curve in exact integer arithmetic) and its inverse ommMicroTriangle;
//   build      ommBuild: per triangle and subdivision level, every micro-triangle classified against the legacy alpha
//              test CONSERVATIVELY (the bilinear texture footprint's min / max texel over the micro-triangle's UV
//              bounds x the vertex-alpha range x the material alpha): Opaque / Transparent only when every point
//              passes / fails; otherwise Unknown (4-state: unknown-opaque / unknown-transparent by the centroid, so
//              Force2State gives the 2-state answer; 2-state: the centroid decides - lossy). Uniform triangles use the
//              special indices; identical blocks are shared. Output: the VkMicromapBuildInfoEXT inputs (data,
//              VkMicromapTriangleEXT array, usage counts) and the per-geometry-triangle index buffer;
//   traverse   ommLookup: the spec's lookup (Ignored / Opaque / Non-opaque), and ommResolveHit: a non-opaque candidate
//              runs the any-hit alpha test. With the 4-state format this equals the plain alpha-test path bit for bit
//              (tests: rl_omm_*), while most candidates skip the any-hit test.
//   device     ommQueryDevice (omm_vk.cpp): VK_EXT_opacity_micromap support and limits; ommSelectPath picks the HW path
//              or the any-hit fallback (Lavapipe: fallback - it does not expose the extension).
#pragma once

#include <fuse/types.hpp>

#include <span>
#include <vector>

namespace fuse::renderer {
class VulkanDevice;
}

namespace fuse::relight::render::pathtrace::omm {

/// VkOpacityMicromapFormatEXT.
enum class OmmFormat : u16 { TwoState = 1, FourState = 2 };

/// VkOpacityMicromapSpecialIndexEXT: -(state + 1).
enum OmmState : u8 {
    kOmmTransparent = 0,
    kOmmOpaque = 1,
    kOmmUnknownTransparent = 2,
    kOmmUnknownOpaque = 3,
};
inline constexpr i32 kOmmSpecialFullyTransparent = -1;
inline constexpr i32 kOmmSpecialFullyOpaque = -2;
inline constexpr i32 kOmmSpecialFullyUnknownTransparent = -3;
inline constexpr i32 kOmmSpecialFullyUnknownOpaque = -4;
inline constexpr u32 kOmmMaxLevel = 12u;

inline constexpr u32 ommMicroTriangleCount(u32 level) { return 1u << (2u * level); }

/// Micro-triangle index of barycentrics (u, v) (weights of vertices 1 and 2) at `level`.
u32 ommIndexFromBarycentrics(float u, float v, u32 level);
/// Lattice cell of micro-triangle `index`: (iu, iv) and whether it is the upper (flipped) triangle of the cell.
void ommCellOfIndex(u32 index, u32 level, u32& iu, u32& iv, bool& upper);
/// The three (u, v) vertices of micro-triangle `index` (in curve orientation).
void ommMicroTriangle(u32 index, u32 level, float uv[3][2]);

/// Legacy alpha source of a geometry (Remix / RL-5.1 semantics: alpha = texture alpha x vertex alpha x base alpha,
/// alpha test `alpha <compare> reference`, VkCompareOp numbering).
struct OmmAlphaTexture {
    u32 width = 0;
    u32 height = 0;
    const float* alpha = nullptr; ///< row-major, top row first; bilinear, repeat, texel centres (i + 0.5) / size
};
struct OmmAlphaTest {
    u32 compare = 4u; ///< VkCompareOp (4 = GREATER)
    float reference = 0.5f;
    float baseAlpha = 1.f;
};

/// Bilinear sample of the alpha texture (the path tracer's sampler semantics, float).
float ommSampleAlpha(const OmmAlphaTexture& tex, float u, float v);
bool ommAlphaPasses(u32 compare, float alpha, float reference);

/// One geometry triangle.
struct OmmTriangle {
    float uv[3][2] = {};
    float vertexAlpha[3] = {1.f, 1.f, 1.f};
};

/// The any-hit alpha test of a hit (barycentrics u, v): alpha = tex(uv(b)) x vertexAlpha(b) x base.
bool ommAlphaTestHit(const OmmTriangle& t, const OmmAlphaTexture& tex, const OmmAlphaTest& test, float u, float v);

struct OmmBuildDesc {
    u32 level = 4;
    OmmFormat format = OmmFormat::FourState;
    OmmAlphaTest test{};
    const OmmAlphaTexture* texture = nullptr; ///< null: alpha = vertex x base only
    bool useSpecialIndices = true;
    bool deduplicate = true;
};

/// VkMicromapTriangleEXT.
struct OmmTriangleRecord {
    u32 dataOffset = 0;
    u16 subdivisionLevel = 0;
    u16 format = 0;
};
static_assert(sizeof(OmmTriangleRecord) == 8u, "VkMicromapTriangleEXT layout");

/// VkMicromapUsageEXT.
struct OmmUsage {
    u32 count = 0;
    u32 subdivisionLevel = 0;
    u32 format = 0;
};

struct OmmStats {
    u64 microOpaque = 0;
    u64 microTransparent = 0;
    u64 microUnknown = 0;
    u32 specialTriangles = 0;
    u32 uniqueBlocks = 0;
    u32 sharedBlocks = 0;
};

struct OmmBuildResult {
    std::vector<u8> data;                   ///< micromap data (VkMicromapBuildInfoEXT::data)
    std::vector<OmmTriangleRecord> records; ///< VkMicromapBuildInfoEXT::triangleArray
    std::vector<OmmUsage> usage;            ///< VkMicromapBuildInfoEXT::pUsageCounts
    std::vector<i32> indices;               ///< per geometry triangle: record index or special index (indexBuffer)
    OmmStats stats{};
};

bool ommBuild(std::span<const OmmTriangle> triangles, const OmmBuildDesc& desc, OmmBuildResult& out);

/// Lookup result (spec tables).
enum class OmmHit : u8 { Ignored = 0, Opaque = 1, NonOpaque = 2 };
OmmHit ommLookup(const OmmBuildResult& omm, u32 triangle, float u, float v, bool force2State = false);
/// The candidate's fate with the any-hit fallback for non-opaque results (true = hit accepted); `anyHit` counts
/// alpha-test invocations.
bool ommResolveHit(const OmmBuildResult& omm, u32 triangle, float u, float v, const OmmTriangle& t,
                   const OmmAlphaTexture* tex, const OmmAlphaTest& test, u32* anyHit = nullptr);

// ---- device capability (omm_vk.cpp) --------------------------------------------------------------------------------

struct OmmDeviceCaps {
    bool extension = false;       ///< VK_EXT_opacity_micromap enumerated
    bool micromap = false;        ///< VkPhysicalDeviceOpacityMicromapFeaturesEXT::micromap
    u32 max2StateLevel = 0;
    u32 max4StateLevel = 0;
};
enum class OmmPath : u8 {
    Hardware = 0,        ///< micromaps attached to the BLAS (VK_EXT_opacity_micromap)
    AnyHitFallback = 1,  ///< the path tracer's alpha re-trace (RL-5.1), optionally culled by the CPU micromap
};
/// False in the stub backend (caps zeroed).
bool ommQueryDevice(const renderer::VulkanDevice& device, OmmDeviceCaps& caps);
OmmPath ommSelectPath(const OmmDeviceCaps& caps, const OmmBuildDesc& desc);

} // namespace fuse::relight::render::pathtrace::omm
