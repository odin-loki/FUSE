// Asset plan 4a (L-DTS): clean-room Torque `.dts` reader + CookedMesh conversion. Format notes: dts_import.hpp.

#include <fuse/cook/dts_import.hpp>

#include <algorithm>
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
            s.skip32(static_cast<usize>(transforms) * 16u);
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

/// Material list after the sequences (only reached when there are none): u8 version, u32 count, count x
/// (u8 length + bytes). The per-material flag / map tables that follow are not needed.
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
    s.skip32(static_cast<usize>(nodeTrans) * 3u);
    s.skip16(static_cast<usize>(nodeRots) * 4u);
    s.guard("node keys");
    s.skip32(uniformScales);
    s.skip32(static_cast<usize>(alignedScales) * 3u);
    s.skip32(static_cast<usize>(arbitraryScales) * 3u);
    s.skip16(static_cast<usize>(arbitraryScales) * 4u);
    s.guard("node scale keys");
    s.skip32(static_cast<usize>(groundFrames) * 3u);
    s.skip16(static_cast<usize>(groundFrames) * 4u);
    s.guard("ground frames");
    s.skip32(static_cast<usize>(objectStates) * 3u);
    s.guard("object states");
    s.skip32(decalStates);
    s.guard("decal states");
    s.skip32(static_cast<usize>(triggers) * 2u);
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

    // Sequences, then the material list (names only when there are no sequences to step over).
    const usize tail = 16u + static_cast<usize>(words) * 4u;
    if (tail + 4u <= size) {
        s32 sequences = 0;
        std::memcpy(&sequences, data + tail, 4u);
        out.sequence_count = sequences > 0 ? static_cast<u32>(sequences) : 0u;
        if (sequences == 0) {
            read_material_list(data, size, tail + 4u, out.materials);
        }
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

bool dts_to_cooked_mesh(const DtsShape& shape, const MeshCookOptions& options, CookedMesh& out, std::string* error,
                        DtsImportStats* stats) {
    out = CookedMesh{};
    DtsImportStats local;
    // Highest visible detail: largest non-negative size (ties: the first).
    s32 best = -1;
    for (usize d = 0; d < shape.details.size(); ++d) {
        const DtsDetail& detail = shape.details[d];
        if (detail.size >= 0.f && (best < 0 || detail.size > shape.details[static_cast<usize>(best)].size)) {
            best = static_cast<s32>(d);
        }
    }
    if (best < 0) {
        set_error(error, "shape has no visible detail level");
        return false;
    }
    local.detail = best;
    const DtsDetail& detail = shape.details[static_cast<usize>(best)];

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

} // namespace fuse::cook
