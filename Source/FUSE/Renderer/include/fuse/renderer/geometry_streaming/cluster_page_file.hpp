#pragma once

// WP-5.3 cluster page file: the streaming layout of a WP-5.2 cluster DAG (docs/unification/
// RENDERER-EXECUTION.md, renderer plan Phase 5 "cluster page streaming and residency").
//
// Unit of streaming = a *page*: whole DAG groups' members (every cluster that is a member of the group,
// with its triangles and the VPOS of its vertices, cluster_page_types.hpp) packed up to `page_bytes`.
// Drawing cluster c needs the page of its group (StreamClusterInfo::member_page); refining past c (drawing
// the members of the group that produced c) needs that group's page (producer_page).
//
// Layout (build_cluster_pages):
//   1. terminal groups first (the coarse LOD: never replaced), then the others by DAG depth descending
//      (coarse to fine); inside each class by submesh, then by the Morton code of the group's LOD-sphere
//      centre in the mesh bounds (spatially coherent pages);
//   2. greedy packing in that order, a page never mixes terminal and non-terminal groups; a group larger
//      than page_bytes fails the build (raise page_bytes);
//   3. dependencies: page P depends on page Q != P when a group in P produced a cluster that is a member
//      of a group in Q. Such a group has a strictly larger depth, so Q < P: page order is a topological
//      order (the core_logic residency requires it), coarse pages have no dependencies.
// Why dependencies make residency-limited cuts watertight: see stream_cut_kernel.hpp.
//
// File (".fusepages", FCPG 1.0, little-endian, deterministic bytes):
//   header (128 B): u32 magic "FCPG", u16 major 1, u16 minor 0, u32 header_bytes 128, u32 page_count,
//                   u32 group_count, u32 cluster_count, u32 leaf_cluster_count, u32 coarse_page_count,
//                   u32 dep_count, u32 page_bytes, u64 links_hash (FNV-1a 64 of the DCLK links, binds
//                   the file to its DAG), QuantParams (s32 exponent[3], f32 offset[3], f32 step[3]),
//                   u32 pad (0), u64 table_offset (128), u64 payload_offset, u64 payload_bytes,
//                   u32 reserved[4] (0)
//   pages       ClusterPageEntry[page_count] (48 B)            at table_offset
//   page_groups u32[group_count], page_deps u32[dep_count], group_page u32[group_count]
//   payload     page payloads (16-aligned), at payload_offset (16-aligned)
//   trailer     u64 FNV-1a 64 of every byte before it
// parse_cluster_page_file checks the layout and every structural rule (ranges tile, topological
// dependencies, coarse pages first and dependency-free, group_page consistent, payload headers and
// sections in range); validate_cluster_page_file additionally checks the file against its DAG mesh
// (every cluster stored exactly once in its group's page with the cooked record, triangles and vertex
// positions, dependencies exactly as defined above).

#include <fuse/renderer/geometry/dag/cluster_dag.hpp>
#include <fuse/renderer/geometry_streaming/cluster_page_types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::geometry_streaming {

inline constexpr u32 kClusterPageMagic = 0x47504346u; ///< "FCPG"
inline constexpr u16 kClusterPageVersionMajor = 1u;
inline constexpr u16 kClusterPageVersionMinor = 0u;
inline constexpr u32 kClusterPageHeaderBytes = 128u;
inline constexpr u32 kDefaultPageBytes = 64u * 1024u;

struct PageBuildOptions {
    u32 page_bytes = kDefaultPageBytes; ///< payload capacity of a page (= one pool slot), multiple of 16, >= 1024
};

struct ClusterPageFile {
    u32 leaf_cluster_count = 0;
    u32 cluster_count = 0;
    u32 group_count = 0;
    u32 page_bytes = kDefaultPageBytes;
    u32 coarse_page_count = 0;
    u64 links_hash = 0;
    geometry::QuantParams quant{};
    std::vector<ClusterPageEntry> pages;
    std::vector<u32> page_groups; ///< group ids, page by page
    std::vector<u32> page_deps;   ///< dependency page ids, page by page, ascending
    std::vector<u32> group_page;  ///< page of every group
    std::vector<u8> payload;      ///< page payloads

    [[nodiscard]] u32 page_count() const { return static_cast<u32>(pages.size()); }
    [[nodiscard]] const u8* page_payload(u32 page) const { return payload.data() + pages[page].payload_offset; }
};

/// FNV-1a 64 of the DAG links (the value ClusterPageFile::links_hash stores).
[[nodiscard]] u64 cluster_links_hash(const geometry::dag::ClusterDag& dag);

/// Page layout of `mesh` (a valid DAG mesh, e.g. from build_cluster_dag_mesh / load_cluster_dag_file).
bool build_cluster_pages(const geometry::dag::ClusterDagMesh& mesh, const PageBuildOptions& options, ClusterPageFile& out,
                         std::string* error = nullptr);

/// Structural rules (file comment). `payload` sections are checked too.
bool validate_cluster_page_layout(const ClusterPageFile& file, std::string* error = nullptr);
/// validate_cluster_page_layout + consistency with the DAG mesh the file was built from.
bool validate_cluster_page_file(const ClusterPageFile& file, const geometry::dag::ClusterDagMesh& mesh,
                                std::string* error = nullptr);

[[nodiscard]] std::vector<u8> serialize_cluster_page_file(const ClusterPageFile& file);
bool parse_cluster_page_file(const u8* data, usize size, ClusterPageFile& out, std::string* error = nullptr);
bool write_cluster_page_file(const std::string& path, const ClusterPageFile& file, std::string* error = nullptr);
bool load_cluster_page_file(const std::string& path, ClusterPageFile& out, std::string* error = nullptr);
/// Field-for-field, bit-for-bit.
bool cluster_page_file_equal(const ClusterPageFile& a, const ClusterPageFile& b);

/// Typed view of one page payload (pointers into `data`; valid while it lives). False when the header
/// or a section is out of range / misaligned for `bytes`.
struct PageView {
    const PagePayloadHeader* header = nullptr;
    const StreamCluster* clusters = nullptr;
    const u32* refs = nullptr;
    const u32* triangles = nullptr;
    const u16* positions = nullptr;
    const u32* source_vertices = nullptr;
};
bool view_page(const u8* data, usize bytes, PageView& out);

/// Per-cluster StreamClusterInfo (member / producer page) for the cut kernel.
void build_stream_cluster_info(const geometry::dag::ClusterDag& dag, const ClusterPageFile& file,
                               std::vector<StreamClusterInfo>& out);

/// Decoded (exactly representable) position of a quantised VPOS entry.
inline void decode_page_position(const geometry::QuantParams& q, const u16* vpos, f32 out[3]) {
    for (u32 a = 0; a < 3u; ++a) {
        out[a] = q.offset[a] + static_cast<f32>(vpos[a]) * q.step[a];
    }
}

} // namespace fuse::renderer::geometry_streaming
