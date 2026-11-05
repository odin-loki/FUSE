// Remaster W1.1: clean-room `.dts` / `.dsq` writer (inverse of dts_import.cpp). Layout notes: dts_import.hpp;
// derived fields: dts_write.hpp.

#include <fuse/cook/dts_write.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace fuse::cook {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

constexpr u32 kPrimIndexed = 1u << 29u;
constexpr u32 kPrimNoMaterial = 1u << 28u;

/// The three lockstep streams of the memory buffer.
class StreamWriter {
public:
    void i32(s32 v) { put(m32, &v, 4u); }
    void u32v(u32 v) { put(m32, &v, 4u); }
    void f(f32 v) { put(m32, &v, 4u); }
    void f(const f32* v, usize n) {
        for (usize i = 0; i < n; ++i) {
            f(v[i]);
        }
    }
    void i16(std::int16_t v) { put(m16, &v, 2u); }
    void i8(std::int8_t v) { m8.push_back(static_cast<u8>(v)); }
    void quat(const DtsQuat16& q) {
        for (std::int16_t c : q) {
            i16(c);
        }
    }
    void cstring(const std::string& s) {
        for (char c : s) {
            m8.push_back(static_cast<u8>(c));
        }
        m8.push_back(0u);
    }
    void guard() {
        i32(static_cast<s32>(m_guard));
        i16(static_cast<std::int16_t>(m_guard));
        i8(static_cast<std::int8_t>(m_guard));
        ++m_guard;
    }
    /// Header (start16 / start8 / words) + the three streams, each padded to 4 bytes.
    void finish(u32 versionWord, std::vector<u8>& out) {
        std::vector<u8> s16 = m16;
        std::vector<u8> s8 = m8;
        s16.resize((s16.size() + 3u) & ~usize(3u), 0u);
        s8.resize((s8.size() + 3u) & ~usize(3u), 0u);
        const s32 start16 = static_cast<s32>(m32.size() / 4u);
        const s32 start8 = start16 + static_cast<s32>(s16.size() / 4u);
        const s32 words = start8 + static_cast<s32>(s8.size() / 4u);
        const s32 header[4] = {static_cast<s32>(versionWord), words, start16, start8};
        put(out, header, sizeof(header));
        out.insert(out.end(), m32.begin(), m32.end());
        out.insert(out.end(), s16.begin(), s16.end());
        out.insert(out.end(), s8.begin(), s8.end());
    }

private:
    static void put(std::vector<u8>& out, const void* v, usize n) {
        const u8* p = static_cast<const u8*>(v);
        out.insert(out.end(), p, p + n);
    }
    std::vector<u8> m32;
    std::vector<u8> m16;
    std::vector<u8> m8;
    u32 m_guard = 0;
};

/// Plain little-endian appender (sequences, material list, `.dsq`).
struct ByteWriter {
    std::vector<u8>& out;
    void raw(const void* v, usize n) {
        const u8* p = static_cast<const u8*>(v);
        out.insert(out.end(), p, p + n);
    }
    void i32(s32 v) { raw(&v, 4u); }
    void f(f32 v) { raw(&v, 4u); }
    void i16(std::int16_t v) { raw(&v, 2u); }
    void u8v(u8 v) { out.push_back(v); }
    void name(const std::string& s) {
        i32(static_cast<s32>(s.size()));
        raw(s.data(), s.size());
    }
    void quat(const DtsQuat16& q) {
        for (std::int16_t c : q) {
            i16(c);
        }
    }
};

void write_integer_set(ByteWriter& w, const DtsIntegerSet& set) {
    usize words = set.words.size();
    while (words > 0u && set.words[words - 1u] == 0u) {
        --words;
    }
    w.i32(0);
    w.i32(static_cast<s32>(words));
    for (usize i = 0; i < words; ++i) {
        w.i32(static_cast<s32>(set.words[i]));
    }
}

void write_sequence_record(ByteWriter& w, const DtsSequence& seq) {
    w.i32(static_cast<s32>(seq.flags));
    w.i32(seq.keyframes);
    w.f(seq.duration);
    w.i32(seq.priority);
    w.i32(seq.first_ground_frame);
    w.i32(seq.ground_frames);
    w.i32(seq.base_rotation);
    w.i32(seq.base_translation);
    w.i32(seq.base_scale);
    w.i32(seq.base_object_state);
    w.i32(seq.base_decal_state);
    w.i32(seq.first_trigger);
    w.i32(seq.trigger_count);
    w.f(seq.tool_begin);
    for (const DtsIntegerSet* set : {&seq.rotation_matters, &seq.translation_matters, &seq.scale_matters,
                                     &seq.decal_matters, &seq.ifl_matters, &seq.vis_matters, &seq.frame_matters,
                                     &seq.mat_frame_matters}) {
        write_integer_set(w, *set);
    }
}

struct Mat34 {
    f32 m[3][4] = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};
};

Mat34 node_local(const DtsNode& node) {
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

Mat34 node_world(const DtsShape& shape, s32 node) {
    Mat34 world;
    u32 depth = 0;
    while (node >= 0 && static_cast<usize>(node) < shape.nodes.size() && ++depth <= shape.nodes.size()) {
        world = mul(node_local(shape.nodes[static_cast<usize>(node)]), world);
        node = shape.nodes[static_cast<usize>(node)].parent;
    }
    return world;
}

/// Rigid inverse (rotation transposed, translation -R^T t).
Mat34 rigid_inverse(const Mat34& a) {
    Mat34 r;
    for (u32 i = 0; i < 3u; ++i) {
        for (u32 j = 0; j < 3u; ++j) {
            r.m[i][j] = a.m[j][i];
        }
    }
    for (u32 i = 0; i < 3u; ++i) {
        r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    }
    return r;
}

struct Box {
    f32 lo[3] = {std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()};
    f32 hi[3] = {-std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max()};
    bool empty() const { return lo[0] > hi[0]; }
    void add(const f32 p[3]) {
        for (u32 a = 0; a < 3u; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
    void finish() {
        if (empty()) {
            for (u32 a = 0; a < 3u; ++a) {
                lo[a] = hi[a] = 0.f;
            }
        }
    }
};

/// Vertex positions of a mesh as stored (skins: the bind-pose block, in shape space).
const std::vector<f32>& mesh_positions(const DtsMesh& mesh) { return mesh.positions; }

void write_mesh(StreamWriter& s, u32 version, const DtsShape& shape, const DtsMesh& mesh) {
    s.u32v(static_cast<u32>(mesh.type));
    if (mesh.type == DtsMeshType::Null) {
        return;
    }
    const bool skin = mesh.type == DtsMeshType::Skin;
    const u32 verts = static_cast<u32>(mesh.positions.size() / 3u);
    Box box;
    for (u32 v = 0; v < verts; ++v) {
        box.add(&mesh.positions[v * 3u]);
    }
    box.finish();
    f32 centre[3];
    f32 radius = 0.f;
    for (u32 a = 0; a < 3u; ++a) {
        centre[a] = 0.5f * (box.lo[a] + box.hi[a]);
    }
    for (u32 v = 0; v < verts; ++v) {
        const f32* p = &mesh.positions[v * 3u];
        const f32 d[3] = {p[0] - centre[0], p[1] - centre[1], p[2] - centre[2]};
        radius = std::max(radius, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    }
    s.guard();
    s.i32(1); // frames
    s.i32(1); // material frames
    s.i32(-1); // parent mesh
    s.f(box.lo, 3u);
    s.f(box.hi, 3u);
    s.f(centre, 3u);
    s.f(radius);
    // Standard block: skins keep their vertices in the skin block below.
    const u32 standardVerts = skin ? 0u : verts;
    s.i32(static_cast<s32>(standardVerts));
    if (!skin) {
        s.f(mesh.positions.data(), mesh.positions.size());
    }
    const u32 tverts = static_cast<u32>(mesh.uvs.size() / 2u);
    s.i32(static_cast<s32>(tverts));
    s.f(mesh.uvs.data(), static_cast<usize>(tverts) * 2u);
    if (version > 25u) {
        s.i32(0); // uv1
        s.i32(0); // colours
    }
    if (!skin) {
        for (u32 v = 0; v < verts; ++v) {
            const bool has = mesh.normals.size() >= static_cast<usize>(verts) * 3u;
            for (u32 a = 0; a < 3u; ++a) {
                s.f(has ? mesh.normals[v * 3u + a] : 0.f);
            }
        }
        for (u32 v = 0; v < verts; ++v) {
            s.i8(0); // encoded normal (unused: the float normals are authoritative)
        }
    }
    s.i32(static_cast<s32>(mesh.primitives.size()));
    auto rawMaterial = [](const DtsPrimitive& p) {
        const u32 material = p.material < 0 ? kPrimNoMaterial : static_cast<u32>(p.material);
        return (static_cast<u32>(p.type) << 30u) | kPrimIndexed | material;
    };
    if (version > 25u) {
        for (const DtsPrimitive& p : mesh.primitives) {
            s.u32v(p.start);
            s.u32v(p.count);
            s.u32v(rawMaterial(p));
        }
    } else {
        for (const DtsPrimitive& p : mesh.primitives) {
            s.i16(static_cast<std::int16_t>(p.start));
            s.i16(static_cast<std::int16_t>(p.count));
        }
        for (const DtsPrimitive& p : mesh.primitives) {
            s.u32v(rawMaterial(p));
        }
    }
    s.i32(static_cast<s32>(mesh.indices.size()));
    for (u32 index : mesh.indices) {
        if (version > 25u) {
            s.u32v(index);
        } else {
            s.i16(static_cast<std::int16_t>(static_cast<std::uint16_t>(index)));
        }
    }
    s.i32(0); // merge indices
    s.i32(static_cast<s32>(verts)); // vertices per frame
    s.i32(0);                       // flags
    s.guard();
    if (!skin) {
        return;
    }
    s.i32(static_cast<s32>(verts));
    s.f(mesh.positions.data(), mesh.positions.size());
    for (u32 v = 0; v < verts; ++v) {
        const bool has = mesh.normals.size() >= static_cast<usize>(verts) * 3u;
        for (u32 a = 0; a < 3u; ++a) {
            s.f(has ? mesh.normals[v * 3u + a] : 0.f);
        }
    }
    for (u32 v = 0; v < verts; ++v) {
        s.i8(0);
    }
    const u32 bones = static_cast<u32>(mesh.bone_nodes.size());
    s.i32(static_cast<s32>(bones));
    if (mesh.skin_initial_transforms.size() == static_cast<usize>(bones) * 16u) {
        s.f(mesh.skin_initial_transforms.data(), mesh.skin_initial_transforms.size());
    } else {
        for (s32 node : mesh.bone_nodes) {
            const Mat34 inv = rigid_inverse(node_world(shape, node));
            for (u32 r = 0; r < 3u; ++r) {
                s.f(inv.m[r], 4u);
            }
            const f32 last[4] = {0.f, 0.f, 0.f, 1.f};
            s.f(last, 4u);
        }
    }
    const u32 influences = static_cast<u32>(mesh.skin_vertex.size());
    s.i32(static_cast<s32>(influences));
    for (u32 v : mesh.skin_vertex) {
        s.u32v(v);
    }
    for (u32 b : mesh.skin_bone) {
        s.u32v(b);
    }
    for (f32 w : mesh.skin_weight) {
        s.f(w);
    }
    s.i32(static_cast<s32>(bones));
    for (s32 node : mesh.bone_nodes) {
        s.i32(node);
    }
    s.guard();
}

bool check_shape(const DtsShape& shape, std::string* error) {
    const usize nodes = shape.nodes.size();
    for (usize n = 0; n < nodes; ++n) {
        if (shape.nodes[n].parent < -1 || shape.nodes[n].parent >= static_cast<s32>(nodes) ||
            shape.nodes[n].parent == static_cast<s32>(n)) {
            set_error(error, "node " + std::to_string(n) + " parent out of range");
            return false;
        }
    }
    for (const DtsObject& object : shape.objects) {
        if (object.node < -1 || object.node >= static_cast<s32>(nodes) || object.first_mesh < 0 || object.mesh_count < 0 ||
            static_cast<usize>(object.first_mesh) + static_cast<usize>(object.mesh_count) > shape.meshes.size()) {
            set_error(error, "object '" + object.name + "' references meshes / node out of range");
            return false;
        }
    }
    for (usize m = 0; m < shape.meshes.size(); ++m) {
        const DtsMesh& mesh = shape.meshes[m];
        if (mesh.type != DtsMeshType::Standard && mesh.type != DtsMeshType::Skin && mesh.type != DtsMeshType::Null) {
            set_error(error, "mesh " + std::to_string(m) + ": only standard, skin and null meshes are written");
            return false;
        }
        if (mesh.type == DtsMeshType::Null) {
            continue;
        }
        const usize verts = mesh.positions.size() / 3u;
        if (mesh.positions.size() % 3u != 0u || mesh.uvs.size() % 2u != 0u) {
            set_error(error, "mesh " + std::to_string(m) + ": position / uv arrays not whole vertices");
            return false;
        }
        for (const DtsPrimitive& p : mesh.primitives) {
            if (static_cast<u64>(p.start) + p.count > mesh.indices.size()) {
                set_error(error, "mesh " + std::to_string(m) + ": primitive outside the index list");
                return false;
            }
        }
        for (u32 i : mesh.indices) {
            if (i >= verts) {
                set_error(error, "mesh " + std::to_string(m) + ": index out of range");
                return false;
            }
        }
        if (mesh.type == DtsMeshType::Skin) {
            if (mesh.skin_bone.size() != mesh.skin_vertex.size() || mesh.skin_weight.size() != mesh.skin_vertex.size()) {
                set_error(error, "mesh " + std::to_string(m) + ": skin influence arrays differ in length");
                return false;
            }
            for (s32 node : mesh.bone_nodes) {
                if (node < 0 || static_cast<usize>(node) >= nodes) {
                    set_error(error, "mesh " + std::to_string(m) + ": bone node out of range");
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

DtsQuat16 dts_quat16(f32 x, f32 y, f32 z, f32 w) {
    DtsQuat16 q{};
    const f32 v[4] = {x, y, z, w};
    for (u32 i = 0; i < 4u; ++i) {
        const f32 scaled = std::round(std::clamp(v[i], -1.f, 1.f) * 32767.f);
        q[i] = static_cast<std::int16_t>(scaled);
    }
    return q;
}

std::vector<u8> write_dts(const DtsShape& shape, const DtsWriteOptions& options, std::string* error) {
    if (options.version < 24u || options.version > 26u) {
        set_error(error, "the writer emits .dts versions 24..26 (asked for " + std::to_string(options.version) + ")");
        return {};
    }
    if (!check_shape(shape, error)) {
        return {};
    }
    const u32 version = options.version;
    // Name table: node, object, detail, then sequence names.
    std::vector<std::string> names;
    auto nameIndex = [&names](const std::string& name) {
        for (usize i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
                return static_cast<s32>(i);
            }
        }
        names.push_back(name);
        return static_cast<s32>(names.size() - 1u);
    };
    std::vector<s32> nodeNames, objectNames, detailNames, sequenceNames;
    for (const DtsNode& n : shape.nodes) {
        nodeNames.push_back(nameIndex(n.name));
    }
    for (const DtsObject& o : shape.objects) {
        objectNames.push_back(nameIndex(o.name));
    }
    for (const DtsDetail& d : shape.details) {
        detailNames.push_back(nameIndex(d.name));
    }
    for (const DtsSequence& seq : shape.sequences) {
        sequenceNames.push_back(nameIndex(seq.name));
    }

    // Bounds of every mesh in shape space (rigid meshes through their object's node).
    Box box;
    for (const DtsObject& object : shape.objects) {
        const Mat34 world = node_world(shape, object.node);
        for (s32 m = object.first_mesh; m < object.first_mesh + object.mesh_count; ++m) {
            const DtsMesh& mesh = shape.meshes[static_cast<usize>(m)];
            if (mesh.type == DtsMeshType::Null) {
                continue;
            }
            const std::vector<f32>& p = mesh_positions(mesh);
            for (usize v = 0; v + 2u < p.size(); v += 3u) {
                f32 q[3];
                for (u32 r = 0; r < 3u; ++r) {
                    q[r] = mesh.type == DtsMeshType::Skin
                               ? p[v + r]
                               : world.m[r][0] * p[v] + world.m[r][1] * p[v + 1u] + world.m[r][2] * p[v + 2u] + world.m[r][3];
                }
                box.add(q);
            }
        }
    }
    box.finish();
    f32 centre[3];
    f32 radius = 0.f;
    for (u32 a = 0; a < 3u; ++a) {
        centre[a] = 0.5f * (box.lo[a] + box.hi[a]);
        const f32 h = 0.5f * (box.hi[a] - box.lo[a]);
        radius += h * h;
    }
    radius = std::sqrt(radius);

    const DtsKeyframes& keys = shape.keyframes;
    StreamWriter s;
    s.i32(static_cast<s32>(shape.nodes.size()));
    s.i32(static_cast<s32>(shape.objects.size()));
    s.i32(0); // decals
    s.i32(shape.nodes.empty() && shape.objects.empty() ? 0 : 1); // sub-shapes
    s.i32(0); // IFL materials
    s.i32(static_cast<s32>(keys.node_rotations.size()));
    s.i32(static_cast<s32>(keys.node_translations.size() / 3u));
    s.i32(static_cast<s32>(keys.node_uniform_scales.size()));
    s.i32(static_cast<s32>(keys.node_aligned_scales.size() / 3u));
    s.i32(static_cast<s32>(keys.node_arbitrary_scale_factors.size() / 3u));
    s.i32(static_cast<s32>(keys.ground_translations.size() / 3u));
    s.i32(static_cast<s32>(shape.objects.size())); // object states
    s.i32(0);                                       // decal states
    s.i32(static_cast<s32>(shape.triggers.size()));
    s.i32(static_cast<s32>(shape.details.size()));
    s.i32(static_cast<s32>(shape.meshes.size()));
    s.i32(static_cast<s32>(names.size()));
    // Smallest visible size / detail.
    s32 smallest = -1;
    for (usize d = 0; d < shape.details.size(); ++d) {
        const f32 size = shape.details[d].size;
        if (size >= 0.f && (smallest < 0 || size < shape.details[static_cast<usize>(smallest)].size)) {
            smallest = static_cast<s32>(d);
        }
    }
    s.f(smallest < 0 ? 0.f : shape.details[static_cast<usize>(smallest)].size);
    s.i32(smallest);
    s.guard();
    s.f(radius);
    s.f(radius); // tube radius
    s.f(centre, 3u);
    s.f(box.lo, 3u);
    s.f(box.hi, 3u);
    s.guard();
    const s32 nodeCount = static_cast<s32>(shape.nodes.size());
    for (s32 n = 0; n < nodeCount; ++n) {
        s32 firstObject = -1, firstChild = -1, nextSibling = -1;
        for (usize o = 0; o < shape.objects.size() && firstObject < 0; ++o) {
            if (shape.objects[o].node == n) {
                firstObject = static_cast<s32>(o);
            }
        }
        for (s32 c = 0; c < nodeCount && firstChild < 0; ++c) {
            if (shape.nodes[static_cast<usize>(c)].parent == n) {
                firstChild = c;
            }
        }
        for (s32 c = n + 1; c < nodeCount && nextSibling < 0; ++c) {
            if (shape.nodes[static_cast<usize>(c)].parent == shape.nodes[static_cast<usize>(n)].parent) {
                nextSibling = c;
            }
        }
        s.i32(nodeNames[static_cast<usize>(n)]);
        s.i32(shape.nodes[static_cast<usize>(n)].parent);
        s.i32(firstObject);
        s.i32(firstChild);
        s.i32(nextSibling);
    }
    s.guard();
    for (usize o = 0; o < shape.objects.size(); ++o) {
        s.i32(objectNames[o]);
        s.i32(shape.objects[o].mesh_count);
        s.i32(shape.objects[o].first_mesh);
        s.i32(shape.objects[o].node);
        s.i32(-1); // next sibling
        s.i32(-1); // first decal
    }
    s.guard();
    s.guard(); // decals
    s.guard(); // IFL materials
    if (!shape.nodes.empty() || !shape.objects.empty()) {
        s.i32(0); // first node
        s.i32(0); // first object
        s.i32(0); // first decal
    }
    s.guard();
    if (!shape.nodes.empty() || !shape.objects.empty()) {
        s.i32(nodeCount);
        s.i32(static_cast<s32>(shape.objects.size()));
        s.i32(0);
    }
    s.guard();
    for (const DtsNode& n : shape.nodes) {
        s.quat(dts_quat16(n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]));
    }
    for (const DtsNode& n : shape.nodes) {
        s.f(n.translation, 3u);
    }
    s.f(keys.node_translations.data(), keys.node_translations.size());
    for (const DtsQuat16& q : keys.node_rotations) {
        s.quat(q);
    }
    s.guard();
    s.f(keys.node_uniform_scales.data(), keys.node_uniform_scales.size());
    s.f(keys.node_aligned_scales.data(), keys.node_aligned_scales.size());
    s.f(keys.node_arbitrary_scale_factors.data(), keys.node_arbitrary_scale_factors.size());
    for (const DtsQuat16& q : keys.node_arbitrary_scale_rotations) {
        s.quat(q);
    }
    s.guard();
    s.f(keys.ground_translations.data(), keys.ground_translations.size());
    for (const DtsQuat16& q : keys.ground_rotations) {
        s.quat(q);
    }
    s.guard();
    for (usize o = 0; o < shape.objects.size(); ++o) {
        s.f(1.f); // visible
        s.i32(0); // frame
        s.i32(0); // material frame
    }
    s.guard();
    s.guard(); // decal states
    for (const DtsTrigger& t : shape.triggers) {
        s.u32v(t.state);
        s.f(t.pos);
    }
    s.guard();
    for (usize d = 0; d < shape.details.size(); ++d) {
        const DtsDetail& detail = shape.details[d];
        s.i32(detailNames[d]);
        s.i32(detail.subshape);
        s.i32(detail.object_detail);
        s.f(detail.size);
        s.f(-1.f); // average error
        s.f(-1.f); // max error
        s.i32(detail.poly_count);
        if (version >= 26u) {
            for (u32 i = 0; i < 4u; ++i) {
                s.i32(0); // billboard dimension / detail level / equator / polar steps
            }
            s.f(0.f); // billboard polar angle
            s.i32(0); // billboard include poles
        }
    }
    s.guard();
    for (const DtsMesh& mesh : shape.meshes) {
        write_mesh(s, version, shape, mesh);
    }
    s.guard();
    for (const std::string& name : names) {
        s.cstring(name);
    }
    s.guard();

    std::vector<u8> out;
    s.finish(version | (options.exporter_version << 16u), out);
    ByteWriter w{out};
    w.i32(static_cast<s32>(shape.sequences.size()));
    for (usize i = 0; i < shape.sequences.size(); ++i) {
        w.i32(sequenceNames[i]);
        write_sequence_record(w, shape.sequences[i]);
    }
    // Material list (version 1).
    w.u8v(1u);
    w.i32(static_cast<s32>(shape.materials.size()));
    for (const std::string& m : shape.materials) {
        const usize length = std::min<usize>(m.size(), 255u);
        w.u8v(static_cast<u8>(length));
        w.raw(m.data(), length);
    }
    for (usize i = 0; i < shape.materials.size(); ++i) {
        w.i32(3); // S / T wrap
    }
    for (usize i = 0; i < shape.materials.size(); ++i) {
        w.i32(static_cast<s32>(i)); // reflectance map: its own
    }
    for (u32 table = 0; table < 2u; ++table) { // bump, detail
        for (usize i = 0; i < shape.materials.size(); ++i) {
            w.i32(-1);
        }
    }
    for (u32 table = 0; table < 2u; ++table) { // detail scale, reflection amount
        for (usize i = 0; i < shape.materials.size(); ++i) {
            w.f(1.f);
        }
    }
    return out;
}

std::vector<u8> write_dsq(const DsqFile& dsq, std::string* error) {
    const u32 version = dsq.version == 0u ? 26u : dsq.version;
    if (version < kDsqMinVersion || version > kDsqMaxVersion) {
        set_error(error, "unsupported .dsq version " + std::to_string(version));
        return {};
    }
    const DtsKeyframes& keys = dsq.keyframes;
    if (keys.node_translations.size() % 3u != 0u || keys.node_aligned_scales.size() % 3u != 0u ||
        keys.node_arbitrary_scale_factors.size() != keys.node_arbitrary_scale_rotations.size() * 3u ||
        keys.ground_translations.size() != keys.ground_rotations.size() * 3u) {
        set_error(error, "inconsistent .dsq keyframe pools");
        return {};
    }
    std::vector<u8> out;
    ByteWriter w{out};
    w.i32(static_cast<s32>(version));
    w.i32(static_cast<s32>(dsq.nodes.size()));
    for (const std::string& n : dsq.nodes) {
        w.name(n);
    }
    w.i32(0); // legacy objects
    w.i32(dsq.shape_object_count);
    w.i32(static_cast<s32>(keys.node_rotations.size()));
    for (const DtsQuat16& q : keys.node_rotations) {
        w.quat(q);
    }
    w.i32(static_cast<s32>(keys.node_translations.size() / 3u));
    for (f32 v : keys.node_translations) {
        w.f(v);
    }
    w.i32(static_cast<s32>(keys.node_uniform_scales.size()));
    for (f32 v : keys.node_uniform_scales) {
        w.f(v);
    }
    w.i32(static_cast<s32>(keys.node_aligned_scales.size() / 3u));
    for (f32 v : keys.node_aligned_scales) {
        w.f(v);
    }
    w.i32(static_cast<s32>(keys.node_arbitrary_scale_rotations.size()));
    for (const DtsQuat16& q : keys.node_arbitrary_scale_rotations) {
        w.quat(q);
    }
    for (f32 v : keys.node_arbitrary_scale_factors) {
        w.f(v);
    }
    w.i32(static_cast<s32>(keys.ground_rotations.size()));
    for (f32 v : keys.ground_translations) {
        w.f(v);
    }
    for (const DtsQuat16& q : keys.ground_rotations) {
        w.quat(q);
    }
    w.i32(0); // object states
    w.i32(static_cast<s32>(dsq.sequences.size()));
    for (const DtsSequence& seq : dsq.sequences) {
        w.name(seq.name);
        write_sequence_record(w, seq);
    }
    w.i32(static_cast<s32>(dsq.triggers.size()));
    for (const DtsTrigger& t : dsq.triggers) {
        w.i32(static_cast<s32>(t.state));
        w.f(t.pos);
    }
    return out;
}

bool write_binary_file(const std::string& path, const std::vector<u8>& bytes, std::string* error) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        set_error(error, "cannot write '" + path + "'");
        return false;
    }
    return true;
}

} // namespace fuse::cook
