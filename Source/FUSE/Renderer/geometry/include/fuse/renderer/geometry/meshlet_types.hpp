#pragma once

// WP-1.2 meshlet cook: plain data shared by the offline builder, the cooked format, the vertex codec
// and the CPU reference cull (docs/unification/RENDERER-EXECUTION.md, renderer plan Phase 1/5).
// Device-safe: only <fuse/types.hpp>, no STL, so kernel headers and a future GPU mirror can include it.

#include <fuse/types.hpp>

namespace fuse::renderer::geometry {

using i8 = std::int8_t;

/// Renderer plan Phase 1: meshlets of at most 64 vertices and 124 triangles (NVIDIA's mesh-shader
/// sweet spot; 124 keeps the triangle count a multiple of 4 below 128 as meshoptimizer requires).
inline constexpr u32 kMeshletMaxVertices = 64u;
inline constexpr u32 kMeshletMaxTriangles = 124u;
/// Hard format limits: per-meshlet counts are stored as u8 and micro-indices are 8 bits.
inline constexpr u32 kMeshletFormatMaxVertices = 255u;
inline constexpr u32 kMeshletFormatMaxTriangles = 252u; // largest multiple of 4 that fits in u8

/// Position quantisation (per mesh, per axis). `step[a] == 2^exponent[a]` and
/// `offset[a] == k * step[a]` for an integer k with |k| + 65535 < 2^24, so every decoded value
/// `offset + float(q) * step` (q in 0..65535) is exactly representable in f32: the product is exact
/// (power-of-two scale of a 16-bit integer) and so is the sum. Decoding is therefore bit-exact on the
/// CPU, CUDA and any Vulkan implementation, with or without FMA contraction.
struct QuantParams {
    s32 exponent[3] = {0, 0, 0};
    f32 offset[3] = {0.f, 0.f, 0.f};
    f32 step[3] = {1.f, 1.f, 1.f};
};

/// One meshlet as the builder, the reader and the cull kernels see it (the on-disk record is the
/// same fields, packed; see meshlet_format.hpp). Bounds are computed on *decoded* positions, so they
/// hold for exactly what the GPU rasterises.
struct MeshletRecord {
    u32 vertex_offset = 0;   ///< first entry in MeshletMesh::meshlet_vertices
    u32 triangle_offset = 0; ///< first entry in MeshletMesh::meshlet_triangles
    u32 vertex_count = 0;    ///< <= max_vertices
    u32 triangle_count = 0;  ///< <= max_triangles
    u32 submesh = 0;         ///< index into MeshletMesh::submeshes
    f32 center[3] = {0.f, 0.f, 0.f}; ///< bounding sphere (contains every decoded vertex)
    f32 radius = 0.f;
    f32 cone_apex[3] = {0.f, 0.f, 0.f}; ///< backface cone (meshoptimizer convention)
    f32 cone_axis[3] = {0.f, 0.f, 0.f};
    f32 cone_cutoff = 1.f;              ///< cos(half angle); >= 1 means "never cone-cull"
    i8 cone_axis_s8[3] = {0, 0, 0};    ///< snorm8 axis (x / 127), conservative with cone_cutoff_s8
    i8 cone_cutoff_s8 = 127;
    f32 aabb_min[3] = {0.f, 0.f, 0.f};  ///< box around the decoded vertices
    f32 aabb_max[3] = {0.f, 0.f, 0.f};
};
static_assert(sizeof(MeshletRecord) == 92u, "MeshletRecord must stay padding-free (bitwise compares)");

struct SubmeshRange {
    u32 meshlet_offset = 0;
    u32 meshlet_count = 0;
    u32 material_index = 0;
    u32 triangle_count = 0; ///< source triangles of this submesh (== sum of its meshlets' triangles)
};

/// Micro-index packing in MeshletMesh::meshlet_triangles: one u32 per triangle,
/// `i0 | i1 << 8 | i2 << 16`, bits 24..31 zero. Indices are meshlet-local (< vertex_count).
FUSE_HOST_DEVICE constexpr u32 pack_triangle(u32 i0, u32 i1, u32 i2) { return i0 | (i1 << 8) | (i2 << 16); }
FUSE_HOST_DEVICE constexpr u32 triangle_index(u32 packed, u32 corner) { return (packed >> (corner * 8u)) & 0xFFu; }

/// VPOS.w bit 0: the tangent's bitangent sign is negative (tangent.w == -1).
inline constexpr u16 kVposTangentNegative = 1u;

} // namespace fuse::renderer::geometry
