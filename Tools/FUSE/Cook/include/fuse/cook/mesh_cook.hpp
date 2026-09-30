#pragma once

#include <fuse/asset/cooked_mesh.hpp>
#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fuse::cook {

// UNI-U7-ASSET-1: the cooked-mesh data types and the FMSH reader live in the runtime asset library
// (Source/FUSE/Asset, fuse/asset/cooked_mesh.hpp). Re-exported here under their original names.
using asset::ClusterDagTable;
using asset::CookedMesh;
using asset::CookedMeshlet;
using asset::MeshletTable;
using asset::MeshLod;
using asset::MeshStreamFormat;
using asset::MeshStreamSemantic;
using asset::kCookedMeshVersion;
using asset::kCookedMeshVersionV1;
using asset::deserialize_cooked_mesh;

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
    /// RE-P1-7: also write the WP-5.3 cluster page file (FCPG, `.fusepages`, see
    /// `cluster_pages_path`) next to the FMSH, built from the same DAG (implies cluster_dag). Without it
    /// a `.fusepages` left next to the output by an earlier cook is removed (it would no longer bind).
    bool cluster_pages = false;
    u32 page_bytes = 64u * 1024u; ///< page payload capacity (multiple of 16, >= 1024)
};

/// GREP-COOK-1: LOD options for a manifest / import `lod_count` (levels including LOD 0). 0 or 1: no
/// LOD chain (returns false). N > 1: N - 1 target ratios, the MeshLodOptions defaults
/// (0.5, 0.25, 0.1) first, then halving. The chain may still stop early (the min_reduction rule).
bool lod_options_for_count(u32 lod_count, MeshLodOptions& out);

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

// deserialize_cooked_mesh: fuse::asset (re-exported above).

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

/// RE-P1-7: `foo/bar.fusemesh` -> `foo/bar.fusepages` (any other extension is replaced the same way).
[[nodiscard]] std::string cluster_pages_path(const std::string& fusemesh_path);

/// True when this build links the renderer's WP-5.3 page builder (`.fusepages` cook available).
[[nodiscard]] bool mesh_cluster_pages_available();

/// RE-P1-7: build the `.fusepages` bytes for `mesh`, which must carry the meshlet + DAG sections built
/// by `build_mesh_meshlets(mesh, true)` from its current (float) streams: the renderer DAG mesh is
/// rebuilt deterministically, checked equal to the FMSH sections, paged with build_cluster_pages and
/// validated against that DAG (validate_cluster_page_file) before serializing.
bool build_mesh_cluster_pages(const CookedMesh& mesh, u32 page_bytes, std::vector<u8>& out,
                              std::string* error = nullptr);

/// Full check of a `.fusepages` file against `mesh` (float streams + DAG sections, e.g. the imported
/// source): the rebuilt DAG equals the mesh's sections and validate_cluster_page_file passes.
bool validate_mesh_cluster_pages(const CookedMesh& mesh, const std::string& pages_path,
                                 std::string* error = nullptr);

/// Cheap binding check of a `.fusepages` file against a loaded FMSH (works on quantised files): the
/// file parses, its layout validates, its cluster / group / leaf counts match the FMSH DAG section and
/// its links_hash equals the hash of the FMSH DCLK links. Used for cache liveness (stale detection).
bool cluster_pages_bind_to_mesh(const CookedMesh& mesh, const std::string& pages_path,
                                std::string* error = nullptr);

/// Import + serialize + write. Fails without writing when the source cannot be imported; the
/// result's `failure` says why (see `import_mesh_file`).
CookStubWriteResult cook_mesh_file(const std::string& input_path, const std::string& output_path,
                                   const MeshCookOptions& options = {});

/// Read and validate a cooked `.fusemesh` file (forwards to asset::read_cooked_mesh_file).
bool load_cooked_mesh(const std::string& path, CookedMesh& out, std::string* error = nullptr);

} // namespace fuse::cook
