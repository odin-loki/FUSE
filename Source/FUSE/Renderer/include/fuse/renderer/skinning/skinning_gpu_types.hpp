#pragma once

// GPU skinning in the GPU scene ("skin.lbs"): records shared by the C++ side and the shaders
// (shaders/skinning/skin_common.{glsl,slang}). Keep them in sync; the static_asserts pin the layouts.
// Device-safe (only <fuse/types.hpp>).
//
// Streams (all f32 x 4 per vertex, 16-byte aligned, addressed by BDA):
//   rest   positions (xyz, w = tangent handedness +-1) [0, n), normals (xyz, 0) [n, 2n), weights
//          (SkinVertexWeights, 32 bytes) at byte 32 n
//   out    current skinned positions [0, n), previous skinned positions [n, 2n) (the GpuMesh kMeshFloatPositions
//          layout: the per-instance GpuMesh row's `positions` points here), skinned normals [2n, 3n)
//          (kMeshFloatNormals: the row's `normals` points here)

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::skinning {

/// One vertex's influences. Byte-identical to fuse::animation::SkinningWeights (u32 indices[4], f32 weights[4]),
/// so the Animator's weight arrays upload without repacking.
struct SkinVertexWeights {
    u32 bone[4] = {0u, 0u, 0u, 0u};
    f32 weight[4] = {1.f, 0.f, 0.f, 0.f};
};
static_assert(sizeof(SkinVertexWeights) == 32u, "SkinVertexWeights layout (skin_common.glsl / .slang)");

/// SkinJob::flags.
enum SkinJobFlag : u32 {
    kSkinJobNormals = 1u << 0, ///< the rest stream has normals: skin and write them
};

/// One skinned instance of one frame (48 bytes, std430: FuseSkinJob / SkinJob).
struct SkinJob {
    u64 rest = 0;        ///< BDA of the rest streams (layout above)
    u64 palette = 0;     ///< BDA: this frame's bone palette, column-major 4x4 f32 per bone
    u64 prevPalette = 0; ///< BDA: last frame's palette (== palette on the first frame / after a reset)
    u64 out = 0;         ///< BDA of the output streams (layout above)
    u32 vertexCount = 0;
    u32 boneCount = 0;
    u32 flags = 0; ///< SkinJobFlag
    u32 pad = 0;
};
static_assert(sizeof(SkinJob) == 48u && offsetof(SkinJob, vertexCount) == 32u, "SkinJob layout (skin_common.glsl / .slang)");

/// Push constants of "skin.lbs" (16 bytes).
struct SkinPush {
    u64 jobs = 0;      ///< BDA of SkinJob[jobCount]
    u32 jobCount = 0;  ///< dispatch y
    u32 pad = 0;
};
static_assert(sizeof(SkinPush) == 16u, "SkinPush layout");

/// Threads per workgroup (x): one vertex per thread, the animation kernel's 128 (skinning_kernel::kWorkgroup).
inline constexpr u32 kSkinWorkgroup = 128u;

} // namespace fuse::renderer::skinning
