#pragma once

// WP-1.2 cooked meshlet format: `.fusemeshlet`, magic "FMLT", version 1.0.
//
// The cook writes it next to the `.fusemesh` (FMSH v1) it was built from ("sidecar"): FMSH v1 bytes
// and readers are untouched. The chunk encodings below are the ones the asset plan's FMSH v2 stream
// table lists (quantised positions, oct tangent + sign, meshlet table with cone + sphere bounds;
// docs/plans/FUSE_ASSET_PLAN.md §5.1, W0.1/W0.2): FMSH v2 is meant to carry these chunks with the same
// fourccs and layouts, so the sidecar is also the v1 -> v2 migration path.
//
// Byte layout (little-endian; offsets from the start of the file):
//
//   Header, 64 bytes
//     0  char[4] magic "FMLT"
//     4  u16     version_major = 1    readers reject any other major
//     6  u16     version_minor = 0    readers accept any minor (newer minors only add chunks / flags)
//     8  u32     header_bytes = 64
//    12  u32     flags                 bit 0 normals from source, bit 1 tangents from source,
//                                      bit 2 uv0 from source (else generated / zero); informational
//    16  u32     chunk_count
//    20  u32     vertex_count
//    24  u32     triangle_count        == MTRI elements == sum of meshlet triangle counts
//    28  u32     meshlet_count
//    32  u32     submesh_count
//    36  u16     max_vertices (64)     per-meshlet limits the cook used (<= 255 / 252)
//    38  u16     max_triangles (124)
//    40  u64     source_hash           FNV-1a 64 of the `.fusemesh` bytes it was built from (0: none)
//    48  u32     meshlet_vertex_count  == MVRT elements
//    52  u32[3]  reserved (0)
//   Chunk table, chunk_count x 32 bytes at offset 64
//     u32 fourcc, u32 element_bytes, u32 element_count, u32 reserved (0), u64 offset, u64 byte_size
//     byte_size == element_bytes * element_count; offset 16-byte aligned; payloads in table order,
//     non-overlapping, after the table and before the trailer; gaps are zero padding.
//   Trailer: u64 FNV-1a 64 of every preceding byte.
//
// Chunks (element size in bytes). All required except VSRC; unknown fourccs are skipped, a known
// fourcc twice is an error.
//   QPRM  48 x 1         s32 exponent[3], f32 offset[3], f32 step[3], u32 reserved[3] (QuantParams)
//   SUBM  16 x submesh   u32 meshlet_offset, meshlet_count, material_index, triangle_count
//   MSHL  96 x meshlet   u32 vertex_offset, u32 triangle_offset, u8 vertex_count, u8 triangle_count,
//                        u16 submesh, f32 center[3], f32 radius, f32 cone_apex[3], f32 cone_axis[3],
//                        f32 cone_cutoff, s8 cone_axis_s8[3], s8 cone_cutoff_s8, f32 aabb_min[3],
//                        f32 aabb_max[3], u32 reserved[3] (0)
//   MVRT   4 x mvert     u32 mesh vertex index
//   MTRI   4 x triangle  u32 i0 | i1 << 8 | i2 << 16 (meshlet-local, top byte 0)
//   VPOS   8 x vertex    u16 x, y, z quantised, u16 w (bit 0 tangent sign negative, rest 0)
//   VNRM   4 x vertex    oct snorm16x2 normal
//   VTAN   4 x vertex    oct snorm16x2 tangent
//   VUV0   4 x vertex    half2 uv0
//   VSRC   4 x vertex    source (FMSH) vertex index of each cooked vertex (optional)
//
// Structural rules the reader enforces beyond sizes: meshlets are packed in order (vertex_offset and
// triangle_offset are running sums, so each triangle belongs to exactly one meshlet), each submesh
// owns a contiguous meshlet range and the ranges tile the table in order, per-meshlet counts are
// 1..max, micro-indices < vertex_count, MVRT entries < vertex_count, bounds finite with
// aabb_min <= aabb_max and radius >= 0, step == 2^exponent and offset a multiple of step with
// |offset / step| + 65535 < 2^24 (the exact-decode contract in vertex_codec_kernel.hpp).

#include <fuse/renderer/geometry/meshlet_types.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::geometry {

inline constexpr u16 kMeshletFormatVersionMajor = 1u;
inline constexpr u16 kMeshletFormatVersionMinor = 0u;
inline constexpr const char* kMeshletFileExtension = ".fusemeshlet";

inline constexpr u32 kMeshletFlagNormalsFromSource = 1u << 0;
inline constexpr u32 kMeshletFlagTangentsFromSource = 1u << 1;
inline constexpr u32 kMeshletFlagUvFromSource = 1u << 2;

/// A parsed (or freshly built) `.fusemeshlet`.
struct MeshletMesh {
    u16 version_minor = kMeshletFormatVersionMinor;
    u32 flags = 0;
    u32 max_vertices = kMeshletMaxVertices;
    u32 max_triangles = kMeshletMaxTriangles;
    u64 source_hash = 0;
    QuantParams quant{};
    std::vector<SubmeshRange> submeshes;
    std::vector<MeshletRecord> meshlets;
    std::vector<u32> meshlet_vertices;  ///< MVRT
    std::vector<u32> meshlet_triangles; ///< MTRI (pack_triangle)
    std::vector<u16> positions;         ///< VPOS, 4 per vertex
    std::vector<u32> normals;           ///< VNRM
    std::vector<u32> tangents;          ///< VTAN
    std::vector<u32> uvs;               ///< VUV0
    std::vector<u32> source_vertices;   ///< VSRC (empty: chunk absent)

    [[nodiscard]] u32 vertex_count() const { return static_cast<u32>(normals.size()); }
    [[nodiscard]] u32 triangle_count() const { return static_cast<u32>(meshlet_triangles.size()); }
};

/// Field-for-field, bit-for-bit equality (floats compared by their bits).
bool meshlet_mesh_equal(const MeshletMesh& a, const MeshletMesh& b);

enum class MeshletFormatError : u8 {
    None = 0,
    Truncated,          ///< shorter than header + table + trailer, or a chunk past the end
    BadMagic,
    UnsupportedVersion, ///< version_major != 1
    BadHeader,          ///< header_bytes, reserved fields, limits
    BadChunkTable,      ///< alignment, overlap, order, byte_size != element_bytes * count, reserved
    MissingChunk,
    DuplicateChunk,
    BadElementSize,     ///< known chunk with the wrong element_bytes or count
    ChecksumMismatch,
    Invalid,            ///< structural / semantic rule broken (see the header comment)
};

[[nodiscard]] const char* meshlet_format_error_name(MeshletFormatError error);

/// FNV-1a 64 (the hash FMSH v1 uses for its trailer).
[[nodiscard]] u64 meshlet_fnv1a64(const u8* data, usize size);

/// Semantic checks shared by the reader and the builder (everything but byte layout).
bool validate_meshlet_mesh(const MeshletMesh& mesh, std::string* error = nullptr);

/// Deterministic: output depends only on `mesh`.
[[nodiscard]] std::vector<u8> serialize_meshlet_mesh(const MeshletMesh& mesh);

bool parse_meshlet_mesh(const u8* data, usize size, MeshletMesh& out, std::string* error = nullptr,
                        MeshletFormatError* code = nullptr);

bool write_meshlet_file(const std::string& path, const MeshletMesh& mesh, std::string* error = nullptr);
bool load_meshlet_file(const std::string& path, MeshletMesh& out, std::string* error = nullptr,
                       MeshletFormatError* code = nullptr);

/// `foo/bar.fusemesh` -> `foo/bar.fusemeshlet` (any other extension is replaced the same way).
[[nodiscard]] std::string meshlet_sidecar_path(const std::string& fusemesh_path);

} // namespace fuse::renderer::geometry
