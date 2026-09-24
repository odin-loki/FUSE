#pragma once

// WP-5.2 cluster DAG + LOD (docs/unification/RENDERER-EXECUTION.md; renderer plan Phase 5
// "cluster hierarchy (DAG) built with meshoptimizer's simplification, with LOD selected by a
// screen-space error metric").
//
// Builder (build_cluster_dag), per submesh, on the *decoded* (quantised) positions of a WP-1.2
// MeshletMesh, so every LOD cluster references the same cooked vertex streams and shared
// boundaries are vertex-identical by construction:
//   level 0   the WP-1.2 meshlets (MSHL) are the leaf clusters;
//   repeat    meshopt_partitionClusters groups the pending clusters (~group_size per group,
//             spatial + shared-vertex affinity); vertices (by exact position) used by two groups of
//             the level, by another submesh, or by a cluster of an earlier terminal group are locked;
//             each group is merged and simplified (meshopt_simplifyWithAttributes, Sparse +
//             ErrorAbsolute, target simplify_ratio of its triangles); if the result keeps more than
//             stuck_ratio of the triangles the group is terminal, otherwise the result is re-split
//             with meshopt_buildMeshlets (same limits / cone weight as WP-1.2) into child clusters
//             that are pending for the next level;
//   end       the last pending cluster (or all of them at max_levels) forms a terminal group.
// Group bounds: sphere around the members' LOD spheres (meshopt_computeSphereBounds, then grown
// in f64 until it contains every member sphere, times 1 + sphere_slack, rounded up); error =
// max(simplifier error, max member error * (1 + error_slack)) rounded up. Child clusters get
// culling bounds (sphere, cone, AABB) exactly as WP-1.2 meshlets (geometry_meshlet_bounds).
// Deterministic: the output depends only on the input mesh and options (any CPU backend / workers).
//
// Crack-free argument: edge collapses only move unlocked vertices, so every group-boundary edge
// shared with a neighbour (both ends locked) survives into the children unchanged; the children's
// boundary equals the members' boundary except along the source mesh's open border.
//
// Cooked format: FMLT 1.1 = the WP-1.2 `.fusemeshlet` 1.0 layout (meshlet_format.hpp) plus seven
// chunks, all 16-byte aligned after the 1.0 chunks, little-endian. A 1.0 reader skips them (it
// accepts newer minors and unknown chunks) and sees the full-detail mesh; the DAG reader below
// in turn skips chunks it does not know.
//   DAGH  32 x 1        u32 leaf_cluster_count (== MSHL count), lod_cluster_count, group_count,
//                       level_count, lod_meshlet_vertex_count, lod_triangle_count, reserved[2] (0)
//   DMSH  96 x lod      LOD cluster records, byte-identical layout to MSHL; vertex_offset /
//                       triangle_offset index DMVR / DMTR (running sums from 0)
//   DMVR   4 x n        u32 mesh vertex index (into VPOS/VNRM/VTAN/VUV0)
//   DMTR   4 x n        u32 packed micro-triangle (as MTRI)
//   DGRP  48 x group    DagGroup (f32 center[3], radius, error, u32 member_offset, member_count,
//                       child_offset, child_count, depth, submesh, reserved 0)
//   DGMB   4 x cluster  u32 cluster id; group member lists, concatenated in group order
//   DCLK  48 x cluster  DagClusterLink (self bounds, parent bounds, u32 group, u32 refined)
// Reader rules on top of the 1.0 ones (validate_cluster_dag): all seven chunks present (none of
// them: MissingChunk "no DAG"), counts agree with DAGH, header minor >= 1; LOD clusters packed,
// within limits, micro-indices / vertex indices in range, bounds finite; group member ranges tile
// DGMB and every cluster is a member of exactly one group; child ranges tile the LOD ids in group
// order; terminal <=> no children <=> error == kDagErrorTerminal; links equal the group records
// bit for bit; leaves' self = {meshlet sphere, 0}; levels consistent; monotonicity: for every LOD
// cluster, error(group) >= error(producer) and sphere(group) contains sphere(producer).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/geometry/dag/cluster_dag_types.hpp>
#include <fuse/renderer/geometry/dag/dag_cut_kernel.hpp>
#include <fuse/renderer/geometry/meshlet_builder.hpp>
#include <fuse/renderer/geometry/meshlet_format.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::geometry::dag {

/// FMLT minor version written with DAG chunks (the 1.0 layout is unchanged).
inline constexpr u16 kClusterDagFormatVersionMinor = 1u;

struct DagBuildOptions {
    u32 group_size = 16u;          ///< target clusters per group (partitions may be up to 4/3 of it)
    f32 simplify_ratio = 0.5f;     ///< target triangle fraction per simplification
    f32 stuck_ratio = 0.85f;       ///< keep more than this fraction -> terminal group
    u32 max_levels = 48u;          ///< safety cap on DAG depth
    f32 cone_weight = 0.25f;       ///< meshopt_buildMeshlets cone weight for LOD clusters (WP-1.2 value)
    f32 sphere_slack = 1.f / 1024.f; ///< relative LOD-sphere growth per level (float-robust monotone cut)
    f32 error_slack = 1.f / 65536.f; ///< relative error growth per level (float-robust monotone cut)
    kernel::Backend backend = kernel::Backend::CpuParallel;
};

/// The DAG on top of a MeshletMesh (see the file comment for ids and chunks).
struct ClusterDag {
    u32 leaf_cluster_count = 0;
    u32 level_count = 0;                     ///< 1 + highest cluster level (leaves are level 0)
    std::vector<MeshletRecord> lod_clusters; ///< DMSH; cluster id = leaf_cluster_count + index
    std::vector<u32> lod_meshlet_vertices;   ///< DMVR
    std::vector<u32> lod_meshlet_triangles;  ///< DMTR (pack_triangle)
    std::vector<DagGroup> groups;            ///< DGRP
    std::vector<u32> group_members;          ///< DGMB
    std::vector<DagClusterLink> links;       ///< DCLK, one per cluster id

    [[nodiscard]] u32 cluster_count() const { return leaf_cluster_count + static_cast<u32>(lod_clusters.size()); }
};

/// A `.fusemeshlet` with its DAG (FMLT 1.1).
struct ClusterDagMesh {
    MeshletMesh base;
    ClusterDag dag;
};

/// One cluster's record and its meshlet-local vertex / triangle arrays (leaf or LOD).
struct ClusterRef {
    const MeshletRecord* record = nullptr;
    const u32* vertices = nullptr;  ///< record->vertex_count mesh vertex indices
    const u32* triangles = nullptr; ///< record->triangle_count packed triangles
};
[[nodiscard]] ClusterRef cluster_ref(const ClusterDagMesh& mesh, u32 cluster_id);
/// Level of a cluster: 0 for leaves, depth(producer) + 1 otherwise.
[[nodiscard]] u32 cluster_level(const ClusterDag& dag, u32 cluster_id);

/// Build the DAG over `base` (a valid MeshletMesh, e.g. from build_meshlets).
bool build_cluster_dag(const MeshletMesh& base, const DagBuildOptions& options, ClusterDag& out,
                       std::string* error = nullptr);
/// build_meshlets + build_cluster_dag; `out.base.version_minor` is set to kClusterDagFormatVersionMinor.
bool build_cluster_dag_mesh(const MeshletSource& source, const MeshletBuildOptions& meshlet_options,
                            const DagBuildOptions& dag_options, ClusterDagMesh& out, std::string* error = nullptr);

/// Every rule of the file comment (everything but byte layout).
bool validate_cluster_dag(const MeshletMesh& base, const ClusterDag& dag, std::string* error = nullptr);

/// Field-for-field, bit-for-bit equality (base via meshlet_mesh_equal).
bool cluster_dag_mesh_equal(const ClusterDagMesh& a, const ClusterDagMesh& b);

/// Deterministic FMLT 1.1 bytes (minor is max(base.version_minor, 1)). Writes whatever it is given
/// (tests serialise corrupted DAGs); the reader validates.
[[nodiscard]] std::vector<u8> serialize_cluster_dag_mesh(const ClusterDagMesh& mesh);
/// Parse + validate. Error codes as parse_meshlet_mesh; a valid FMLT without DAG chunks fails with
/// MissingChunk (use parse_meshlet_mesh for the full-detail mesh alone).
bool parse_cluster_dag_mesh(const u8* data, usize size, ClusterDagMesh& out, std::string* error = nullptr,
                            MeshletFormatError* code = nullptr);
bool write_cluster_dag_file(const std::string& path, const ClusterDagMesh& mesh, std::string* error = nullptr);
bool load_cluster_dag_file(const std::string& path, ClusterDagMesh& out, std::string* error = nullptr,
                           MeshletFormatError* code = nullptr);

/// Run "geometry_dag_cut" over every cluster: one kInCut / kNotInCut word per cluster id.
void evaluate_dag_cut(const ClusterDag& dag, const cut_kernel::DagView& view, std::vector<u32>& out,
                      kernel::Backend backend = kernel::Backend::CpuParallel);
/// evaluate_dag_cut, compacted to the selected cluster ids in increasing order.
void select_dag_cut(const ClusterDag& dag, const cut_kernel::DagView& view, std::vector<u32>& cluster_ids,
                    kernel::Backend backend = kernel::Backend::CpuParallel);

struct DagLevelStats {
    u32 clusters = 0;
    u64 triangles = 0;
    u32 groups = 0; ///< groups formed at this depth
    u32 terminal_groups = 0;
};
/// Per level: clusters / triangles at that level and groups formed at that depth.
[[nodiscard]] std::vector<DagLevelStats> dag_level_stats(const ClusterDagMesh& mesh);

} // namespace fuse::renderer::geometry::dag
