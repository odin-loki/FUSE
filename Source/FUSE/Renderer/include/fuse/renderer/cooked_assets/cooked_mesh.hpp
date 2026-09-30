#pragma once

// E06 (AP-RT-COOKED): FMSH (`.fusemesh`, fuse/asset/cooked_mesh.hpp) -> the renderer's WP-1.2 meshlet mesh
// (geometry::MeshletMesh, what GpuScene::addMeshletMesh uploads) and WP-5.2 cluster DAG (geometry::dag::ClusterDag,
// what virtual-geometry streaming consumes).
//
// Two paths:
//   * adopt  (FMSH v2 with a meshlet table, W0.2): the cook's meshlets are used as they are. Vertices are renumbered
//            in first-use order over the meshlet triangles (the order the WP-1.2 builder's
//            meshopt_optimizeVertexFetchRemap produces; VSRC keeps the FMSH index), quantised over the used vertices
//            (compute_quant_params), oct-encoded (normals, tangents + sign as supplied, like the builder) and half
//            UVs through the "geometry_vertex_encode" kernel; each meshlet's AABB is recomputed and its sphere grown to contain every
//            *decoded* vertex ("geometry_meshlet_bounds"), so culling stays conservative. The cone is the cook's
//            (built on the same quantisation). A cluster DAG section is adopted too (its FMSH vertex indices mapped
//            through the same renumbering) and checked with validate_cluster_dag. For an FMSH whose meshlets the
//            cook built from the same streams, the result equals geometry::build_meshlets' output bit for bit.
//   * build  (FMSH v1, or v2 without meshlets / tangents / usable normals): geometry::build_meshlets over the
//            FMSH streams (per submesh, meshoptimizer), as MeshRegistry::registerFuseMesh does for v1 sidecars.
// Both results pass validate_meshlet_mesh.

#include <fuse/asset/cooked_mesh.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/geometry/dag/cluster_dag.hpp>
#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::renderer::cooked_assets {

enum class CookedMeshPath : u8 {
    Adopted = 0, ///< the FMSH v2 meshlet table (+ DAG) used directly
    Built,       ///< meshlets built from the FMSH streams
};

[[nodiscard]] const char* cooked_mesh_path_name(CookedMeshPath path);

struct CookedMeshletResult {
    geometry::MeshletMesh mesh;
    geometry::dag::ClusterDag dag; ///< empty unless the FMSH carried a DAG and it was adopted
    bool hasDag = false;
    CookedMeshPath path = CookedMeshPath::Built;
    f32 boundsMin[3] = {0.f, 0.f, 0.f}; ///< object-space AABB of the source positions
    f32 boundsMax[3] = {0.f, 0.f, 0.f};
    std::string note; ///< why the build path was taken (empty when adopted)
};

/// Converts `mesh` (see the file comment). False (with `error`) on invalid geometry.
bool cooked_mesh_to_meshlets(const asset::CookedMesh& mesh, CookedMeshletResult& out, std::string* error = nullptr,
                             kernel::Backend backend = kernel::Backend::CpuParallel);

} // namespace fuse::renderer::cooked_assets
