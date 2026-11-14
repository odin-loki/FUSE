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
// are part of the encoding, not geometry). Nothing else is repaired: malformed or unsupported data is refused
// (MeshCookOptions::repair_degenerate is the documented, opt-in exception, applied by import_mesh_file).
//
// Sequences (after the memory buffer): s32 count, then per sequence s32 name index, u32 flags (1 uniform scale,
// 2 aligned scale, 4 arbitrary scale, 8 blend, 16 cyclic, 32 make path), s32 keyframes, f32 duration (s), s32
// priority, s32 first / count ground frames, s32 base rotation / translation / scale / object state / decal state,
// s32 first / count triggers, f32 tool begin, then eight integer sets (rotation, translation, scale, decal, IFL,
// visibility, frame, material frame matters; each s32 0, s32 word count, word count x u32 bits). A node in a
// sequence's rotation set takes keyframes [base_rotation + rank * keyframes, + keyframes) of the shape's node
// rotation pool (rank = its position among the set's nodes); translations and scales likewise (the scale pool is
// the one the flags name). Keyframe k sits at pos k / keyframes (cyclic: the last key blends back into key 0) or
// k / (keyframes - 1) (one-shot) of the duration. Ground frames g = 1..n sit at pos g / n with the identity at 0
// (root motion). Triggers: u32 state (bit 31 on, bit 30 invert on reverse, low bits a one-hot trigger number) +
// f32 pos. The material list follows (u8 version 1, u32 count, count x (u8 length + name), then per material
// u32 flags, s32 reflectance / bump / detail map, f32 detail scale, f32 reflection amount).
//
// `.dsq` sequence files (parse_dsq): s32 version, s32 node count + node names (s32 length + bytes), s32 0 (legacy
// objects), s32 object count of the exporting shape, then the pools with their own counts (rotations Quat16,
// translations, uniform / aligned scales, arbitrary scale rotations + factors, ground translations + rotations),
// s32 object states (0), s32 sequences (s32 name length + name, then the record above without the name index),
// s32 triggers (state, pos). Set bits index the file's node list; nodes map onto a shape by name (ASCII
// case-insensitive, as Torque does).
//
// No template shape under Templates/ carries sequences (all 20 non-empty `.dts` files have a sequence count of 0,
// and there is no `.dsq`), so the sequence layout above is exercised against shapes written by the clean-room
// writer (dts_write.hpp) and a `.dsq` animating the Verve soldier's real skeleton.
//
// Sequences -> `.fuseanim` (dts_sequence_to_cooked_anim): the joints are every shape node in dts_joint_order (node
// order when each parent precedes its children, else depth-first pre-order), so the clip's skeleton hash (names +
// parents) equals the shape skeleton's and the skinned mesh's rig. Tracks are local TRS in FUSE space (vectors and
// quaternion axes p' = (x, z, -y), aligned scales swap y / z), sampled at `sample_rate` with Torque's keyframe
// spacing (lerp / shortest-path slerp). Nodes outside a set keep an absent track (the bind value). Ground frames
// become the root motion of joint 0 (cumulative translation + yaw of +Z about +Y). Triggers become events
// "trigger<N>" (on) / "trigger<N>_off". Blend sequences are cooked as stored (their keys are relative to the blend
// reference); arbitrary scales keep their factors and drop the scale rotation; visibility / IFL / mesh frames are
// not exported.

#include <fuse/animation/cooked_anim.hpp>
#include <fuse/cook/mesh_cook.hpp>
#include <fuse/types.hpp>

#include <array>
#include <cstdint>
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
    std::vector<f32> skin_initial_transforms; ///< 16 floats per bone (row-major, shape -> bone space), as stored
};

/// Quat16: x, y, z, w as signed 16-bit, value / 32767.
using DtsQuat16 = std::array<std::int16_t, 4>;

/// TSShape sequence flags.
inline constexpr u32 kDtsSeqUniformScale = 1u << 0;
inline constexpr u32 kDtsSeqAlignedScale = 1u << 1;
inline constexpr u32 kDtsSeqArbitraryScale = 1u << 2;
inline constexpr u32 kDtsSeqBlend = 1u << 3;
inline constexpr u32 kDtsSeqCyclic = 1u << 4;
inline constexpr u32 kDtsSeqMakePath = 1u << 5;

/// Trigger state bits.
inline constexpr u32 kDtsTriggerOn = 1u << 31;
inline constexpr u32 kDtsTriggerInvertOnReverse = 1u << 30;
inline constexpr u32 kDtsTriggerStateMask = (1u << 30) - 1u;

/// One integer set (bit i = element i).
struct DtsIntegerSet {
    std::vector<u32> words;
    [[nodiscard]] bool test(u32 i) const { return i / 32u < words.size() && ((words[i / 32u] >> (i % 32u)) & 1u) != 0u; }
    void set(u32 i) {
        if (words.size() <= i / 32u) {
            words.resize(i / 32u + 1u, 0u);
        }
        words[i / 32u] |= 1u << (i % 32u);
    }
    /// Rank of `i` among the set elements (number of set bits below i).
    [[nodiscard]] u32 rank(u32 i) const;
    [[nodiscard]] u32 count() const;
    /// Highest set element + 1 (0 when empty).
    [[nodiscard]] u32 extent() const;
    friend bool operator==(const DtsIntegerSet&, const DtsIntegerSet&) = default;
};

struct DtsSequence {
    std::string name;
    u32 flags = 0;
    s32 keyframes = 0;
    f32 duration = 0.f; ///< seconds
    s32 priority = 0;
    s32 first_ground_frame = 0;
    s32 ground_frames = 0;
    s32 base_rotation = 0;
    s32 base_translation = 0;
    s32 base_scale = 0;
    s32 base_object_state = 0;
    s32 base_decal_state = 0;
    s32 first_trigger = 0;
    s32 trigger_count = 0;
    f32 tool_begin = 0.f;
    DtsIntegerSet rotation_matters, translation_matters, scale_matters, decal_matters, ifl_matters, vis_matters,
        frame_matters, mat_frame_matters;
    [[nodiscard]] bool cyclic() const { return (flags & kDtsSeqCyclic) != 0u; }
    friend bool operator==(const DtsSequence&, const DtsSequence&) = default;
};

struct DtsTrigger {
    u32 state = 0;
    f32 pos = 0.f; ///< 0..1 of the sequence
    friend bool operator==(const DtsTrigger&, const DtsTrigger&) = default;
};

/// Keyframe pools a shape's (or a `.dsq`'s) sequences index, as stored.
struct DtsKeyframes {
    std::vector<DtsQuat16> node_rotations;
    std::vector<f32> node_translations;            ///< xyz
    std::vector<f32> node_uniform_scales;
    std::vector<f32> node_aligned_scales;          ///< xyz
    std::vector<f32> node_arbitrary_scale_factors; ///< xyz
    std::vector<DtsQuat16> node_arbitrary_scale_rotations;
    std::vector<f32> ground_translations;          ///< xyz
    std::vector<DtsQuat16> ground_rotations;
    friend bool operator==(const DtsKeyframes&, const DtsKeyframes&) = default;
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
    DtsKeyframes keyframes;
    std::vector<DtsSequence> sequences;
    std::vector<DtsTrigger> triggers;
    /// Material-list names (after the sequences).
    std::vector<std::string> materials;
};

/// A `.dsq` sequence file: node names, keyframe pools, sequences and triggers (indices into this file's pools).
struct DsqFile {
    u32 version = 0;
    std::vector<std::string> nodes;
    s32 shape_object_count = 0;
    DtsKeyframes keyframes;
    std::vector<DtsSequence> sequences;
    std::vector<DtsTrigger> triggers;
};

/// Oldest / newest `.dsq` versions the reader accepts (the >= 22 layout).
inline constexpr u32 kDsqMinVersion = 22;
inline constexpr u32 kDsqMaxVersion = 28;

/// Oldest / newest `.dts` format versions the reader accepts.
inline constexpr u32 kDtsMinVersion = 24;
inline constexpr u32 kDtsMaxVersion = 28;

/// Parse a `.dts` image. False (with `error`) on anything malformed, truncated or unsupported.
bool parse_dts(const u8* data, usize size, DtsShape& out, std::string* error = nullptr);
bool read_dts_file(const std::string& path, DtsShape& out, std::string* error = nullptr);

/// Parse a `.dsq` image. False (with `error`) on anything malformed, truncated or unsupported (also when a
/// sequence indexes past the file's pools or nodes).
bool parse_dsq(const u8* data, usize size, DsqFile& out, std::string* error = nullptr);
bool read_dsq_file(const std::string& path, DsqFile& out, std::string* error = nullptr);

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

/// One detail level (index into shape.details; collision / hidden details included) -> CookedMesh.
bool dts_detail_to_cooked_mesh(const DtsShape& shape, u32 detail, const MeshCookOptions& options, CookedMesh& out,
                               std::string* error = nullptr, DtsImportStats* stats = nullptr);

/// The highest visible detail (largest non-negative size, ties: the first); -1 when there is none.
[[nodiscard]] s32 dts_highest_visible_detail(const DtsShape& shape);

/// The joint order of a shape's skeleton: node order when every parent precedes its children, else depth-first
/// pre-order (roots and children in node order). order[joint] = node index.
[[nodiscard]] std::vector<u32> dts_joint_order(const DtsShape& shape);

/// The shape's skeleton in dts_joint_order: names and parents (joint indices).
void dts_skeleton(const DtsShape& shape, std::vector<std::string>& names, std::vector<s32>& parents);

struct DtsAnimOptions {
    f32 sample_rate = 30.f;
    bool root_motion = true; ///< ground frames -> root motion (when the sequence has any)
};

/// One sequence of `shape` -> CookedAnim (float samples; encode_cooked_anim quantises). False on bad indices.
bool dts_sequence_to_cooked_anim(const DtsShape& shape, u32 sequence, const DtsAnimOptions& options,
                                 animation::CookedAnim& out, std::string* error = nullptr);

/// One sequence of a `.dsq` -> CookedAnim on `shape`'s skeleton (dsq nodes map to shape nodes by name; unmatched
/// dsq nodes are skipped and counted in `unmatched`). Without a shape (nullptr) the joints are the dsq's nodes,
/// all roots.
bool dsq_sequence_to_cooked_anim(const DsqFile& dsq, u32 sequence, const DtsShape* shape, const DtsAnimOptions& options,
                                 animation::CookedAnim& out, std::string* error = nullptr, u32* unmatched = nullptr);

/// read_dts_file + dts_to_cooked_mesh (what import_mesh_file does for `.dts`).
bool import_dts_file(const std::string& path, const MeshCookOptions& options, CookedMesh& out,
                     std::string* error = nullptr, CookFailure* failure = nullptr, DtsImportStats* stats = nullptr);

} // namespace fuse::cook
