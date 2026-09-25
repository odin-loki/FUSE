#pragma once

#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fuse::cook {

/// One discrete LOD level (W0.2). `ranges` holds one index range per submesh, into
/// `CookedMesh::lod_indices`; the ranges of all levels tile `lod_indices` in order.
struct MeshLod {
    struct Range {
        u32 index_offset = 0;
        u32 index_count = 0;
    };
    f32 error = 0.f;        ///< meshopt simplification error vs LOD 0, object-space units (absolute)
    f32 target_ratio = 1.f; ///< requested triangle fraction of LOD 0
    std::vector<Range> ranges;
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
};

/// FMSH meshlet table (W0.2): the WP-1.2 cook's output for this mesh, with `vertices` holding FMSH
/// vertex indices (the renderer's MVRT mapped through its VSRC), so it needs no extra vertex data.
struct MeshletTable {
    struct SubmeshRange { ///< the renderer's SUBM element
        u32 meshlet_offset = 0;
        u32 meshlet_count = 0;
        u32 material_index = 0;
        u32 triangle_count = 0;
    };
    u32 max_vertices = 0;  ///< limits the cook used (64 / 124)
    u32 max_triangles = 0;
    std::vector<SubmeshRange> submeshes;
    std::vector<CookedMeshlet> meshlets;
    std::vector<u32> vertices;  ///< FMSH vertex index per meshlet vertex
    std::vector<u32> triangles; ///< packed meshlet-local triangle: i0 | i1 << 8 | i2 << 16

    [[nodiscard]] bool empty() const { return meshlets.empty(); }
};

/// Cluster DAG (W0.2): the WP-5.2 builder's output (renderer geometry/dag/cluster_dag_types.hpp),
/// same records. Cluster ids [0, leaf_cluster_count) are the meshlets of `MeshletTable`; ids above
/// are `lod_clusters`. `lod_vertices` are FMSH vertex indices.
struct ClusterDagTable {
    struct Bounds {
        f32 center[3] = {0.f, 0.f, 0.f};
        f32 radius = 0.f;
        f32 error = 0.f; ///< object space; terminal groups store FLT_MAX
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
    };
    struct Link {
        Bounds self{};   ///< bounds of the group that produced the cluster (leaf: meshlet sphere, 0)
        Bounds parent{}; ///< bounds of the group the cluster is a member of
        u32 group = 0;
        u32 refined = 0xFFFFFFFFu; ///< producing group, 0xFFFFFFFF for leaves
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
};

/// FMSH v2 stream encodings. The defaults store full-precision floats; any non-default choice makes the
/// file FMSH v2.
struct MeshEncoding {
    bool quantize_positions = false; ///< unorm16 × 3 inside [bounds_min, bounds_max]
    bool quantize_normals = false;   ///< octahedral snorm16 × 2 (zero normals decode as +Z)
    bool weights_unorm8 = false;     ///< skin weights as unorm8 × 4 instead of unorm16 × 4
    /// W0.2: vertex streams (meshopt vertex codec v1, level 2) and index lists (meshopt index codec v1)
    /// stored encoded; lossless, decoded bit-exactly on load. Needs meshoptimizer in the build
    /// (`mesh_optimizer_available()`); without it the payloads are stored raw.
    bool meshopt_codec = false;
};

/// W0.2 discrete LOD chain (asset plan §1.4): meshopt_simplifyWithAttributes (normals weighted) of
/// LOD 0 per submesh at each target ratio, vertex-cache optimised. A level is kept only when it cuts
/// at least `min_reduction` of the previous level's triangles (the §5.3 `asset_budget_mesh` rule);
/// the chain stops at the first level that does not.
struct MeshLodOptions {
    std::vector<f32> ratios = {0.5f, 0.25f, 0.1f}; ///< LOD 1..N triangle fractions of LOD 0
    f32 min_reduction = 0.4f;
    f32 normal_weight = 0.5f; ///< attribute weight of each normal component (0: positions only)
    bool lock_border = false; ///< meshopt_SimplifyLockBorder (modular pieces that must tile)
};

/// W0.2 meshoptimizer steps run by `cook_mesh_file` after import (all off by default).
struct MeshOptimizeOptions {
    bool lods = false;
    MeshLodOptions lod{};
    bool meshlets = false;     ///< WP-1.2 meshlet table (renderer fuse_geometry)
    bool cluster_dag = false;  ///< WP-5.2 cluster DAG (implies meshlets)
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

/// Optional post-write hook for `cook_mesh_file` (WP-1.2: the renderer's meshlet cook installs one
/// that writes a `.fusemeshlet` sidecar, see Source/FUSE/Renderer/geometry/meshlet_cook_hook.hpp).
/// Called after the `.fusemesh` is written, with the cooked mesh, its exact FMSH bytes and the output
/// path. It never changes the FMSH bytes. Returning false fails the cook (`WriteFailed`, hook note).
using MeshCookPostHook = bool (*)(const CookedMesh& mesh, const std::vector<u8>& fmsh_bytes,
                                  const std::string& output_path, std::string* note);

struct MeshCookOptions {
    bool generate_normals = true;
    MeshCookPostHook post_hook = nullptr; ///< nullptr: FMSH only (default)

    // FMSH v2 streams (W0.1). All off by default, so existing cooks stay FMSH v1 byte for byte. A
    // stream is written only when every imported sub-mesh provides it.
    bool import_tangents = false;       ///< assimp CalcTangentSpace (needs uv0)
    bool import_uv1 = false;
    bool import_colors = false;
    /// Joints + weights (≤ 4 per vertex, the 4 largest kept and renormalised). Disables assimp's
    /// pre-transform (it deletes bones), so vertices stay in each mesh's bind-pose space. A source with
    /// bones on some sub-meshes but not others, or skinned vertices without weights, is rejected.
    bool import_skin = false;
    bool import_material_names = false;
    MeshEncoding encoding{};
    MeshOptimizeOptions optimize{}; ///< W0.2 LODs / meshlets / cluster DAG
};

/// Newest FMSH version written. v1 is still written for meshes with no v2 content.
inline constexpr u32 kCookedMeshVersion = 2;
inline constexpr u32 kCookedMeshVersionV1 = 1;

/// Import FBX / glTF / OBJ / … through assimp into `CookedMesh`. Returns false (with `error` and a
/// `failure` class) when the library is absent (`ImporterUnavailable`), the file cannot be parsed
/// (`MalformedSource`), or the parsed geometry is unusable (`InvalidGeometry`): no triangles, face
/// indices outside the vertex range (including faces an importer silently dropped for that reason),
/// or non-finite positions / normals / uvs. Nothing is ever "repaired" — bad data is rejected.
bool import_mesh_file(const std::string& input_path, const MeshCookOptions& options, CookedMesh& out,
                      std::string* error = nullptr, CookFailure* failure = nullptr);

/// W0.2 additions to FMSH v2 (additive: files without them are byte-identical to W0.1 v2 and load
/// unchanged). Header flag bits (bits 1..3 stay reserved and are refused):
///   bit 4  meshopt codec: each stream-table entry gains a 4th u32 `encoded_bytes`, the payload is
///          the meshopt vertex-codec (v1) encoding of the stream with each element zero-padded to a
///          multiple of 4 bytes (Unorm16x3 → 8), padded to 4; `byte_length` stays the decoded size.
///          The index list becomes u32 `encoded_bytes` + meshopt index-codec (v1) bytes, padded to 4:
///          the triangle codec (first byte 0xE_) when it reproduces the list exactly (it may rotate
///          corners), else the index-sequence codec (0xD_). Lossless either way.
///   bit 5  sections: after the index list, u32 `section_count`, then per section u32 fourcc,
///          u32 element_bytes, u32 element_count and element_bytes × element_count payload bytes
///          (padded to 4). Unknown fourccs are skipped; a known one twice is refused. Known:
///            LODT 16 × lod        f32 error, f32 target_ratio, u32 reserved[2] (0)
///            LODR  8 × lod×sub    u32 index_offset, index_count per (level, submesh); tile LODI
///            LODI  4 × n          u32 LOD indices  | LODZ 1 × bytes: meshopt index codec (bit 4)
///            MLTH 16 × 1          u32 max_vertices, max_triangles, reserved[2] (0)
///            SUBM / MSHL / MTRI   the WP-1.2 `.fusemeshlet` chunks, byte-identical
///            MVRT  4 × n          FMSH vertex index (the renderer's MVRT mapped through VSRC)
///            DAGH / DMSH / DMTR / DGRP / DGMB / DCLK   the WP-5.2 FMLT 1.1 chunks, byte-identical
///            DMVR  4 × n          FMSH vertex index (mapped like MVRT)
///          Meshlet sections come all together or not at all, likewise DAG ones (which need them).
/// Serialize to the little-endian `FMSH` layout (header, submeshes, streams, FNV-1a trailer).
/// Output depends only on mesh contents and `encoding` — never on paths, time, or host. Writes v1
/// when the mesh has no v2 stream and `encoding` is default, v2 otherwise. Joints are stored as
/// u8 × 4 when every index is < 256, else u16 × 4; quantised weights always sum exactly to 1.
std::vector<u8> serialize_cooked_mesh(const CookedMesh& mesh, const MeshEncoding& encoding = {});

/// Parse and validate an `FMSH` v1 or v2 blob (magic, version, sizes, stream table, index range,
/// skin weight sums, checksum). Quantised streams are decoded back to floats.
bool deserialize_cooked_mesh(const u8* data, usize size, CookedMesh& out, std::string* error = nullptr);

/// True when this build links meshoptimizer (LODs, codec) — `mesh_meshlets_available()` additionally
/// needs the renderer's WP-1.2 / WP-5.2 geometry libraries.
[[nodiscard]] bool mesh_optimizer_available();
[[nodiscard]] bool mesh_meshlets_available();

/// Build `mesh.lods` / `mesh.lod_indices` (replacing any). Deterministic. False (mesh unchanged) when
/// meshoptimizer is unavailable or the mesh is invalid.
bool build_mesh_lods(CookedMesh& mesh, const MeshLodOptions& options = {}, std::string* error = nullptr);

/// Build `mesh.meshlets` with the renderer's WP-1.2 builder (default options: 64 v / 124 t, cone
/// weight 0.25; the same call the `.fusemeshlet` sidecar cook makes) and, when `with_dag`,
/// `mesh.cluster_dag` with the WP-5.2 builder (default DagBuildOptions). Deterministic.
bool build_mesh_meshlets(CookedMesh& mesh, bool with_dag, std::string* error = nullptr);

/// Screen-space LOD switch distance: the distance at which an object-space `error` projects to
/// `pixels` on a viewport `viewport_height_px` tall with vertical field of view `fov_y_radians`.
[[nodiscard]] f32 lod_switch_distance(f32 error, f32 fov_y_radians, f32 viewport_height_px, f32 pixels = 1.f);

/// Import + serialize + write. Fails without writing when the source cannot be imported; the
/// result's `failure` says why (see `import_mesh_file`).
CookStubWriteResult cook_mesh_file(const std::string& input_path, const std::string& output_path,
                                   const MeshCookOptions& options = {});

/// Read and validate a cooked `.fusemesh` file.
bool load_cooked_mesh(const std::string& path, CookedMesh& out, std::string* error = nullptr);

} // namespace fuse::cook
