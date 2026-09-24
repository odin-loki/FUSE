#pragma once

// WP-5.2 cluster DAG + LOD: plain records shared by the offline DAG builder, the cooked format
// (FMLT 1.1 DAG chunks, cluster_dag.hpp) and the single-source LOD cut kernel (dag_cut_kernel.hpp).
// Device-safe: only <fuse/types.hpp>, no STL, so a GPU mirror (WP-5.1 / WP-5.3) can include it.
//
// Cluster ids: one index space per mesh. Ids [0, leaf_cluster_count) are the WP-1.2 meshlets
// (MSHL, level 0, the full-detail surface); ids [leaf_cluster_count, cluster_count) are the
// simplified LOD clusters (DMSH), stored in the order the groups produced them.
//
// Error metric (Nanite / meshoptimizer clusterlod model, with a strict monotone bound):
//   * A *group* is a set of clusters of one DAG level that are merged, simplified (group boundary
//     locked) and re-split into a coarser set of clusters, its *children*. Its DagLodBounds is the
//     LOD sphere + object-space error of that simplified result.
//   * error(group) >= error(member's producer group) and sphere(group) contains the producer's
//     sphere (with a relative slack, see DagBuildOptions), so the projected error below never
//     decreases from a group to the group that consumes its output: the cut is consistent.
//   * A group the simplifier could not reduce ("stuck") or the last group of a submesh is
//     *terminal*: error == kDagErrorTerminal, no children; its members are never replaced.

#include <fuse/types.hpp>

namespace fuse::renderer::geometry::dag {

/// Error of a terminal group (never coarse enough): FLT_MAX, stored bit-exactly.
inline constexpr f32 kDagErrorTerminal = 3.40282346638528859812e+38f;
/// DagClusterLink::refined of a leaf (level-0) cluster.
inline constexpr u32 kDagNoGroup = 0xFFFFFFFFu;

/// LOD sphere + error (object space). 20 bytes, padding-free.
struct DagLodBounds {
    f32 center[3] = {0.f, 0.f, 0.f};
    f32 radius = 0.f;
    f32 error = 0.f; ///< >= 0, finite, or exactly kDagErrorTerminal
};
static_assert(sizeof(DagLodBounds) == 20u, "DagLodBounds must stay padding-free");

/// Per-cluster DAG link (DCLK chunk), one per cluster id. The two bounds are copies of group
/// records so the cut kernel needs no indirection:
///   self   = bounds of the group that produced this cluster (leaf: meshlet sphere, error 0)
///   parent = bounds of the group this cluster is a member of (terminal: error kDagErrorTerminal)
/// A cluster is drawn when `self` is acceptable and `parent` is not (dag_cut_kernel.hpp).
struct DagClusterLink {
    DagLodBounds self{};
    DagLodBounds parent{};
    u32 group = 0;            ///< group this cluster is a member of (every cluster is in exactly one)
    u32 refined = kDagNoGroup; ///< group that produced it; kDagNoGroup for leaves
};
static_assert(sizeof(DagClusterLink) == 48u, "DagClusterLink must stay padding-free (bitwise compares)");

/// Group record (DGRP chunk).
struct DagGroup {
    DagLodBounds bounds{};   ///< LOD bounds of the simplified result (terminal: error kDagErrorTerminal)
    u32 member_offset = 0;   ///< into ClusterDag::group_members
    u32 member_count = 0;    ///< >= 1
    u32 child_offset = 0;    ///< first produced cluster id (>= leaf count); 0 when child_count == 0
    u32 child_count = 0;     ///< 0 <=> terminal
    u32 depth = 0;           ///< DAG level the group was formed at (>= every member's level)
    u32 submesh = 0;         ///< all members and children belong to this submesh
    u32 reserved = 0;
};
static_assert(sizeof(DagGroup) == 48u, "DagGroup must stay padding-free (bitwise compares)");

} // namespace fuse::renderer::geometry::dag
