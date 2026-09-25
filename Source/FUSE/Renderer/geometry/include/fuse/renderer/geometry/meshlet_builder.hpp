#pragma once

// WP-1.2 offline meshlet builder (renderer plan Phase 1: "meshlet data built offline with
// meshoptimizer, even before mesh shaders are enabled"; Phase 5 consumes the same data).
//
// Pipeline (deterministic: same input -> same bytes, on every CPU backend and worker count):
//   1. validate the source (index range, finite attributes); fill missing normals (area-weighted
//      face normals) and tangents (UV-derived, Gram-Schmidt; arbitrary basis when UVs degenerate);
//   2. per submesh: meshopt_optimizeVertexCache, then meshopt_buildMeshlets (<= 64 v / 124 t by
//      default, cone weight 0.25) and meshopt_optimizeMeshlet per meshlet;
//   3. meshopt_optimizeVertexFetchRemap over the meshlet-ordered index stream: vertices are
//      renumbered in first-use order (unused source vertices are dropped; VSRC keeps the mapping);
//   4. quantise positions (QuantParams), oct-encode normals / tangents, half UVs
//      (kernel "geometry_vertex_encode"), then decode them again ("geometry_vertex_decode");
//   5. meshopt_computeMeshletBounds on the *decoded* positions, then "geometry_meshlet_bounds"
//      adds the AABB and grows the sphere to contain every decoded vertex.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/renderer/geometry/meshlet_cull_kernel.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::geometry {

struct MeshletSourceSubmesh {
    u32 index_offset = 0;
    u32 index_count = 0;
    u32 material_index = 0;
};

/// Borrowed source geometry (e.g. a decoded FMSH v1 `CookedMesh`). Optional streams may be null.
struct MeshletSource {
    const f32* positions = nullptr; ///< xyz per vertex (required)
    const f32* normals = nullptr;   ///< xyz per vertex (optional; zero-length entries are regenerated)
    const f32* uvs = nullptr;       ///< uv per vertex (optional)
    const f32* tangents = nullptr;  ///< xyzw per vertex (optional; w sign = handedness)
    u32 vertex_count = 0;
    const u32* indices = nullptr; ///< triangle list
    u32 index_count = 0;
    std::vector<MeshletSourceSubmesh> submeshes; ///< empty: one submesh over every index, material 0
};

struct MeshletBuildOptions {
    u32 max_vertices = kMeshletMaxVertices;
    u32 max_triangles = kMeshletMaxTriangles; ///< multiple of 4 (meshoptimizer requirement)
    f32 cone_weight = 0.25f;                  ///< meshoptimizer: 0 = locality only, 1 = cone quality
    u64 source_hash = 0;                      ///< stored in the header (FNV-1a of the source .fusemesh)
    bool keep_source_vertex_map = true;       ///< write VSRC
    kernel::Backend backend = kernel::Backend::CpuParallel;
};

/// Decoded vertex streams (what a GPU decoder reconstructs; see vertex_codec_kernel.hpp).
struct DecodedVertices {
    std::vector<f32> positions; ///< xyz
    std::vector<f32> normals;   ///< xyz
    std::vector<f32> tangents;  ///< xyzw
    std::vector<f32> uvs;       ///< uv
};

/// Build the meshlet mesh. Returns false (with `error`) on invalid source data or options.
bool build_meshlets(const MeshletSource& source, const MeshletBuildOptions& options, MeshletMesh& out,
                    std::string* error = nullptr);

/// Quantisation parameters for positions spanning [min, max] per axis (see QuantParams).
[[nodiscard]] QuantParams compute_quant_params(const f32 min[3], const f32 max[3]);

/// Run "geometry_vertex_decode" over every vertex of `mesh`.
void decode_vertices(const MeshletMesh& mesh, DecodedVertices& out,
                     kernel::Backend backend = kernel::Backend::CpuParallel);

/// Run "geometry_meshlet_cull" (CPU reference) over every meshlet; one result word per meshlet
/// (cull_kernel::kVisible or kCulled* bits).
void cull_meshlets(const MeshletMesh& mesh, const cull_kernel::CullView& view, std::vector<u32>& out,
                   kernel::Backend backend = kernel::Backend::CpuParallel);

} // namespace fuse::renderer::geometry
