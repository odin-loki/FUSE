#pragma once

// Asset plan 4a (L-DTS): clean-room reader for Torque `.dts` shapes (versions 24..28) and their conversion into
// the cook's CookedMesh. Written from the public description of the format, not from Torque's source:
//
//   file    s32 version (low byte = format version, high 16 bits = exporter version), s32 buffer_words,
//           s32 start16 (word offset of the 16-bit stream), s32 start8 (word offset of the 8-bit stream),
//           buffer_words x 4 bytes of memory buffer, then the sequences and the material list.
//   buffer  three interleaved streams read in lockstep: 32-bit words [0, start16), 16-bit [start16, start8),
//           8-bit [start8, buffer_words). Section ends carry a guard: the next value of a counter (0, 1, 2, ...)
//           in all three streams at once; every guard is checked, so a mis-sized section is a parse error.
//   shape   19 counts, bounds, nodes (name, parent, first object, first child, next sibling), objects (name, mesh
//           count, first mesh, node, ...), sub-shapes, default node rotations (Quat16) and translations, sequence
//           key tables, object states, triggers, details (7 words; 13 from v26), [v27+: vertex format + shared
//           vertex buffer], meshes, names.
//   mesh    type (standard / skin / decal / sorted / null) + flags, frames, parent mesh, bounds, vertices, uvs,
//           [v26+: uv1, colours], normals, primitives (start, count, type | material), indices (16-bit before v26,
//           32-bit from v26), verts-per-frame, flags; skins add initial vertices / normals, initial transforms and
//           (vertex, bone, weight) triples over the mesh's bone-node list.
//
// Conversion (dts_to_cooked_mesh): the highest visible detail level (largest non-negative size; collision /
// hidden details have negative sizes), every object's mesh at that level, rigid meshes transformed by their node's
// default-pose world transform (skins are already in shape space), frame 0 of vertex-animated meshes. Torque is
// Z-up / clockwise-front; the cooked mesh is FUSE space (Y-up, counter-clockwise front): p' = (x, z, -y) (the
// world converter's proper rotation), each triangle's winding reversed, v' = 1 - v (Torque stores D3D-style UVs).
// Strips and fans are expanded to lists; the zero-area stitch triangles that strip encoding needs are dropped (they
// are part of the encoding, not geometry). Nothing else is repaired: malformed or unsupported data is refused.

#include <fuse/cook/mesh_cook.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

struct DtsNode {
    std::string name;
    s32 parent = -1;
    f32 rotation[4] = {0.f, 0.f, 0.f, 1.f}; ///< default-pose rotation (x, y, z, w), Torque space
    f32 translation[3] = {0.f, 0.f, 0.f};   ///< default-pose translation, Torque space
};

struct DtsObject {
    std::string name;
    s32 node = -1;
    s32 first_mesh = 0;
    s32 mesh_count = 0;
};

struct DtsDetail {
    std::string name;
    s32 subshape = 0;
    s32 object_detail = 0;
    f32 size = 0.f;     ///< pixel size; negative = never drawn (collision / LOS details)
    s32 poly_count = 0;
};

enum class DtsPrimitiveType : u32 { Triangles = 0, Strip = 1, Fan = 2 };

struct DtsPrimitive {
    u32 start = 0;
    u32 count = 0;
    DtsPrimitiveType type = DtsPrimitiveType::Triangles;
    s32 material = -1; ///< material-list index, -1 = NoMaterial
};

enum class DtsMeshType : u32 { Standard = 0, Skin = 1, Decal = 2, Sorted = 3, Null = 4 };

struct DtsMesh {
    DtsMeshType type = DtsMeshType::Null;
    s32 parent = -1;            ///< mesh whose vertex data this one shares (-1: own data)
    u32 frames = 0;
    u32 verts_per_frame = 0;
    f32 bounds_min[3] = {0.f, 0.f, 0.f};
    f32 bounds_max[3] = {0.f, 0.f, 0.f};
    std::vector<f32> positions; ///< xyz (all frames), Torque space, mesh / node space
    std::vector<f32> normals;   ///< xyz per vertex (may be empty)
    std::vector<f32> uvs;       ///< uv per vertex as stored (D3D-style, v down)
    std::vector<DtsPrimitive> primitives;
    std::vector<u32> indices;
    // Skin data (type Skin, v24..v26): one (vertex, bone, weight) triple per influence; bone indexes bone_nodes.
    std::vector<u32> skin_vertex;
    std::vector<u32> skin_bone;
    std::vector<f32> skin_weight;
    std::vector<s32> bone_nodes;
};

struct DtsShape {
    u32 version = 0;
    u32 exporter_version = 0;
    f32 radius = 0.f;
    f32 bounds_min[3] = {0.f, 0.f, 0.f}; ///< Torque space (every detail, collision included)
    f32 bounds_max[3] = {0.f, 0.f, 0.f};
    std::vector<std::string> names;
    std::vector<DtsNode> nodes;
    std::vector<DtsObject> objects;
    std::vector<DtsDetail> details;
    std::vector<DtsMesh> meshes;
    u32 sequence_count = 0;
    /// Material-list names (read when the shape has no sequences; the sequence records precede the list).
    std::vector<std::string> materials;
};

/// Oldest / newest `.dts` format versions the reader accepts.
inline constexpr u32 kDtsMinVersion = 24;
inline constexpr u32 kDtsMaxVersion = 28;

/// Parse a `.dts` image. False (with `error`) on anything malformed, truncated or unsupported.
bool parse_dts(const u8* data, usize size, DtsShape& out, std::string* error = nullptr);
bool read_dts_file(const std::string& path, DtsShape& out, std::string* error = nullptr);

struct DtsImportStats {
    s32 detail = -1;            ///< detail level converted
    u32 meshes = 0;             ///< meshes converted
    u32 triangles = 0;          ///< triangles in the cooked mesh
    u32 dropped_stitch = 0;     ///< zero-area strip / fan stitch triangles dropped
};

/// DtsShape -> CookedMesh (FUSE space). `options.import_material_names` fills material_slots from the material list;
/// `options.import_skin` keeps skin weights (joints = shape node indices) when every converted mesh is a skin;
/// `options.generate_normals` computes area-weighted normals for meshes stored without them.
bool dts_to_cooked_mesh(const DtsShape& shape, const MeshCookOptions& options, CookedMesh& out,
                        std::string* error = nullptr, DtsImportStats* stats = nullptr);

/// read_dts_file + dts_to_cooked_mesh (what import_mesh_file does for `.dts`).
bool import_dts_file(const std::string& path, const MeshCookOptions& options, CookedMesh& out,
                     std::string* error = nullptr, CookFailure* failure = nullptr, DtsImportStats* stats = nullptr);

} // namespace fuse::cook
