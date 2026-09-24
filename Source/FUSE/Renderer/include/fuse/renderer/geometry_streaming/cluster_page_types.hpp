#pragma once

// WP-5.3 cluster streaming: device-safe records of the cluster page file (cluster_page_file.hpp) and of
// the page payloads a GPU pool slot holds. Only <fuse/types.hpp> and the WP-1.2 / WP-5.2 plain records,
// no STL, so a GPU mirror (stream_cut.{comp,slang}, a future WP-5.1 page-aware mesh path) can follow it.
//
// Page payload (what one pool slot holds; little-endian, every section 16-byte aligned, offsets in
// bytes from the payload start):
//   PagePayloadHeader                    48 bytes
//   StreamCluster[cluster_count]         96 bytes each: the cluster's MeshletRecord (WP-1.2 layout, bounds
//                                        and cone as cooked) with vertex_offset / triangle_offset rewritten
//                                        to index this page's ref / triangle arrays, + the cluster id
//   u32 refs[ref_count]                  page-local vertex index per cluster vertex (record.vertex_offset)
//   u32 triangles[triangle_count]        pack_triangle micro-indices, cluster-local (record.triangle_offset)
//   u16 positions[4 * vertex_count]      the page's vertices' VPOS entries (quantised; decode with the
//                                        mesh QuantParams: offset + q * step, bit-exact everywhere)
//   u32 source_vertices[vertex_count]    mesh vertex index of each page vertex (VNRM / VTAN / VUV0 fetch)
// Every cluster of every group stored in the page is in it (group by group, members in DGMB order), so a
// resident page is enough to draw any of its clusters: vertex data is duplicated across pages on shared
// borders, bit-identical (same quantised VPOS), so cuts stay watertight across page boundaries.

#include <fuse/renderer/geometry/meshlet_types.hpp>

namespace fuse::renderer::geometry_streaming {

using geometry::MeshletRecord;

/// "No page" (leaf clusters' producer page, unassigned pool slots).
inline constexpr u32 kPageNone = 0xFFFFFFFFu;
/// Payload header magic: "FCPP".
inline constexpr u32 kPagePayloadMagic = 0x50504346u;
/// ClusterPageEntry::flags: the page holds only terminal groups (the always-resident coarse LOD).
inline constexpr u32 kPageFlagCoarse = 1u;

struct PagePayloadHeader {
    u32 magic = kPagePayloadMagic;
    u32 page = 0;
    u32 cluster_count = 0;
    u32 vertex_count = 0;
    u32 ref_count = 0;
    u32 triangle_count = 0;
    u32 clusters_offset = 0;
    u32 refs_offset = 0;
    u32 triangles_offset = 0;
    u32 positions_offset = 0;
    u32 source_offset = 0;
    u32 total_bytes = 0;
};
static_assert(sizeof(PagePayloadHeader) == 48u, "PagePayloadHeader layout");

struct StreamCluster {
    MeshletRecord record{}; ///< vertex_offset -> refs, triangle_offset -> triangles (page-local)
    u32 cluster = 0;        ///< DAG cluster id
};
static_assert(sizeof(StreamCluster) == 96u, "StreamCluster layout");

/// Page table entry (file and memory). Pages are in topological order: every dependency of page p is
/// a page < p; pages [0, coarse_page_count) are the coarse pages and have no dependencies.
struct ClusterPageEntry {
    u64 payload_offset = 0; ///< into ClusterPageFile::payload (16-aligned)
    u32 payload_bytes = 0;  ///< <= page_bytes
    u32 group_offset = 0;   ///< into ClusterPageFile::page_groups
    u32 group_count = 0;
    u32 cluster_count = 0;
    u32 dep_offset = 0;     ///< into ClusterPageFile::page_deps
    u32 dep_count = 0;
    u32 flags = 0;          ///< kPageFlagCoarse
    u32 min_depth = 0;      ///< DAG depth range of the page's groups
    u32 max_depth = 0;
    u32 reserved = 0;
};
static_assert(sizeof(ClusterPageEntry) == 48u, "ClusterPageEntry layout");

/// Per-cluster streaming record the cut kernel reads next to the DAG link (DCLK).
struct StreamClusterInfo {
    u32 member_page = kPageNone;   ///< page of the group the cluster is a member of (holds its geometry)
    u32 producer_page = kPageNone; ///< page of the group that produced it (holds its finer replacement); leaves: kPageNone
};
static_assert(sizeof(StreamClusterInfo) == 8u, "StreamClusterInfo layout");

} // namespace fuse::renderer::geometry_streaming
