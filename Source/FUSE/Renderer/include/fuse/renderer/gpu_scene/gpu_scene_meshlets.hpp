#pragma once

// WP-1.1 GPU scene: WP-1.2 meshlet meshes as GPU geometry buffers.
//
// One device buffer per mesh holds every stream of the cooked `.fusemeshlet` layout, each section
// 16-byte aligned, in this order:
//
//   meshlets          GpuMeshlet x meshlet_count    (== the MSHL chunk element, byte for byte)
//   submeshes         GpuSubmesh x submesh_count    (== SUBM)
//   meshletVertices   u32 x meshlet_vertex_count    (== MVRT)
//   meshletTriangles  u32 x triangle_count          (== MTRI, i0 | i1 << 8 | i2 << 16)
//   positions         u16 x 4 x vertex_count        (== VPOS, quantised; see GpuMesh::quant*)
//   normals           u32 x vertex_count            (== VNRM, oct snorm16x2)
//   tangents          u32 x vertex_count            (== VTAN)
//   uvs               u32 x vertex_count            (== VUV0, half2)
//
// GpuMesh stores base address + section offset per stream, so shaders never see the file layout.

#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::geometry {
struct MeshletMesh;
struct MeshletRecord;
} // namespace fuse::renderer::geometry

namespace fuse::renderer::gpu_scene {

/// Byte offsets of each stream inside a mesh geometry buffer.
struct MeshletGeometryLayout {
    u64 meshlets = 0;
    u64 submeshes = 0;
    u64 meshletVertices = 0;
    u64 meshletTriangles = 0;
    u64 positions = 0;
    u64 normals = 0;
    u64 tangents = 0;
    u64 uvs = 0;
    u64 totalBytes = 0;
};

/// MSHL-identical GPU record of one meshlet.
GpuMeshlet packGpuMeshlet(const geometry::MeshletRecord& record);

/// Lays the streams of `mesh` out into `blob` (resized, zero padding between sections).
MeshletGeometryLayout packMeshletGeometry(const geometry::MeshletMesh& mesh, std::vector<u8>& blob);

/// Appends the mesh's triangle list in the scene index layout (gpu_scene_types.hpp, GpuMesh "Index
/// layout"): for every MTRI entry t in order, the three mesh-local vertex indices
/// MVRT[meshlet.vertex_offset + micro-index]. Returns the number of indices appended (3 x triangles).
u32 appendMeshletIndices(const geometry::MeshletMesh& mesh, std::vector<u32>& indices);

/// GpuMesh for `mesh` whose geometry buffer starts at `baseAddress` (0: addresses stay 0, e.g. the
/// CPU-only mode). Bounds: exact over the decoded positions (AABB centre, farthest vertex, radius
/// rounded up to f32), i.e. the sphere of what the GPU rasterises.
GpuMesh makeGpuMesh(const geometry::MeshletMesh& mesh, const MeshletGeometryLayout& layout, u64 baseAddress,
                    u32 geometryHandle);

} // namespace fuse::renderer::gpu_scene
