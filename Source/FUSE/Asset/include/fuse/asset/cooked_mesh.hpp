#pragma once

// UNI-U7-ASSET-1: the runtime FMSH (`.fusemesh`) reader and the cooked-mesh data it produces.
//
// Moved out of the offline cook (Tools/FUSE/Cook, asset plan W0.1 / W0.2) so runtime targets can read
// cooked meshes without linking assimp or the encoders. Tools/FUSE/Cook keeps the writers and
// re-exports these names in `fuse::cook` (fuse/cook/mesh_cook.hpp), so existing tools and tests are
// unchanged. The layout is documented on `fuse::cook::serialize_cooked_mesh`; the shared constants are
// in fuse/asset/detail/fmsh_layout.hpp. The meshopt vertex / index codec (flag bit 4) needs
// meshoptimizer in the build (`mesh_codec_available()`); every other FMSH feature is always readable.

#include <fuse/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fuse::asset {

/// One discrete LOD level (W0.2). `ranges` holds one index range per submesh, into
/// `CookedMesh::lod_indices`; the ranges of all levels tile `lod_indices` in order.
struct MeshLod {
    struct Range {
        u32 index_offset = 0;
        u32 index_count = 0;
        bool operator==(const Range&) const = default;
    };
    f32 error = 0.f;        ///< meshopt simplification error vs LOD 0, object-space units (absolute)
    f32 target_ratio = 1.f; ///< requested triangle fraction of LOD 0
    std::vector<Range> ranges;
    bool operator==(const MeshLod&) const = default;
};

/// Meshlet record: the WP-1.2 `MSHL` element (renderer geometry/meshlet_types.hpp MeshletRecord),
/// field for field. Bounds are the renderer's (computed on its quantised positions, conservative).
struct CookedMeshlet {
    u32 vertex_offset = 0;   ///< into MeshletTable::vertices (running sums)
    u32 triangle_offset = 0; ///< into MeshletTable::triangles (running sums)
    u32 vertex_count = 0;
    u32 triangle_count = 0;
    u32 submesh = 0;
    f32 center[3] = {0.f, 0.f, 0.f};
    f32 radius = 0.f;
    f32 cone_apex[3] = {0.f, 0.f, 0.f};
    f32 cone_axis[3] = {0.f, 0.f, 0.f};
    f32 cone_cutoff = 1.f;
    std::int8_t cone_axis_s8[3] = {0, 0, 0};
    std::int8_t cone_cutoff_s8 = 127;
    f32 aabb_min[3] = {0.f, 0.f, 0.f};
    f32 aabb_max[3] = {0.f, 0.f, 0.f};
    bool operator==(const CookedMeshlet&) const = default;
};

/// FMSH meshlet table (W0.2): the WP-1.2 cook's output for this mesh, with `vertices` holding FMSH
/// vertex indices (the renderer's MVRT mapped through its VSRC), so it needs no extra vertex data.
struct MeshletTable {
    struct SubmeshRange { ///< the renderer's SUBM element
        u32 meshlet_offset = 0;
        u32 meshlet_count = 0;
        u32 material_index = 0;
        u32 triangle_count = 0;
        bool operator==(const SubmeshRange&) const = default;
    };
    u32 max_vertices = 0;  ///< limits the cook used (64 / 124)
    u32 max_triangles = 0;
    std::vector<SubmeshRange> submeshes;
    std::vector<CookedMeshlet> meshlets;
    std::vector<u32> vertices;  ///< FMSH vertex index per meshlet vertex
    std::vector<u32> triangles; ///< packed meshlet-local triangle: i0 | i1 << 8 | i2 << 16

    [[nodiscard]] bool empty() const { return meshlets.empty(); }
    bool operator==(const MeshletTable&) const = default;
};

/// Cluster DAG (W0.2): the WP-5.2 builder's output (renderer geometry/dag/cluster_dag_types.hpp),
/// same records. Cluster ids [0, leaf_cluster_count) are the meshlets of `MeshletTable`; ids above
/// are `lod_clusters`. `lod_vertices` are FMSH vertex indices.
struct ClusterDagTable {
    struct Bounds {
        f32 center[3] = {0.f, 0.f, 0.f};
        f32 radius = 0.f;
        f32 error = 0.f; ///< object space; terminal groups store FLT_MAX
        bool operator==(const Bounds&) const = default;
    };
    struct Group {
        Bounds bounds{};
        u32 member_offset = 0;
        u32 member_count = 0;
        u32 child_offset = 0;
        u32 child_count = 0;
        u32 depth = 0;
        u32 submesh = 0;
        u32 reserved = 0;
        bool operator==(const Group&) const = default;
    };
    struct Link {
        Bounds self{};   ///< bounds of the group that produced the cluster (leaf: meshlet sphere, 0)
        Bounds parent{}; ///< bounds of the group the cluster is a member of
        u32 group = 0;
        u32 refined = 0xFFFFFFFFu; ///< producing group, 0xFFFFFFFF for leaves
        bool operator==(const Link&) const = default;
    };
    u32 leaf_cluster_count = 0;
    u32 level_count = 0;
    std::vector<CookedMeshlet> lod_clusters;
    std::vector<u32> lod_vertices;  ///< FMSH vertex indices
    std::vector<u32> lod_triangles; ///< packed as MeshletTable::triangles
    std::vector<Group> groups;
    std::vector<u32> group_members;
    std::vector<Link> links; ///< one per cluster id

    [[nodiscard]] bool empty() const { return groups.empty(); }
    [[nodiscard]] u32 cluster_count() const { return leaf_cluster_count + static_cast<u32>(lod_clusters.size()); }
    bool operator==(const ClusterDagTable&) const = default;
};

/// Engine binary mesh (`.fusemesh`, magic `FMSH`). Triangle list with 32-bit indices; every
/// vertex carries position, normal and uv0 (zero when the source has no texture coordinates).
///
/// FMSH v2 (asset plan W0.1, docs/plans/FUSE_ASSET_PLAN.md §5.1) adds optional per-vertex streams —
/// tangent (+ bitangent sign), uv1, colour0, skin joints + weights — per-submesh material slot names,
/// and quantised encodings (positions as unorm16 inside the bounds, normals oct-encoded snorm16).
/// A mesh without any of these still serializes as FMSH v1, byte for byte; v1 files keep loading.
struct CookedMesh {
    struct Submesh {
        u32 index_offset = 0;
        u32 index_count = 0;
        u32 vertex_offset = 0;
        u32 material_index = 0;
        bool operator==(const Submesh&) const = default;
    };

    std::vector<f32> positions; ///< xyz per vertex
    std::vector<f32> normals;   ///< xyz per vertex
    std::vector<f32> uvs;       ///< uv per vertex
    std::vector<u32> indices;   ///< global vertex indices, 3 per triangle
    std::vector<Submesh> submeshes;
    f32 bounds_min[3] = {0.f, 0.f, 0.f};
    f32 bounds_max[3] = {0.f, 0.f, 0.f};

    // FMSH v2 optional streams: each is empty (absent) or holds exactly one element per vertex.
    std::vector<f32> tangents;    ///< xyzw per vertex; w = bitangent sign (+1 / -1)
    std::vector<f32> uv1s;        ///< uv per vertex (second UV set: lightmaps, detail)
    std::vector<u8> colors;       ///< RGBA8 per vertex (colour0)
    std::vector<u16> joints;      ///< 4 joint indices per vertex (present iff `weights` is)
    std::vector<f32> weights;     ///< 4 weights per vertex, each >= 0, summing to 1
    std::vector<std::string> material_slots; ///< empty, or one material slot name per submesh

    // FMSH v2 sections (W0.2, meshoptimizer; see build_mesh_lods / build_mesh_meshlets). Each is empty
    // (absent) or complete. They index this mesh's vertex streams directly (no separate vertex data).
    std::vector<MeshLod> lods;      ///< discrete LODs 1..N (LOD 0 is `indices` / `submeshes`, error 0)
    std::vector<u32> lod_indices;   ///< every LOD's index lists, level-major then submesh order
    MeshletTable meshlets;          ///< WP-1.2 meshlets over `indices` (≤ 64 v / 124 t)
    ClusterDagTable cluster_dag;    ///< WP-5.2 cluster DAG over `meshlets` (needs them)

    [[nodiscard]] u32 vertex_count() const { return static_cast<u32>(positions.size() / 3u); }
    /// Exact (bitwise-value) equality of every stream and section (UNI-U7-ASSET-1 gates).
    bool operator==(const CookedMesh&) const = default;
};

/// Stream table ids (FMSH v2). Stored as u32 in the file.
enum class MeshStreamSemantic : u32 {
    Position = 1,
    Normal = 2,
    Uv0 = 3,
    Tangent = 4,
    Uv1 = 5,
    Color0 = 6,
    Joints0 = 7,
    Weights0 = 8,
};

enum class MeshStreamFormat : u32 {
    F32x2 = 1,
    F32x3 = 2,
    Unorm16x3 = 3,    ///< quantised position inside the mesh bounds
    OctSnorm16x2 = 4, ///< octahedral unit vector
    OctSnorm16x2Sign = 5, ///< octahedral tangent + i16 bitangent sign (+1 / -1) + i16 zero pad
    Unorm8x4 = 6,
    Uint8x4 = 7,
    Uint16x4 = 8,
    Unorm16x4 = 9,
};

/// Newest FMSH version written. v1 is still written for meshes with no v2 content.
inline constexpr u32 kCookedMeshVersion = 2;
inline constexpr u32 kCookedMeshVersionV1 = 1;

/// Parse and validate an `FMSH` v1 or v2 blob (magic, version, sizes, stream table, index range,
/// skin weight sums, W0.2 sections, checksum). Quantised streams are decoded back to floats. On
/// failure `out` is reset and `error` (when given) says why.
bool deserialize_cooked_mesh(const u8* data, usize size, CookedMesh& out, std::string* error = nullptr);

/// Read a `.fusemesh` from the host file system and deserialize it (tools / tests; the runtime reads
/// through the VFS and AssetRegistry instead).
bool read_cooked_mesh_file(const std::string& path, CookedMesh& out, std::string* error = nullptr);

/// True when this build can decode meshopt-coded FMSH payloads (meshoptimizer linked).
[[nodiscard]] bool mesh_codec_available();

} // namespace fuse::asset
