// Asset plan 4a (L-DTS): clean-room Torque `.dts` reader + CookedMesh conversion. Format notes: dts_import.hpp.

#include <fuse/cook/dts_import.hpp>

#include <fuse/animation/math.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

namespace fuse::cook {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

constexpr u32 kTypeMask = 0x7u;                // mesh type in the low bits of the mesh-type word
constexpr u32 kPrimTypeShift = 30u;            // primitive type in bits 30..31
constexpr u32 kPrimNoMaterial = 1u << 28u;     // primitive without a material
constexpr u32 kPrimMaterialMask = 0x0FFFFFFFu; // material index bits
constexpr u32 kMaxElements = 1u << 26u;        // sanity bound for any one count (64 M)

/// The three lockstep streams of the memory buffer. Every read is bounds-checked; the first failure sticks.
class Streams {
public:
    Streams(const u8* base, usize words, usize start16, usize start8)
        : m_base(base), m_end32(start16 * 4u), m_pos16(start16 * 4u), m_end16(start8 * 4u), m_pos8(start8 * 4u),
          m_end8(words * 4u) {}

    bool ok() const { return m_ok; }
    const std::string& failure() const { return m_failure; }
    void fail(const std::string& why) {
        if (m_ok) {
            m_ok = false;
            m_failure = why;
        }
    }

    s32 i32() {
        if (!need(m_pos32, m_end32, 4u, "32-bit")) {
            return 0;
        }
        s32 v = 0;
        std::memcpy(&v, m_base + m_pos32, 4u);
        m_pos32 += 4u;
        return v;
    }
    f32 f() {
        const s32 bits = i32();
        f32 v = 0.f;
        std::memcpy(&v, &bits, 4u);
        return v;
    }
    void f(f32* out, usize n) {
        for (usize i = 0; i < n; ++i) {
            out[i] = f();
        }
    }
    std::int16_t i16() {
        if (!need(m_pos16, m_end16, 2u, "16-bit")) {
            return 0;
        }
        std::int16_t v = 0;
        std::memcpy(&v, m_base + m_pos16, 2u);
        m_pos16 += 2u;
        return v;
    }
    std::int8_t i8() {
        if (!need(m_pos8, m_end8, 1u, "8-bit")) {
            return 0;
        }
        const std::int8_t v = static_cast<std::int8_t>(m_base[m_pos8]);
        m_pos8 += 1u;
        return v;
    }
    /// A count field: non-negative and small enough that `elementWords` 32-bit words of it could still be present.
    u32 count(const char* what, usize elementWords = 0u) {
        const s32 n = i32();
        if (!m_ok) {
            return 0u;
        }
        if (n < 0 || static_cast<u32>(n) > kMaxElements ||
            (elementWords != 0u && static_cast<usize>(n) * elementWords * 4u > m_end32 - m_pos32)) {
            fail(std::string("bad ") + what + " count " + std::to_string(n));
            return 0u;
        }
        return static_cast<u32>(n);
    }
    /// True when `halves` 16-bit values remain (fails the stream otherwise): checked before sizing a vector.
    bool has16(usize halves) {
        return need(m_pos16, m_end16, halves * 2u, "16-bit");
    }
    void quats(std::vector<DtsQuat16>& out, usize n) {
        out.clear();
        if (!has16(n * 4u)) {
            return;
        }
        out.resize(n);
        for (DtsQuat16& q : out) {
            for (std::int16_t& c : q) {
                c = i16();
            }
        }
    }
    void floats(std::vector<f32>& out, usize n) {
        out.clear();
        if (!need(m_pos32, m_end32, n * 4u, "32-bit")) {
            return;
        }
        out.resize(n);
        f(out.data(), n);
    }
    void skip32(usize words) {
        if (need(m_pos32, m_end32, words * 4u, "32-bit")) {
            m_pos32 += words * 4u;
        }
    }
    void skip16(usize halves) {
        if (need(m_pos16, m_end16, halves * 2u, "16-bit")) {
            m_pos16 += halves * 2u;
        }
    }
    void skip8(usize bytes) {
        if (need(m_pos8, m_end8, bytes, "8-bit")) {
            m_pos8 += bytes;
        }
    }
    /// Raw bytes of the 8-bit stream (v27+ shared vertex buffer). Null on failure.
    const u8* take8(usize bytes) {
        if (!need(m_pos8, m_end8, bytes, "8-bit")) {
            return nullptr;
        }
        const u8* p = m_base + m_pos8;
        m_pos8 += bytes;
        return p;
    }
    /// Section guard: the next counter value in all three streams.
    void guard(const char* section) {
        const s32 a = i32();
        const std::int16_t b = i16();
        const std::int8_t c = i8();
        if (!m_ok) {
            return;
        }
        if (a != static_cast<s32>(m_guard) || b != static_cast<std::int16_t>(m_guard) || c != static_cast<std::int8_t>(m_guard)) {
            fail(std::string("guard ") + std::to_string(m_guard) + " mismatch after " + section + " (32-bit " +
                 std::to_string(a) + ", 16-bit " + std::to_string(b) + ", 8-bit " + std::to_string(c) + ")");
            return;
        }
        ++m_guard;
    }
    std::string cstring() {
        std::string s;
        while (m_ok) {
            const std::int8_t c = i8();
            if (!m_ok || c == 0) {
                break;
            }
            s.push_back(static_cast<char>(c));
        }
        return s;
    }

private:
    bool need(usize pos, usize end, usize bytes, const char* stream) {
        if (!m_ok) {
            return false;
        }
        if (pos > end || bytes > end - pos) {
            fail(std::string("truncated ") + stream + " stream");
            return false;
        }
        return true;
    }

    const u8* m_base;
    usize m_pos32 = 0;
    usize m_end32;
    usize m_pos16;
    usize m_end16;
    usize m_pos8;
    usize m_end8;
    u32 m_guard = 0;
    bool m_ok = true;
    std::string m_failure;
};

/// v27+ shared vertex buffer layout (5 values from the 16-bit stream).
struct VertexFormat {
    s32 texcoord_offset = -1;
    s32 bone_offset = -1;
    s32 color_offset = -1;
    s32 bones = 0;
    s32 vertex_size = 0;
};

bool parse_mesh(Streams& s, u32 version, const u8* vertexData, usize vertexBytes, const VertexFormat& format,
                DtsMesh& mesh, std::string* error) {
    const u32 typeWord = static_cast<u32>(s.i32());
    const u32 type = typeWord & kTypeMask;
    if (!s.ok()) {
        set_error(error, s.failure());
        return false;
    }
    if (type == static_cast<u32>(DtsMeshType::Null)) {
        mesh.type = DtsMeshType::Null;
        return true;
    }
    if (type == static_cast<u32>(DtsMeshType::Decal)) {
        set_error(error, "decal meshes are not supported");
        return false;
    }
    if (type == static_cast<u32>(DtsMeshType::Sorted)) {
        set_error(error, "sorted (translucency-ordered) meshes are not supported");
        return false;
    }
    if (type != static_cast<u32>(DtsMeshType::Standard) && type != static_cast<u32>(DtsMeshType::Skin)) {
        set_error(error, "unknown mesh type " + std::to_string(type));
        return false;
    }
    mesh.type = static_cast<DtsMeshType>(type);
    s.guard("mesh header");
    mesh.frames = s.count("mesh frame");
    (void)s.i32(); // material frames
    mesh.parent = s.i32();
    s.f(mesh.bounds_min, 3);
    s.f(mesh.bounds_max, 3);
    s.skip32(4); // centre + radius
    const bool own = mesh.parent < 0;

    u32 vbOffset = 0;
    u32 vbVerts = 0;
    u32 vbSize = 0;
    if (version >= 27) {
        vbOffset = s.count("vertex buffer offset");
        vbVerts = s.count("vertex buffer vertex");
        vbSize = s.count("vertex buffer stride");
    }
    const u32 verts = s.count("vertex", own ? 3u : 0u);
    if (own) {
        mesh.positions.resize(static_cast<usize>(verts) * 3u);
        s.f(mesh.positions.data(), mesh.positions.size());
    }
    const u32 tverts = s.count("uv", own ? 2u : 0u);
    if (own) {
        mesh.uvs.resize(static_cast<usize>(tverts) * 2u);
        s.f(mesh.uvs.data(), mesh.uvs.size());
    }
    if (version > 25) {
        const u32 tverts2 = s.count("uv1", own ? 2u : 0u);
        if (own) {
            s.skip32(static_cast<usize>(tverts2) * 2u);
        }
        const u32 colors = s.count("colour", own ? 1u : 0u);
        if (own) {
            s.skip32(colors);
        }
    }
    if (own) {
        mesh.normals.resize(static_cast<usize>(verts) * 3u);
        s.f(mesh.normals.data(), mesh.normals.size());
        s.skip8(verts); // encoded normals
    }
    const u32 primitives = s.count("primitive");
    mesh.primitives.resize(primitives);
    std::vector<u32> rawMaterial(primitives, 0u);
    if (version > 25) {
        for (u32 p = 0; p < primitives; ++p) {
            mesh.primitives[p].start = static_cast<u32>(s.i32());
            mesh.primitives[p].count = static_cast<u32>(s.i32());
            rawMaterial[p] = static_cast<u32>(s.i32());
        }
    } else {
        for (u32 p = 0; p < primitives; ++p) {
            mesh.primitives[p].start = static_cast<u16>(s.i16());
            mesh.primitives[p].count = static_cast<u16>(s.i16());
        }
        for (u32 p = 0; p < primitives; ++p) {
            rawMaterial[p] = static_cast<u32>(s.i32());
        }
    }
    for (u32 p = 0; p < primitives; ++p) {
        const u32 raw = rawMaterial[p];
        const u32 primType = raw >> kPrimTypeShift;
        if (primType > 2u) {
            set_error(error, "unknown primitive type " + std::to_string(primType));
            return false;
        }
        mesh.primitives[p].type = static_cast<DtsPrimitiveType>(primType);
        mesh.primitives[p].material = (raw & kPrimNoMaterial) != 0u ? -1 : static_cast<s32>(raw & kPrimMaterialMask);
    }
    const u32 indices = s.count("index");
    mesh.indices.resize(indices);
    for (u32 i = 0; i < indices; ++i) {
        mesh.indices[i] = version > 25 ? static_cast<u32>(s.i32()) : static_cast<u16>(s.i16());
    }
    const u32 merges = s.count("merge index");
    s.skip16(merges);
    mesh.verts_per_frame = s.count("verts per frame");
    (void)s.i32(); // flags
    s.guard("mesh");

    if (version >= 27 && own && vbVerts > 0u) {
        // v27+: positions / normals / uvs live in the shape's shared vertex buffer.
        if (vbSize < 48u || vbSize != static_cast<u32>(format.vertex_size) ||
            static_cast<u64>(vbOffset) + static_cast<u64>(vbVerts) * vbSize > vertexBytes) {
            set_error(error, "mesh vertex-buffer range outside the shape's vertex buffer");
            return false;
        }
        mesh.positions.resize(static_cast<usize>(vbVerts) * 3u);
        mesh.normals.resize(static_cast<usize>(vbVerts) * 3u);
        mesh.uvs.resize(static_cast<usize>(vbVerts) * 2u);
        for (u32 v = 0; v < vbVerts; ++v) {
            const u8* vertex = vertexData + vbOffset + static_cast<usize>(v) * vbSize;
            // position xyz, tangent w, normal xyz, tangent xyz, uv
            std::memcpy(&mesh.positions[v * 3u], vertex, 12u);
            std::memcpy(&mesh.normals[v * 3u], vertex + 16u, 12u);
            std::memcpy(&mesh.uvs[v * 2u], vertex + 40u, 8u);
        }
    }

    if (mesh.type == DtsMeshType::Skin) {
        if (version >= 27) {
            set_error(error, "skinned meshes in v27+ shapes are not supported");
            return false;
        }
        const u32 initial = s.count("skin vertex", own ? 6u : 0u);
        if (own) {
            mesh.positions.resize(static_cast<usize>(initial) * 3u);
            s.f(mesh.positions.data(), mesh.positions.size());
            mesh.normals.resize(static_cast<usize>(initial) * 3u);
            s.f(mesh.normals.data(), mesh.normals.size());
            s.skip8(initial);
        }
        const u32 transforms = s.count("skin transform", own ? 16u : 0u);
        if (own) {
            s.floats(mesh.skin_initial_transforms, static_cast<usize>(transforms) * 16u);
        }
        const u32 influences = s.count("skin influence", own ? 3u : 0u);
        if (own) {
            mesh.skin_vertex.resize(influences);
            mesh.skin_bone.resize(influences);
            mesh.skin_weight.resize(influences);
            for (u32 i = 0; i < influences; ++i) {
                mesh.skin_vertex[i] = static_cast<u32>(s.i32());
            }
            for (u32 i = 0; i < influences; ++i) {
                mesh.skin_bone[i] = static_cast<u32>(s.i32());
            }
            for (u32 i = 0; i < influences; ++i) {
                mesh.skin_weight[i] = s.f();
            }
        }
        const u32 bones = s.count("skin bone", own ? 1u : 0u);
        if (own) {
            mesh.bone_nodes.resize(bones);
            for (u32 i = 0; i < bones; ++i) {
                mesh.bone_nodes[i] = s.i32();
            }
        }
        s.guard("skin");
    }
    if (!s.ok()) {
        set_error(error, s.failure());
        return false;
    }
    return true;
}

/// Plain little-endian reader for the parts outside the memory buffer (sequences, material list, `.dsq`).
class ByteReader {
public:
    ByteReader(const u8* data, usize size, usize pos = 0) : m_data(data), m_size(size), m_pos(pos) {}
    bool ok() const { return m_ok; }
    usize pos() const { return m_pos; }
    usize remaining() const { return m_pos <= m_size ? m_size - m_pos : 0u; }
    bool has(usize bytes) {
        if (!m_ok || bytes > remaining()) {
            m_ok = false;
            return false;
        }
        return true;
    }
    s32 i32() {
        s32 v = 0;
        if (has(4u)) {
            std::memcpy(&v, m_data + m_pos, 4u);
            m_pos += 4u;
        }
        return v;
    }
    f32 f() {
        const s32 bits = i32();
        f32 v = 0.f;
        std::memcpy(&v, &bits, 4u);
        return v;
    }
    std::int16_t i16() {
        std::int16_t v = 0;
        if (has(2u)) {
            std::memcpy(&v, m_data + m_pos, 2u);
            m_pos += 2u;
        }
        return v;
    }
    u8 byte() {
        if (!has(1u)) {
            return 0u;
        }
        return m_data[m_pos++];
    }
    std::string bytes(usize n) {
        if (!has(n)) {
            return std::string();
        }
        std::string s(reinterpret_cast<const char*>(m_data + m_pos), n);
        m_pos += n;
        return s;
    }
    /// A count whose elements take at least `minBytes` each.
    u32 count(usize minBytes) {
        const s32 n = i32();
        if (!m_ok || n < 0 || static_cast<u32>(n) > kMaxElements ||
            (minBytes != 0u && static_cast<usize>(n) * minBytes > remaining())) {
            m_ok = false;
            return 0u;
        }
        return static_cast<u32>(n);
    }
    void quats(std::vector<DtsQuat16>& out, u32 n) {
        out.clear();
        if (!has(static_cast<usize>(n) * 8u)) {
            return;
        }
        out.resize(n);
        for (DtsQuat16& q : out) {
            for (std::int16_t& c : q) {
                c = i16();
            }
        }
    }
    void floats(std::vector<f32>& out, usize n) {
        out.clear();
        if (!has(n * 4u)) {
            return;
        }
        out.resize(n);
        for (f32& v : out) {
            v = f();
        }
    }

private:
    const u8* m_data;
    usize m_size;
    usize m_pos;
    bool m_ok = true;
};

void read_integer_set(ByteReader& r, DtsIntegerSet& set) {
    (void)r.i32(); // legacy element count, written as 0
    const u32 words = r.count(4u);
    set.words.resize(words);
    for (u32& w : set.words) {
        w = static_cast<u32>(r.i32());
    }
    while (!set.words.empty() && set.words.back() == 0u) {
        set.words.pop_back(); // trailing zero words carry no members
    }
}

/// One sequence record (the name index, when present, is read by the caller).
void read_sequence(ByteReader& r, DtsSequence& seq) {
    seq.flags = static_cast<u32>(r.i32());
    seq.keyframes = r.i32();
    seq.duration = r.f();
    seq.priority = r.i32();
    seq.first_ground_frame = r.i32();
    seq.ground_frames = r.i32();
    seq.base_rotation = r.i32();
    seq.base_translation = r.i32();
    seq.base_scale = r.i32();
    seq.base_object_state = r.i32();
    seq.base_decal_state = r.i32();
    seq.first_trigger = r.i32();
    seq.trigger_count = r.i32();
    seq.tool_begin = r.f();
    for (DtsIntegerSet* set : {&seq.rotation_matters, &seq.translation_matters, &seq.scale_matters, &seq.decal_matters,
                               &seq.ifl_matters, &seq.vis_matters, &seq.frame_matters, &seq.mat_frame_matters}) {
        read_integer_set(r, *set);
    }
}

/// Smallest byte size of a sequence record without its name (15 words + 8 empty sets of 2 words).
constexpr usize kMinSequenceBytes = 15u * 4u + 8u * 8u;

/// Index checks of every sequence against the node count and the pools.
bool validate_sequences(usize nodes, const DtsKeyframes& keys, const std::vector<DtsSequence>& sequences,
                        usize triggers, std::string* error) {
    auto within = [](s64 base, s64 count, usize size) {
        return base >= 0 && count >= 0 && static_cast<u64>(base) + static_cast<u64>(count) <= size;
    };
    for (const DtsSequence& seq : sequences) {
        const std::string where = "sequence '" + seq.name + "': ";
        if (seq.keyframes < 0 || !std::isfinite(seq.duration) || seq.duration < 0.f) {
            set_error(error, where + "bad keyframe count / duration");
            return false;
        }
        const s64 k = seq.keyframes;
        const bool animates = seq.rotation_matters.count() + seq.translation_matters.count() + seq.scale_matters.count() > 0u;
        if (animates && k == 0) {
            set_error(error, where + "animates nodes but has no keyframes");
            return false;
        }
        if (seq.rotation_matters.extent() > nodes || seq.translation_matters.extent() > nodes ||
            seq.scale_matters.extent() > nodes) {
            set_error(error, where + "node set past the node count");
            return false;
        }
        if (!within(seq.base_rotation, k * seq.rotation_matters.count(), keys.node_rotations.size()) ||
            !within(seq.base_translation, k * seq.translation_matters.count(), keys.node_translations.size() / 3u)) {
            set_error(error, where + "rotation / translation keys outside the pools");
            return false;
        }
        if (seq.scale_matters.count() > 0u) {
            usize pool = keys.node_uniform_scales.size();
            if ((seq.flags & kDtsSeqArbitraryScale) != 0u) {
                pool = std::min(keys.node_arbitrary_scale_factors.size() / 3u, keys.node_arbitrary_scale_rotations.size());
            } else if ((seq.flags & kDtsSeqAlignedScale) != 0u) {
                pool = keys.node_aligned_scales.size() / 3u;
            } else if ((seq.flags & kDtsSeqUniformScale) == 0u) {
                set_error(error, where + "scale set without a scale type flag");
                return false;
            }
            if (!within(seq.base_scale, k * seq.scale_matters.count(), pool)) {
                set_error(error, where + "scale keys outside the pool");
                return false;
            }
        }
        if (!within(seq.first_ground_frame, seq.ground_frames,
                    std::min(keys.ground_translations.size() / 3u, keys.ground_rotations.size())) ||
            !within(seq.first_trigger, seq.trigger_count, triggers)) {
            set_error(error, where + "ground frames / triggers outside the pools");
            return false;
        }
    }
    return true;
}

/// Material list after the sequences: u8 version, u32 count, count x (u8 length + bytes). The per-material flag /
/// map tables that follow are not needed.
void read_material_list(const u8* data, usize size, usize pos, std::vector<std::string>& out) {
    if (pos + 5u > size) {
        return;
    }
    ++pos; // list version
    u32 count = 0;
    std::memcpy(&count, data + pos, 4u);
    pos += 4u;
    if (count > 4096u) {
        return;
    }
    std::vector<std::string> names;
    for (u32 i = 0; i < count; ++i) {
        if (pos >= size) {
            return;
        }
        const usize len = data[pos++];
        if (pos + len > size) {
            return;
        }
        names.emplace_back(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
    }
    out = std::move(names);
}

// ---- conversion helpers ---------------------------------------------------------------------------------

struct Mat34 {
    f32 m[3][4] = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};
};

Mat34 node_local(const DtsNode& node) {
    // Unit quaternion -> rotation matrix (column vectors), translation in the last column.
    f32 x = node.rotation[0];
    f32 y = node.rotation[1];
    f32 z = node.rotation[2];
    f32 w = node.rotation[3];
    const f32 len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len > 0.f) {
        x /= len;
        y /= len;
        z /= len;
        w /= len;
    }
    Mat34 r;
    r.m[0][0] = 1.f - 2.f * (y * y + z * z);
    r.m[0][1] = 2.f * (x * y - w * z);
    r.m[0][2] = 2.f * (x * z + w * y);
    r.m[1][0] = 2.f * (x * y + w * z);
    r.m[1][1] = 1.f - 2.f * (x * x + z * z);
    r.m[1][2] = 2.f * (y * z - w * x);
    r.m[2][0] = 2.f * (x * z - w * y);
    r.m[2][1] = 2.f * (y * z + w * x);
    r.m[2][2] = 1.f - 2.f * (x * x + y * y);
    for (u32 i = 0; i < 3u; ++i) {
        r.m[i][3] = node.translation[i];
    }
    return r;
}

Mat34 mul(const Mat34& a, const Mat34& b) {
    Mat34 r;
    for (u32 i = 0; i < 3u; ++i) {
        for (u32 j = 0; j < 4u; ++j) {
            f32 v = j == 3u ? a.m[i][3] : 0.f;
            for (u32 k = 0; k < 3u; ++k) {
                v += a.m[i][k] * b.m[k][j];
            }
            r.m[i][j] = v;
        }
    }
    return r;
}

bool node_world(const DtsShape& shape, s32 node, Mat34& out) {
    Mat34 world;
    u32 depth = 0;
    while (node >= 0) {
        if (static_cast<usize>(node) >= shape.nodes.size() || ++depth > shape.nodes.size()) {
            return false; // out of range or a parent cycle
        }
        world = mul(node_local(shape.nodes[static_cast<usize>(node)]), world);
        node = shape.nodes[static_cast<usize>(node)].parent;
    }
    out = world;
    return true;
}

/// Torque (Z-up) -> FUSE (Y-up): p' = (x, z, -y).
void to_fuse(const f32 in[3], f32 out[3]) {
    const f32 x = in[0];
    const f32 y = in[1];
    const f32 z = in[2];
    out[0] = x;
    out[1] = z;
    out[2] = -y;
}

f32 tri_area2(const std::vector<f32>& p, u32 a, u32 b, u32 c) {
    const f32* A = &p[a * 3u];
    const f32* B = &p[b * 3u];
    const f32* C = &p[c * 3u];
    const f32 e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
    const f32 e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
    const f32 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    return n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
}

} // namespace

bool parse_dts(const u8* data, usize size, DtsShape& out, std::string* error) {
    out = DtsShape{};
    if (data == nullptr || size == 0u) {
        set_error(error, "empty .dts file");
        return false;
    }
    if (size < 16u) {
        set_error(error, "truncated .dts header");
        return false;
    }
    s32 header[4];
    std::memcpy(header, data, 16u);
    out.version = static_cast<u32>(header[0]) & 0xFFu;
    out.exporter_version = static_cast<u32>(header[0]) >> 16u;
    if (out.version < kDtsMinVersion || out.version > kDtsMaxVersion) {
        set_error(error, "unsupported .dts version " + std::to_string(out.version) + " (supported " +
                             std::to_string(kDtsMinVersion) + ".." + std::to_string(kDtsMaxVersion) + ")");
        return false;
    }
    const s32 words = header[1];
    const s32 start16 = header[2];
    const s32 start8 = header[3];
    if (words < 0 || start16 < 0 || start8 < start16 || start8 > words ||
        static_cast<u64>(words) * 4u > static_cast<u64>(size - 16u)) {
        set_error(error, "bad .dts memory-buffer header (words " + std::to_string(words) + ", 16-bit " +
                             std::to_string(start16) + ", 8-bit " + std::to_string(start8) + ")");
        return false;
    }
    const u8* buffer = data + 16u;
    Streams s(buffer, static_cast<usize>(words), static_cast<usize>(start16), static_cast<usize>(start8));
    const u32 version = out.version;

    const u32 nodes = s.count("node", 5u);
    const u32 objects = s.count("object", 6u);
    const u32 decals = s.count("decal", 5u);
    const u32 subshapes = s.count("sub-shape", 3u);
    const u32 ifls = s.count("IFL material", 5u);
    const u32 nodeRots = s.count("node rotation");
    const u32 nodeTrans = s.count("node translation", 3u);
    const u32 uniformScales = s.count("uniform scale", 1u);
    const u32 alignedScales = s.count("aligned scale", 3u);
    const u32 arbitraryScales = s.count("arbitrary scale", 3u);
    const u32 groundFrames = s.count("ground frame", 3u);
    const u32 objectStates = s.count("object state", 3u);
    const u32 decalStates = s.count("decal state", 1u);
    const u32 triggers = s.count("trigger", 2u);
    const u32 details = s.count("detail", 7u);
    const u32 meshes = s.count("mesh", 1u);
    const u32 names = s.count("name");
    s.skip32(2); // smallest visible size / detail level
    s.guard("counts");
    out.radius = s.f();
    s.skip32(4); // tube radius, centre
    s.f(out.bounds_min, 3);
    s.f(out.bounds_max, 3);
    s.guard("bounds");

    std::vector<s32> nodeName(nodes, -1);
    out.nodes.resize(nodes);
    for (u32 n = 0; n < nodes; ++n) {
        nodeName[n] = s.i32();
        out.nodes[n].parent = s.i32();
        s.skip32(3); // first object, first child, next sibling
    }
    s.guard("nodes");
    std::vector<s32> objectName(objects, -1);
    out.objects.resize(objects);
    for (u32 o = 0; o < objects; ++o) {
        objectName[o] = s.i32();
        out.objects[o].mesh_count = s.i32();
        out.objects[o].first_mesh = s.i32();
        out.objects[o].node = s.i32();
        s.skip32(2); // next sibling, first decal
    }
    s.guard("objects");
    s.skip32(static_cast<usize>(decals) * 5u);
    s.guard("decals");
    s.skip32(static_cast<usize>(ifls) * 5u);
    s.guard("IFL materials");
    s.skip32(static_cast<usize>(subshapes) * 3u); // first node / object / decal
    s.guard("sub-shape starts");
    s.skip32(static_cast<usize>(subshapes) * 3u); // node / object / decal counts
    s.guard("sub-shape counts");
    for (u32 n = 0; n < nodes; ++n) {
        for (u32 c = 0; c < 4u; ++c) {
            out.nodes[n].rotation[c] = static_cast<f32>(s.i16()) / 32767.f;
        }
    }
    for (u32 n = 0; n < nodes; ++n) {
        s.f(out.nodes[n].translation, 3);
    }
    DtsKeyframes& keys = out.keyframes;
    s.floats(keys.node_translations, static_cast<usize>(nodeTrans) * 3u);
    s.quats(keys.node_rotations, nodeRots);
    s.guard("node keys");
    s.floats(keys.node_uniform_scales, uniformScales);
    s.floats(keys.node_aligned_scales, static_cast<usize>(alignedScales) * 3u);
    s.floats(keys.node_arbitrary_scale_factors, static_cast<usize>(arbitraryScales) * 3u);
    s.quats(keys.node_arbitrary_scale_rotations, arbitraryScales);
    s.guard("node scale keys");
    s.floats(keys.ground_translations, static_cast<usize>(groundFrames) * 3u);
    s.quats(keys.ground_rotations, groundFrames);
    s.guard("ground frames");
    s.skip32(static_cast<usize>(objectStates) * 3u);
    s.guard("object states");
    s.skip32(decalStates);
    s.guard("decal states");
    out.triggers.resize(triggers);
    for (DtsTrigger& trigger : out.triggers) {
        trigger.state = static_cast<u32>(s.i32());
        trigger.pos = s.f();
    }
    s.guard("triggers");
    const u32 detailWords = version >= 26 ? 13u : 7u;
    std::vector<s32> detailName(details, -1);
    out.details.resize(details);
    for (u32 d = 0; d < details; ++d) {
        detailName[d] = s.i32();
        out.details[d].subshape = s.i32();
        out.details[d].object_detail = s.i32();
        out.details[d].size = s.f();
        s.skip32(2); // average / max error
        out.details[d].poly_count = s.i32();
        s.skip32(detailWords - 7u); // v26+ billboard settings
    }
    s.guard("details");

    const u8* vertexData = nullptr;
    usize vertexBytes = 0;
    VertexFormat format;
    if (version >= 27) {
        vertexBytes = s.count("vertex-buffer byte");
        format.texcoord_offset = s.i16();
        format.bone_offset = s.i16();
        format.color_offset = s.i16();
        format.bones = s.i16();
        format.vertex_size = s.i16();
        vertexData = s.take8(vertexBytes);
    }
    if (!s.ok()) {
        set_error(error, s.failure());
        return false;
    }

    out.meshes.resize(meshes);
    for (u32 m = 0; m < meshes; ++m) {
        std::string meshError;
        if (!parse_mesh(s, version, vertexData, vertexBytes, format, out.meshes[m], &meshError)) {
            set_error(error, "mesh " + std::to_string(m) + ": " + meshError);
            return false;
        }
    }
    s.guard("meshes");
    out.names.reserve(names);
    for (u32 n = 0; n < names && s.ok(); ++n) {
        out.names.push_back(s.cstring());
    }
    s.guard("names");
    if (!s.ok()) {
        set_error(error, s.failure());
        return false;
    }

    auto nameOf = [&](s32 index) { return index >= 0 && static_cast<usize>(index) < out.names.size() ? out.names[index] : std::string(); };
    for (u32 n = 0; n < nodes; ++n) {
        out.nodes[n].name = nameOf(nodeName[n]);
        if (out.nodes[n].parent >= static_cast<s32>(nodes) || out.nodes[n].parent < -1) {
            set_error(error, "node " + std::to_string(n) + " parent out of range");
            return false;
        }
    }
    for (u32 o = 0; o < objects; ++o) {
        DtsObject& object = out.objects[o];
        object.name = nameOf(objectName[o]);
        if (object.mesh_count < 0 || object.first_mesh < 0 ||
            static_cast<u64>(object.first_mesh) + static_cast<u64>(object.mesh_count) > meshes ||
            object.node >= static_cast<s32>(nodes) || object.node < -1) {
            set_error(error, "object '" + object.name + "' references meshes / node out of range");
            return false;
        }
    }
    for (u32 d = 0; d < details; ++d) {
        out.details[d].name = nameOf(detailName[d]);
    }
    for (u32 m = 0; m < meshes; ++m) {
        const DtsMesh& mesh = out.meshes[m];
        if (mesh.type == DtsMeshType::Null) {
            continue;
        }
        if (mesh.parent >= 0 && (static_cast<u32>(mesh.parent) >= m || out.meshes[mesh.parent].parent >= 0 ||
                                 out.meshes[mesh.parent].type == DtsMeshType::Null)) {
            set_error(error, "mesh " + std::to_string(m) + " parent mesh out of range");
            return false;
        }
    }

    // Sequences, then the material list. A shape image that ends with its memory buffer has neither.
    const usize tail = 16u + static_cast<usize>(words) * 4u;
    if (tail + 4u <= size) {
        ByteReader r(data, size, tail);
        const u32 sequences = r.count(kMinSequenceBytes + 4u);
        if (!r.ok()) {
            set_error(error, "bad sequence count");
            return false;
        }
        out.sequences.resize(sequences);
        for (DtsSequence& seq : out.sequences) {
            const s32 nameIndex = r.i32();
            read_sequence(r, seq);
            if (!r.ok()) {
                set_error(error, "truncated sequence record");
                return false;
            }
            seq.name = nameOf(nameIndex);
        }
        out.sequence_count = sequences;
        if (!validate_sequences(nodes, out.keyframes, out.sequences, out.triggers.size(), error)) {
            return false;
        }
        read_material_list(data, size, r.pos(), out.materials);
    }
    return true;
}

bool read_dts_file(const std::string& path, DtsShape& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cannot open '" + path + "'");
        return false;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_dts(bytes.data(), bytes.size(), out, error);
}

s32 dts_highest_visible_detail(const DtsShape& shape) {
    // Largest non-negative size (ties: the first).
    s32 best = -1;
    for (usize d = 0; d < shape.details.size(); ++d) {
        const DtsDetail& detail = shape.details[d];
        if (detail.size >= 0.f && (best < 0 || detail.size > shape.details[static_cast<usize>(best)].size)) {
            best = static_cast<s32>(d);
        }
    }
    return best;
}

bool dts_to_cooked_mesh(const DtsShape& shape, const MeshCookOptions& options, CookedMesh& out, std::string* error,
                        DtsImportStats* stats) {
    out = CookedMesh{};
    const s32 best = dts_highest_visible_detail(shape);
    if (best < 0) {
        set_error(error, "shape has no visible detail level");
        return false;
    }
    return dts_detail_to_cooked_mesh(shape, static_cast<u32>(best), options, out, error, stats);
}

bool dts_detail_to_cooked_mesh(const DtsShape& shape, u32 detailIndex, const MeshCookOptions& options, CookedMesh& out,
                               std::string* error, DtsImportStats* stats) {
    out = CookedMesh{};
    DtsImportStats local;
    if (detailIndex >= shape.details.size()) {
        set_error(error, "detail index out of range");
        return false;
    }
    local.detail = static_cast<s32>(detailIndex);
    const DtsDetail& detail = shape.details[detailIndex];

    struct Pick {
        u32 mesh;
        s32 node;
    };
    std::vector<Pick> picks;
    for (const DtsObject& object : shape.objects) {
        if (detail.object_detail < 0 || detail.object_detail >= object.mesh_count) {
            continue;
        }
        const u32 m = static_cast<u32>(object.first_mesh + detail.object_detail);
        if (shape.meshes[m].type == DtsMeshType::Null) {
            continue;
        }
        picks.push_back(Pick{m, object.node});
    }
    if (picks.empty()) {
        set_error(error, "detail '" + detail.name + "' has no meshes");
        return false;
    }
    bool allSkin = options.import_skin;
    for (const Pick& pick : picks) {
        allSkin = allSkin && shape.meshes[pick.mesh].type == DtsMeshType::Skin;
    }

    // Submesh per (material) in first-appearance order; triangles appended per mesh.
    std::map<s32, std::vector<u32>> trianglesByMaterial;
    std::vector<s32> materialOrder;
    for (const Pick& pick : picks) {
        const DtsMesh& mesh = shape.meshes[pick.mesh];
        const DtsMesh& data = mesh.parent >= 0 ? shape.meshes[static_cast<usize>(mesh.parent)] : mesh;
        const u32 storedVerts = static_cast<u32>(data.positions.size() / 3u);
        u32 frameVerts = mesh.verts_per_frame != 0u ? std::min(mesh.verts_per_frame, storedVerts) : storedVerts;
        if (mesh.type == DtsMeshType::Skin) {
            frameVerts = storedVerts;
        }
        if (frameVerts == 0u) {
            continue;
        }
        Mat34 world;
        if (mesh.type != DtsMeshType::Skin && !node_world(shape, pick.node, world)) {
            set_error(error, "mesh " + std::to_string(pick.mesh) + ": bad node chain");
            return false;
        }
        const u32 base = out.vertex_count();
        const bool hasNormals = data.normals.size() >= static_cast<usize>(frameVerts) * 3u;
        const bool hasUvs = data.uvs.size() >= static_cast<usize>(frameVerts) * 2u;
        for (u32 v = 0; v < frameVerts; ++v) {
            const f32* p = &data.positions[v * 3u];
            f32 tp[3];
            f32 tn[3] = {0.f, 0.f, 0.f};
            for (u32 r = 0; r < 3u; ++r) {
                tp[r] = world.m[r][0] * p[0] + world.m[r][1] * p[1] + world.m[r][2] * p[2] + world.m[r][3];
            }
            if (hasNormals) {
                const f32* n = &data.normals[v * 3u];
                for (u32 r = 0; r < 3u; ++r) {
                    tn[r] = world.m[r][0] * n[0] + world.m[r][1] * n[1] + world.m[r][2] * n[2];
                }
                const f32 len = std::sqrt(tn[0] * tn[0] + tn[1] * tn[1] + tn[2] * tn[2]);
                if (len > 0.f) {
                    tn[0] /= len;
                    tn[1] /= len;
                    tn[2] /= len;
                }
            }
            f32 fp[3];
            f32 fn[3];
            to_fuse(tp, fp);
            to_fuse(tn, fn);
            out.positions.insert(out.positions.end(), {fp[0], fp[1], fp[2]});
            out.normals.insert(out.normals.end(), {fn[0], fn[1], fn[2]});
            if (hasUvs) {
                out.uvs.insert(out.uvs.end(), {data.uvs[v * 2u], 1.f - data.uvs[v * 2u + 1u]});
            } else {
                out.uvs.insert(out.uvs.end(), {0.f, 0.f});
            }
        }
        if (allSkin) {
            // Up to 4 largest influences per vertex (ties by joint), renormalised; joints = shape node indices.
            std::vector<std::vector<std::pair<f32, u16>>> influences(frameVerts);
            for (usize i = 0; i < data.skin_vertex.size(); ++i) {
                const u32 v = data.skin_vertex[i];
                const u32 bone = data.skin_bone[i];
                const f32 w = data.skin_weight[i];
                if (v >= frameVerts || bone >= data.bone_nodes.size() || data.bone_nodes[bone] < 0 ||
                    static_cast<usize>(data.bone_nodes[bone]) >= shape.nodes.size() || !std::isfinite(w) || w < 0.f) {
                    set_error(error, "mesh " + std::to_string(pick.mesh) + ": invalid skin influence " + std::to_string(i));
                    return false;
                }
                if (w > 0.f) {
                    influences[v].emplace_back(w, static_cast<u16>(data.bone_nodes[bone]));
                }
            }
            for (u32 v = 0; v < frameVerts; ++v) {
                auto& list = influences[v];
                std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                    return a.first != b.first ? a.first > b.first : a.second < b.second;
                });
                if (list.size() > 4u) {
                    list.resize(4u);
                }
                f32 sum = 0.f;
                for (const auto& e : list) {
                    sum += e.first;
                }
                if (list.empty() || sum <= 0.f) {
                    set_error(error, "mesh " + std::to_string(pick.mesh) + ": skinned vertex " + std::to_string(v) +
                                         " has no weights");
                    return false;
                }
                for (u32 k = 0; k < 4u; ++k) {
                    out.joints.push_back(k < list.size() ? list[k].second : static_cast<u16>(0));
                    out.weights.push_back(k < list.size() ? list[k].first / sum : 0.f);
                }
            }
        }
        // Primitives -> triangle lists: reversed winding (clockwise Torque -> counter-clockwise FUSE).
        for (const DtsPrimitive& prim : mesh.primitives) {
            if (static_cast<u64>(prim.start) + prim.count > mesh.indices.size()) {
                set_error(error, "mesh " + std::to_string(pick.mesh) + ": primitive outside the index list");
                return false;
            }
            if (!trianglesByMaterial.contains(prim.material)) {
                materialOrder.push_back(prim.material);
            }
            std::vector<u32>& list = trianglesByMaterial[prim.material];
            auto emit = [&](u32 a, u32 b, u32 c, bool stitched) -> bool {
                if (a >= frameVerts || b >= frameVerts || c >= frameVerts) {
                    set_error(error, "mesh " + std::to_string(pick.mesh) + ": index out of range (vertices=" +
                                         std::to_string(frameVerts) + ")");
                    return false;
                }
                if (stitched && (a == b || b == c || a == c || tri_area2(out.positions, base + a, base + b, base + c) == 0.f)) {
                    ++local.dropped_stitch;
                    return true;
                }
                list.insert(list.end(), {base + a, base + c, base + b});
                return true;
            };
            const u32* idx = mesh.indices.data() + prim.start;
            if (prim.type == DtsPrimitiveType::Triangles) {
                if (prim.count % 3u != 0u) {
                    set_error(error, "mesh " + std::to_string(pick.mesh) + ": triangle list not a multiple of 3");
                    return false;
                }
                for (u32 i = 0; i + 2u < prim.count; i += 3u) {
                    if (!emit(idx[i], idx[i + 1u], idx[i + 2u], false)) {
                        return false;
                    }
                }
            } else if (prim.type == DtsPrimitiveType::Strip) {
                for (u32 i = 0; i + 2u < prim.count; ++i) {
                    const bool odd = (i & 1u) != 0u;
                    if (!emit(odd ? idx[i + 1u] : idx[i], odd ? idx[i] : idx[i + 1u], idx[i + 2u], true)) {
                        return false;
                    }
                }
            } else {
                for (u32 i = 1; i + 1u < prim.count; ++i) {
                    if (!emit(idx[0], idx[i], idx[i + 1u], true)) {
                        return false;
                    }
                }
            }
        }
        ++local.meshes;
    }
    for (s32 material : materialOrder) {
        const std::vector<u32>& list = trianglesByMaterial[material];
        if (list.empty()) {
            continue;
        }
        CookedMesh::Submesh sub;
        sub.index_offset = static_cast<u32>(out.indices.size());
        sub.index_count = static_cast<u32>(list.size());
        sub.vertex_offset = 0;
        sub.material_index = material < 0 ? 0u : static_cast<u32>(material);
        out.indices.insert(out.indices.end(), list.begin(), list.end());
        out.submeshes.push_back(sub);
        if (options.import_material_names) {
            out.material_slots.push_back(material >= 0 && static_cast<usize>(material) < shape.materials.size()
                                             ? shape.materials[static_cast<usize>(material)]
                                             : std::string());
        }
    }
    if (out.indices.empty()) {
        set_error(error, "shape produced no triangles");
        out = CookedMesh{};
        return false;
    }
    // Normals: generated (area-weighted) only where the source has none.
    bool missingNormals = false;
    for (usize v = 0; v < out.normals.size(); v += 3u) {
        if (out.normals[v] == 0.f && out.normals[v + 1u] == 0.f && out.normals[v + 2u] == 0.f) {
            missingNormals = true;
            break;
        }
    }
    if (missingNormals && options.generate_normals) {
        std::vector<f32> acc(out.normals.size(), 0.f);
        for (usize t = 0; t + 2u < out.indices.size(); t += 3u) {
            const u32 a = out.indices[t];
            const u32 b = out.indices[t + 1u];
            const u32 c = out.indices[t + 2u];
            const f32* A = &out.positions[a * 3u];
            const f32* B = &out.positions[b * 3u];
            const f32* C = &out.positions[c * 3u];
            const f32 e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
            const f32 e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
            const f32 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            for (u32 corner : {a, b, c}) {
                for (u32 k = 0; k < 3u; ++k) {
                    acc[corner * 3u + k] += n[k];
                }
            }
        }
        for (usize v = 0; v < out.normals.size(); v += 3u) {
            if (out.normals[v] != 0.f || out.normals[v + 1u] != 0.f || out.normals[v + 2u] != 0.f) {
                continue;
            }
            const f32 len = std::sqrt(acc[v] * acc[v] + acc[v + 1u] * acc[v + 1u] + acc[v + 2u] * acc[v + 2u]);
            if (len > 0.f) {
                out.normals[v] = acc[v] / len;
                out.normals[v + 1u] = acc[v + 1u] / len;
                out.normals[v + 2u] = acc[v + 2u] / len;
            }
        }
    }
    for (f32 value : out.positions) {
        if (!std::isfinite(value)) {
            set_error(error, "non-finite vertex position");
            out = CookedMesh{};
            return false;
        }
    }
    for (const std::vector<f32>* stream : {&out.normals, &out.uvs}) {
        for (f32 value : *stream) {
            if (!std::isfinite(value)) {
                set_error(error, "non-finite vertex normal / uv");
                out = CookedMesh{};
                return false;
            }
        }
    }
    const u32 count = out.vertex_count();
    for (u32 axis = 0; axis < 3u; ++axis) {
        out.bounds_min[axis] = out.positions[axis];
        out.bounds_max[axis] = out.positions[axis];
    }
    for (u32 v = 0; v < count; ++v) {
        for (u32 axis = 0; axis < 3u; ++axis) {
            out.bounds_min[axis] = std::min(out.bounds_min[axis], out.positions[v * 3u + axis]);
            out.bounds_max[axis] = std::max(out.bounds_max[axis], out.positions[v * 3u + axis]);
        }
    }
    local.triangles = static_cast<u32>(out.indices.size() / 3u);
    if (stats != nullptr) {
        *stats = local;
    }
    return true;
}

bool import_dts_file(const std::string& path, const MeshCookOptions& options, CookedMesh& out, std::string* error,
                     CookFailure* failure, DtsImportStats* stats) {
    out = CookedMesh{};
    auto fail = [&](CookFailure kind, const std::string& message) {
        set_error(error, message);
        if (failure != nullptr) {
            *failure = kind;
        }
        return false;
    };
    if (failure != nullptr) {
        *failure = CookFailure::None;
    }
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(std::filesystem::path(path), ec)) {
        return fail(CookFailure::InvalidArgument, "source file not found");
    }
    DtsShape shape;
    std::string why;
    if (!read_dts_file(path, shape, &why)) {
        return fail(CookFailure::MalformedSource, "dts import failed: " + why);
    }
    if (!dts_to_cooked_mesh(shape, options, out, &why, stats)) {
        return fail(CookFailure::InvalidGeometry, "dts import rejected: " + why);
    }
    return true;
}

// ---- integer sets ---------------------------------------------------------------------------------------

u32 DtsIntegerSet::rank(u32 i) const {
    u32 n = 0;
    for (u32 w = 0; w < words.size() && w * 32u < i; ++w) {
        u32 bits = words[w];
        if ((w + 1u) * 32u > i) {
            bits &= (1u << (i % 32u)) - 1u;
        }
        n += static_cast<u32>(std::popcount(bits));
    }
    return n;
}

u32 DtsIntegerSet::count() const {
    u32 n = 0;
    for (u32 w : words) {
        n += static_cast<u32>(std::popcount(w));
    }
    return n;
}

u32 DtsIntegerSet::extent() const {
    for (usize w = words.size(); w > 0u; --w) {
        if (words[w - 1u] != 0u) {
            return static_cast<u32>((w - 1u) * 32u + 32u - static_cast<u32>(std::countl_zero(words[w - 1u])));
        }
    }
    return 0u;
}

// ---- .dsq ----------------------------------------------------------------------------------------------

bool parse_dsq(const u8* data, usize size, DsqFile& out, std::string* error) {
    out = DsqFile{};
    if (data == nullptr || size == 0u) {
        set_error(error, "empty .dsq file");
        return false;
    }
    ByteReader r(data, size);
    const s32 versionWord = r.i32();
    out.version = static_cast<u32>(versionWord) & 0xFFu;
    if (!r.ok() || out.version < kDsqMinVersion || out.version > kDsqMaxVersion) {
        set_error(error, "unsupported .dsq version " + std::to_string(out.version) + " (supported " +
                             std::to_string(kDsqMinVersion) + ".." + std::to_string(kDsqMaxVersion) + ")");
        return false;
    }
    auto name = [&r]() {
        const u32 length = r.count(1u);
        return r.bytes(length);
    };
    const u32 nodes = r.count(4u);
    out.nodes.reserve(nodes);
    for (u32 n = 0; n < nodes && r.ok(); ++n) {
        out.nodes.push_back(name());
    }
    const u32 legacyObjects = r.count(4u);
    for (u32 o = 0; o < legacyObjects && r.ok(); ++o) {
        (void)name();
    }
    out.shape_object_count = r.i32();
    DtsKeyframes& keys = out.keyframes;
    r.quats(keys.node_rotations, r.count(8u));
    r.floats(keys.node_translations, static_cast<usize>(r.count(12u)) * 3u);
    r.floats(keys.node_uniform_scales, r.count(4u));
    r.floats(keys.node_aligned_scales, static_cast<usize>(r.count(12u)) * 3u);
    const u32 arbitrary = r.count(20u);
    r.quats(keys.node_arbitrary_scale_rotations, arbitrary);
    r.floats(keys.node_arbitrary_scale_factors, static_cast<usize>(arbitrary) * 3u);
    const u32 ground = r.count(20u);
    r.floats(keys.ground_translations, static_cast<usize>(ground) * 3u);
    r.quats(keys.ground_rotations, ground);
    const u32 objectStates = r.count(12u);
    if (r.has(static_cast<usize>(objectStates) * 12u)) {
        (void)r.bytes(static_cast<usize>(objectStates) * 12u);
    }
    if (!r.ok()) {
        set_error(error, "truncated .dsq header / keyframe pools");
        return false;
    }
    const u32 sequences = r.count(kMinSequenceBytes + 4u);
    out.sequences.resize(sequences);
    for (DtsSequence& seq : out.sequences) {
        seq.name = name();
        read_sequence(r, seq);
        if (!r.ok()) {
            set_error(error, "truncated .dsq sequence record");
            return false;
        }
    }
    const u32 triggers = r.count(8u);
    out.triggers.resize(triggers);
    for (DtsTrigger& trigger : out.triggers) {
        trigger.state = static_cast<u32>(r.i32());
        trigger.pos = r.f();
    }
    if (!r.ok()) {
        set_error(error, "truncated .dsq triggers");
        return false;
    }
    if (r.remaining() != 0u) {
        set_error(error, std::to_string(r.remaining()) + " trailing bytes after the .dsq triggers");
        return false;
    }
    return validate_sequences(out.nodes.size(), out.keyframes, out.sequences, out.triggers.size(), error);
}

bool read_dsq_file(const std::string& path, DsqFile& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cannot open '" + path + "'");
        return false;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_dsq(bytes.data(), bytes.size(), out, error);
}

// ---- skeleton + sequences -> .fuseanim -----------------------------------------------------------------

std::vector<u32> dts_joint_order(const DtsShape& shape) {
    const u32 count = static_cast<u32>(shape.nodes.size());
    std::vector<u32> order(count);
    bool parentsFirst = true;
    for (u32 n = 0; n < count; ++n) {
        order[n] = n;
        const s32 parent = shape.nodes[n].parent;
        parentsFirst = parentsFirst && parent < static_cast<s32>(n);
    }
    if (parentsFirst) {
        return order;
    }
    order.clear();
    std::vector<u8> placed(count, 0u);
    // Depth-first pre-order: roots in node order, children in node order (explicit stack, cycle-safe).
    std::vector<u32> stack;
    for (u32 root = 0; root < count; ++root) {
        if (shape.nodes[root].parent >= 0 && static_cast<u32>(shape.nodes[root].parent) < count) {
            continue;
        }
        stack.push_back(root);
        while (!stack.empty()) {
            const u32 n = stack.back();
            stack.pop_back();
            if (placed[n] != 0u) {
                continue;
            }
            placed[n] = 1u;
            order.push_back(n);
            for (u32 c = count; c > 0u; --c) {
                if (shape.nodes[c - 1u].parent == static_cast<s32>(n) && placed[c - 1u] == 0u) {
                    stack.push_back(c - 1u);
                }
            }
        }
    }
    for (u32 n = 0; n < count; ++n) {
        if (placed[n] == 0u) {
            order.push_back(n); // part of a parent cycle (parse_dts does not reject those): appended as is
        }
    }
    return order;
}

void dts_skeleton(const DtsShape& shape, std::vector<std::string>& names, std::vector<s32>& parents) {
    const std::vector<u32> order = dts_joint_order(shape);
    std::vector<s32> jointOf(shape.nodes.size(), -1);
    for (u32 j = 0; j < order.size(); ++j) {
        jointOf[order[j]] = static_cast<s32>(j);
    }
    names.clear();
    parents.clear();
    for (u32 j = 0; j < order.size(); ++j) {
        const DtsNode& node = shape.nodes[order[j]];
        names.push_back(node.name);
        const s32 parent = node.parent >= 0 && static_cast<usize>(node.parent) < jointOf.size() ? jointOf[node.parent] : -1;
        parents.push_back(parent < static_cast<s32>(j) ? parent : -1);
    }
}

namespace {

namespace an = fuse::animation;

an::quat decode_quat(const DtsQuat16& q) {
    an::quat out{static_cast<f32>(q[0]) / 32767.f, static_cast<f32>(q[1]) / 32767.f, static_cast<f32>(q[2]) / 32767.f,
                 static_cast<f32>(q[3]) / 32767.f};
    const f32 len = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
    if (!(len > 1e-6f)) {
        return an::quat{0.f, 0.f, 0.f, 1.f};
    }
    return an::quat{out.x / len, out.y / len, out.z / len, out.w / len};
}

/// Torque Z-up -> FUSE Y-up for a rotation: the axis maps like a vector (proper rotation, so the angle is kept).
an::quat quat_to_fuse(const an::quat& q) { return an::quat{q.x, q.z, -q.y, q.w}; }
an::vec3 vec_to_fuse(f32 x, f32 y, f32 z) { return an::vec3{x, z, -y, 0.f}; }

/// Keyframe bracket at sequence position `pos` (0..1): Torque's spacing (cyclic sequences wrap to key 0).
void key_bracket(f32 pos, s32 keyframes, bool cyclic, u32& k0, u32& k1, f32& frac) {
    k0 = k1 = 0u;
    frac = 0.f;
    if (keyframes <= 1) {
        return;
    }
    const u32 n = static_cast<u32>(keyframes);
    const f64 kf = static_cast<f64>(pos) * static_cast<f64>(cyclic ? n : n - 1u);
    const f64 base = std::floor(kf);
    frac = static_cast<f32>(kf - base);
    if (cyclic) {
        k0 = static_cast<u32>(base) % n;
        k1 = (k0 + 1u) % n;
    } else {
        k0 = std::min(static_cast<u32>(base), n - 1u);
        k1 = std::min(k0 + 1u, n - 1u);
        if (k0 == n - 1u) {
            frac = 0.f;
        }
    }
}

/// The keyframe pools + sequence -> tracks for `jointOfSetNode` (set element -> joint, -1 = skipped).
bool sequence_to_anim(const DtsKeyframes& keys, const std::vector<DtsTrigger>& triggers, const DtsSequence& seq,
                      const std::vector<s32>& jointOfSetNode, const std::vector<std::string>& jointNames,
                      const std::vector<s32>& jointParents, const DtsAnimOptions& options, an::CookedAnim& out,
                      std::string* error) {
    out = an::CookedAnim{};
    if (!(options.sample_rate > 0.f) || options.sample_rate > 1000.f) {
        set_error(error, "sample_rate must be in (0, 1000] Hz");
        return false;
    }
    out.name = seq.name;
    out.sample_rate = options.sample_rate;
    out.duration = seq.duration;
    out.looping = seq.cyclic();
    out.frame_count = an::cooked_anim_frame_count(seq.duration, options.sample_rate);
    out.joints.resize(jointNames.size());
    for (usize j = 0; j < jointNames.size(); ++j) {
        out.joints[j].name = jointNames[j];
        out.joints[j].parent = jointParents[j];
    }
    auto posOf = [&](u32 frame) {
        const f32 t = out.frame_time(frame);
        return seq.duration > 0.f ? std::clamp(t / seq.duration, 0.f, 1.f) : 0.f;
    };
    const u32 k = static_cast<u32>(std::max(seq.keyframes, 0));
    const u32 extent = std::max({seq.rotation_matters.extent(), seq.translation_matters.extent(), seq.scale_matters.extent()});
    for (u32 node = 0; node < extent; ++node) {
        const s32 joint = node < jointOfSetNode.size() ? jointOfSetNode[node] : -1;
        if (joint < 0) {
            continue;
        }
        an::CookedAnimJoint& target = out.joints[static_cast<usize>(joint)];
        if (seq.rotation_matters.test(node)) {
            const usize base = static_cast<usize>(seq.base_rotation) + seq.rotation_matters.rank(node) * static_cast<usize>(k);
            target.rotation.resize(out.frame_count);
            for (u32 f = 0; f < out.frame_count; ++f) {
                u32 k0 = 0, k1 = 0;
                f32 a = 0.f;
                key_bracket(posOf(f), seq.keyframes, seq.cyclic(), k0, k1, a);
                const an::quat q0 = decode_quat(keys.node_rotations[base + k0]);
                const an::quat q = a > 0.f ? an::quat_normalize(an::lerp(q0, decode_quat(keys.node_rotations[base + k1]), a)) : q0;
                target.rotation[f] = quat_to_fuse(q);
            }
        }
        if (seq.translation_matters.test(node)) {
            const usize base =
                static_cast<usize>(seq.base_translation) + seq.translation_matters.rank(node) * static_cast<usize>(k);
            target.translation.resize(out.frame_count);
            for (u32 f = 0; f < out.frame_count; ++f) {
                u32 k0 = 0, k1 = 0;
                f32 a = 0.f;
                key_bracket(posOf(f), seq.keyframes, seq.cyclic(), k0, k1, a);
                const f32* p0 = &keys.node_translations[(base + k0) * 3u];
                const f32* p1 = &keys.node_translations[(base + k1) * 3u];
                target.translation[f] = vec_to_fuse(p0[0] + (p1[0] - p0[0]) * a, p0[1] + (p1[1] - p0[1]) * a,
                                                    p0[2] + (p1[2] - p0[2]) * a);
            }
        }
        if (seq.scale_matters.test(node)) {
            const usize base = static_cast<usize>(seq.base_scale) + seq.scale_matters.rank(node) * static_cast<usize>(k);
            target.scale.resize(out.frame_count);
            for (u32 f = 0; f < out.frame_count; ++f) {
                u32 k0 = 0, k1 = 0;
                f32 a = 0.f;
                key_bracket(posOf(f), seq.keyframes, seq.cyclic(), k0, k1, a);
                f32 s0[3];
                f32 s1[3];
                if ((seq.flags & kDtsSeqArbitraryScale) != 0u) {
                    std::memcpy(s0, &keys.node_arbitrary_scale_factors[(base + k0) * 3u], sizeof(s0));
                    std::memcpy(s1, &keys.node_arbitrary_scale_factors[(base + k1) * 3u], sizeof(s1));
                } else if ((seq.flags & kDtsSeqAlignedScale) != 0u) {
                    std::memcpy(s0, &keys.node_aligned_scales[(base + k0) * 3u], sizeof(s0));
                    std::memcpy(s1, &keys.node_aligned_scales[(base + k1) * 3u], sizeof(s1));
                } else {
                    s0[0] = s0[1] = s0[2] = keys.node_uniform_scales[base + k0];
                    s1[0] = s1[1] = s1[2] = keys.node_uniform_scales[base + k1];
                }
                // Scales are magnitudes along the axes: y / z swap (no sign).
                target.scale[f] = an::vec3{s0[0] + (s1[0] - s0[0]) * a, s0[2] + (s1[2] - s0[2]) * a,
                                           s0[1] + (s1[1] - s0[1]) * a, 0.f};
            }
        }
    }
    // Ground frames -> root motion of joint 0 (identity at pos 0, frame g at pos g / n).
    if (options.root_motion && seq.ground_frames > 0 && !out.joints.empty()) {
        const u32 n = static_cast<u32>(seq.ground_frames);
        const usize first = static_cast<usize>(seq.first_ground_frame);
        auto groundAt = [&](u32 g, an::vec3& t, an::quat& q) {
            if (g == 0u) {
                t = an::vec3{0.f, 0.f, 0.f, 0.f};
                q = an::quat{0.f, 0.f, 0.f, 1.f};
                return;
            }
            const f32* p = &keys.ground_translations[(first + g - 1u) * 3u];
            t = vec_to_fuse(p[0], p[1], p[2]);
            q = quat_to_fuse(decode_quat(keys.ground_rotations[first + g - 1u]));
        };
        out.has_root_motion = true;
        out.root_joint = 0u;
        out.root_motion.resize(out.frame_count);
        f64 yawAccum = 0.0;
        f32 prevYaw = 0.f;
        for (u32 f = 0; f < out.frame_count; ++f) {
            const f64 g = static_cast<f64>(posOf(f)) * n;
            const u32 g0 = std::min(static_cast<u32>(std::floor(g)), n);
            const u32 g1 = std::min(g0 + 1u, n);
            const f32 a = g0 == n ? 0.f : static_cast<f32>(g - g0);
            an::vec3 t0, t1;
            an::quat q0, q1;
            groundAt(g0, t0, q0);
            groundAt(g1, t1, q1);
            const an::vec3 t = an::lerp(t0, t1, a);
            const an::quat q = an::quat_normalize(an::lerp(q0, q1, a));
            const an::vec3 fwd = an::quat_rotate(q, an::vec3{0.f, 0.f, 1.f, 0.f});
            const f32 yaw = static_cast<f32>(std::atan2(static_cast<f64>(fwd.x), static_cast<f64>(fwd.z)));
            f64 d = static_cast<f64>(yaw) - prevYaw;
            while (d > 3.14159265358979) {
                d -= 6.28318530717959;
            }
            while (d < -3.14159265358979) {
                d += 6.28318530717959;
            }
            yawAccum += f == 0u ? static_cast<f64>(yaw) : d;
            prevYaw = yaw;
            out.root_motion[f] = an::CookedAnimRootSample{t.x, t.y, t.z, static_cast<f32>(yawAccum)};
        }
    }
    // Triggers -> events.
    for (s32 i = 0; i < seq.trigger_count; ++i) {
        const DtsTrigger& trigger = triggers[static_cast<usize>(seq.first_trigger + i)];
        const u32 bits = trigger.state & kDtsTriggerStateMask;
        const u32 number = bits == 0u ? 0u : static_cast<u32>(std::countr_zero(bits)) + 1u;
        an::CookedAnimEvent event;
        event.name = "trigger" + std::to_string(number) + ((trigger.state & kDtsTriggerOn) != 0u ? "" : "_off");
        event.time = std::clamp(trigger.pos, 0.f, 1.f) * seq.duration;
        out.events.push_back(event);
    }
    std::sort(out.events.begin(), out.events.end(), [](const an::CookedAnimEvent& a, const an::CookedAnimEvent& b) {
        return a.time != b.time ? a.time < b.time : a.name < b.name;
    });
    return true;
}

bool iequals_ascii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool dts_sequence_to_cooked_anim(const DtsShape& shape, u32 sequence, const DtsAnimOptions& options,
                                 animation::CookedAnim& out, std::string* error) {
    out = animation::CookedAnim{};
    if (sequence >= shape.sequences.size()) {
        set_error(error, "sequence index " + std::to_string(sequence) + " out of range (" +
                             std::to_string(shape.sequences.size()) + " sequences)");
        return false;
    }
    std::vector<std::string> names;
    std::vector<s32> parents;
    dts_skeleton(shape, names, parents);
    const std::vector<u32> order = dts_joint_order(shape);
    std::vector<s32> jointOfNode(shape.nodes.size(), -1);
    for (u32 j = 0; j < order.size(); ++j) {
        jointOfNode[order[j]] = static_cast<s32>(j);
    }
    return sequence_to_anim(shape.keyframes, shape.triggers, shape.sequences[sequence], jointOfNode, names, parents, options,
                            out, error);
}

bool dsq_sequence_to_cooked_anim(const DsqFile& dsq, u32 sequence, const DtsShape* shape, const DtsAnimOptions& options,
                                 animation::CookedAnim& out, std::string* error, u32* unmatched) {
    out = animation::CookedAnim{};
    if (unmatched != nullptr) {
        *unmatched = 0u;
    }
    if (sequence >= dsq.sequences.size()) {
        set_error(error, "sequence index " + std::to_string(sequence) + " out of range (" +
                             std::to_string(dsq.sequences.size()) + " sequences)");
        return false;
    }
    std::vector<std::string> names;
    std::vector<s32> parents;
    std::vector<s32> jointOfDsqNode(dsq.nodes.size(), -1);
    if (shape == nullptr) {
        names = dsq.nodes;
        parents.assign(names.size(), -1);
        for (usize n = 0; n < dsq.nodes.size(); ++n) {
            jointOfDsqNode[n] = static_cast<s32>(n);
        }
    } else {
        dts_skeleton(*shape, names, parents);
        u32 missing = 0;
        for (usize n = 0; n < dsq.nodes.size(); ++n) {
            for (usize j = 0; j < names.size(); ++j) {
                if (iequals_ascii(dsq.nodes[n], names[j])) {
                    jointOfDsqNode[n] = static_cast<s32>(j);
                    break;
                }
            }
            const DtsSequence& seq = dsq.sequences[sequence];
            const u32 node = static_cast<u32>(n);
            if (jointOfDsqNode[n] < 0 &&
                (seq.rotation_matters.test(node) || seq.translation_matters.test(node) || seq.scale_matters.test(node))) {
                ++missing;
            }
        }
        if (unmatched != nullptr) {
            *unmatched = missing;
        }
    }
    return sequence_to_anim(dsq.keyframes, dsq.triggers, dsq.sequences[sequence], jointOfDsqNode, names, parents, options,
                            out, error);
}

} // namespace fuse::cook
