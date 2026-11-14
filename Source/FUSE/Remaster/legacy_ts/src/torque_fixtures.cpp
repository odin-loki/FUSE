// FUSE Remaster W1.1: synthetic Torque fixtures (see torque_fixtures.hpp).

#include <fuse/remaster/legacy_ts/torque_fixtures.hpp>

#include <fuse/cook/dts_write.hpp>
#include <fuse/remaster/digest.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

namespace fuse::remaster::legacy_ts {

namespace {

using cook::DtsDetail;
using cook::DtsMesh;
using cook::DtsMeshType;
using cook::DtsNode;
using cook::DtsObject;
using cook::DtsPrimitive;
using cook::DtsPrimitiveType;
using cook::DtsSequence;
using cook::DtsShape;
using cook::DtsTrigger;

constexpr f64 kPi = 3.14159265358979323846;

using V3 = std::array<f32, 3>;

V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 scale(const V3& a, f32 s) { return {a[0] * s, a[1] * s, a[2] * s}; }

/// One planar grid face: corner(s, t) for s in [0, ns], t in [0, nt]; normal n (= U x V of the grid axes). Triangles
/// are written clockwise about n (Torque's front face).
template <class Corner>
void emitFace(DtsMesh& mesh, u32 ns, u32 nt, const V3& n, Corner corner) {
    const u32 base = static_cast<u32>(mesh.positions.size() / 3u);
    for (u32 t = 0; t <= nt; ++t) {
        for (u32 s = 0; s <= ns; ++s) {
            const V3 p = corner(static_cast<f32>(s) / static_cast<f32>(ns), static_cast<f32>(t) / static_cast<f32>(nt));
            mesh.positions.insert(mesh.positions.end(), p.begin(), p.end());
            mesh.normals.insert(mesh.normals.end(), n.begin(), n.end());
            mesh.uvs.push_back(static_cast<f32>(s) / static_cast<f32>(ns));
            mesh.uvs.push_back(1.f - static_cast<f32>(t) / static_cast<f32>(nt)); // stored D3D-style (v down)
        }
    }
    auto at = [&](u32 s, u32 t) { return base + t * (ns + 1u) + s; };
    for (u32 t = 0; t < nt; ++t) {
        for (u32 s = 0; s < ns; ++s) {
            mesh.indices.insert(mesh.indices.end(), {at(s, t), at(s + 1u, t + 1u), at(s + 1u, t)});
            mesh.indices.insert(mesh.indices.end(), {at(s, t), at(s, t + 1u), at(s + 1u, t + 1u)});
        }
    }
}

void finishMesh(DtsMesh& mesh, s32 material) {
    mesh.frames = 1;
    mesh.verts_per_frame = static_cast<u32>(mesh.positions.size() / 3u);
    DtsPrimitive prim;
    prim.start = 0;
    prim.count = static_cast<u32>(mesh.indices.size());
    prim.type = DtsPrimitiveType::Triangles;
    prim.material = material;
    mesh.primitives = {prim};
}

/// Unit cube [-0.5, 0.5]^2 x [0, 1], `n` x `n` quads per face.
DtsMesh cubeMesh(u32 n, s32 material) {
    DtsMesh mesh;
    mesh.type = DtsMeshType::Standard;
    struct Face {
        V3 n, u, v;
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };
    for (const Face& f : faces) {
        const V3 centre = add(V3{0.f, 0.f, 0.5f}, scale(f.n, 0.5f));
        emitFace(mesh, n, n, f.n, [&](f32 s, f32 t) { return add(add(centre, scale(f.u, s - 0.5f)), scale(f.v, t - 0.5f)); });
    }
    finishMesh(mesh, material);
    return mesh;
}

constexpr f32 kArmHalfWidth = 0.1f;
constexpr f32 kArmHeight = 1.6f;

/// Skin weights by height: root below 0.1, upper to 0.7, upper/lower blend to 1.0, lower to 1.5, lower/tip blend.
void armWeights(f32 z, std::vector<std::pair<u32, f32>>& out) {
    out.clear();
    if (z <= 0.1f + 1e-6f) {
        out.emplace_back(0u, 1.f);
    } else if (z < 0.7f) {
        out.emplace_back(1u, 1.f);
    } else if (z < 1.0f) {
        const f32 w = (z - 0.7f) / 0.3f;
        out.emplace_back(1u, 1.f - w);
        out.emplace_back(2u, w);
    } else if (z < 1.5f) {
        out.emplace_back(2u, 1.f);
    } else {
        const f32 w = std::min((z - 1.5f) / 0.1f, 1.f);
        out.emplace_back(2u, 1.f - w);
        out.emplace_back(3u, w);
    }
}

/// Square tube (four sides with `rings` segments, two caps), skinned over the four arm joints.
DtsMesh armMesh(u32 rings, s32 material) {
    DtsMesh mesh;
    mesh.type = DtsMeshType::Skin;
    const V3 up{0.f, 0.f, 1.f};
    const V3 sides[4][2] = {{{1, 0, 0}, {0, 1, 0}}, {{-1, 0, 0}, {0, -1, 0}}, {{0, 1, 0}, {-1, 0, 0}}, {{0, -1, 0}, {1, 0, 0}}};
    for (const auto& side : sides) {
        const V3 centre = scale(side[0], kArmHalfWidth);
        emitFace(mesh, 1u, rings, side[0], [&](f32 s, f32 t) {
            return add(add(centre, scale(side[1], (s - 0.5f) * 2.f * kArmHalfWidth)), scale(up, t * kArmHeight));
        });
    }
    emitFace(mesh, 1u, 1u, V3{0, 0, 1}, [&](f32 s, f32 t) {
        return V3{(s - 0.5f) * 2.f * kArmHalfWidth, (t - 0.5f) * 2.f * kArmHalfWidth, kArmHeight};
    });
    emitFace(mesh, 1u, 1u, V3{0, 0, -1}, [&](f32 s, f32 t) {
        return V3{(t - 0.5f) * 2.f * kArmHalfWidth, (s - 0.5f) * 2.f * kArmHalfWidth, 0.f};
    });
    finishMesh(mesh, material);
    mesh.bone_nodes = {0, 1, 2, 3};
    std::vector<std::pair<u32, f32>> weights;
    for (u32 v = 0; v < mesh.positions.size() / 3u; ++v) {
        armWeights(mesh.positions[v * 3u + 2u], weights);
        for (const auto& [bone, w] : weights) {
            if (w > 0.f) {
                mesh.skin_vertex.push_back(v);
                mesh.skin_bone.push_back(bone);
                mesh.skin_weight.push_back(w);
            }
        }
    }
    return mesh;
}

DtsMesh nullMesh() {
    DtsMesh mesh;
    mesh.type = DtsMeshType::Null;
    return mesh;
}

DtsNode node(const char* name, s32 parent, f32 z) {
    DtsNode n;
    n.name = name;
    n.parent = parent;
    n.translation[2] = z;
    return n;
}

DtsDetail detail(const char* name, s32 objectDetail, f32 size, s32 polys) {
    DtsDetail d;
    d.name = name;
    d.subshape = 0;
    d.object_detail = objectDetail;
    d.size = size;
    d.poly_count = polys;
    return d;
}

cook::DtsQuat16 axisAngle(f32 ax, f32 ay, f32 az, f64 radians) {
    const f32 s = static_cast<f32>(std::sin(radians * 0.5));
    return cook::dts_quat16(ax * s, ay * s, az * s, static_cast<f32>(std::cos(radians * 0.5)));
}

f64 deg(f64 d) { return d * kPi / 180.0; }

/// Snap to a 1e-4 grid: libm sin / cos may differ in the last ulp between toolchains, the snapped value does not, so
/// the fixtures stay byte-identical on MSVC and GCC (rotations are snapped by the Quat16 quantisation itself).
f32 grid4(f64 v) { return static_cast<f32>(std::round(v * 1e4) / 1e4); }

// ---- PNG (stored deflate) ------------------------------------------------------------------------------------

u32 crc32(const u8* data, usize size, u32 crc = 0u) {
    crc = ~crc;
    for (usize i = 0; i < size; ++i) {
        crc ^= data[i];
        for (u32 k = 0; k < 8u; ++k) {
            crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void putBe32(std::vector<u8>& out, u32 v) {
    for (s32 shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<u8>((v >> shift) & 0xFFu));
    }
}

void chunk(std::vector<u8>& out, const char type[4], const std::vector<u8>& data) {
    putBe32(out, static_cast<u32>(data.size()));
    std::vector<u8> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    putBe32(out, crc32(body.data(), body.size()));
}

} // namespace

std::vector<u8> encodePngRgba8(const u8* rgba, u32 width, u32 height) {
    std::vector<u8> raw;
    raw.reserve(static_cast<usize>(height) * (static_cast<usize>(width) * 4u + 1u));
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0u); // filter: none
        const u8* row = rgba + static_cast<usize>(y) * width * 4u;
        raw.insert(raw.end(), row, row + static_cast<usize>(width) * 4u);
    }
    std::vector<u8> z = {0x78u, 0x01u};
    usize pos = 0;
    do {
        const usize n = std::min<usize>(raw.size() - pos, 65535u);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1u : 0u);
        z.push_back(static_cast<u8>(n & 0xFFu));
        z.push_back(static_cast<u8>(n >> 8u));
        z.push_back(static_cast<u8>(~n & 0xFFu));
        z.push_back(static_cast<u8>((~n >> 8u) & 0xFFu));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    u32 a = 1u, b = 0u;
    for (u8 c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    putBe32(z, (b << 16u) | a);
    std::vector<u8> out = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    std::vector<u8> ihdr;
    putBe32(ihdr, width);
    putBe32(ihdr, height);
    ihdr.insert(ihdr.end(), {8u, 6u, 0u, 0u, 0u}); // 8-bit RGBA, deflate, adaptive filter, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

DtsShape fixtureCrateShape() {
    DtsShape shape;
    shape.version = 24;
    shape.nodes = {node("root", -1, 0.f)};
    DtsObject crate;
    crate.name = "crate";
    crate.node = 0;
    crate.first_mesh = 0;
    crate.mesh_count = 3;
    DtsObject collision;
    collision.name = "Col-1";
    collision.node = 0;
    collision.first_mesh = 3;
    collision.mesh_count = 3;
    shape.objects = {crate, collision};
    shape.meshes = {cubeMesh(8u, 0), cubeMesh(2u, 0), nullMesh(), nullMesh(), nullMesh(), cubeMesh(1u, -1)};
    shape.details = {detail("detail128", 0, 128.f, 768), detail("detail32", 1, 32.f, 48), detail("Collision-1", 2, -1.f, 12)};
    shape.materials = {"crate_mat"};
    return shape;
}

DtsShape fixtureArmShape() {
    DtsShape shape;
    shape.version = 26;
    shape.nodes = {node("root", -1, 0.f), node("upper", 0, 0.1f), node("lower", 1, 0.75f), node("tip", 2, 0.75f)};
    DtsObject arm;
    arm.name = "arm";
    arm.node = 0;
    arm.first_mesh = 0;
    arm.mesh_count = 2;
    shape.objects = {arm};
    shape.meshes = {armMesh(8u, 0), armMesh(2u, 0)};
    shape.details = {detail("detail64", 0, 64.f, 68), detail("detail16", 1, 16.f, 20)};
    shape.materials = {"arm_mat"};

    // "wave": cyclic, 8 keys over 1 s.
    cook::DtsKeyframes& keys = shape.keyframes;
    constexpr s32 kKeys = 8;
    for (s32 k = 0; k < kKeys; ++k) {
        keys.node_rotations.push_back(axisAngle(1.f, 0.f, 0.f, deg(30.0) * std::sin(2.0 * kPi * k / kKeys))); // upper
    }
    for (s32 k = 0; k < kKeys; ++k) {
        keys.node_rotations.push_back(axisAngle(1.f, 0.f, 0.f, deg(45.0) * std::cos(2.0 * kPi * k / kKeys))); // lower
    }
    for (s32 k = 0; k < kKeys; ++k) {
        keys.node_translations.insert(keys.node_translations.end(),
                                      {0.f, 0.f, grid4(0.05 * std::sin(2.0 * kPi * k / kKeys))}); // root
    }
    for (s32 g = 1; g <= 4; ++g) {
        keys.ground_translations.insert(keys.ground_translations.end(), {0.f, 0.25f * static_cast<f32>(g), 0.f});
        keys.ground_rotations.push_back(axisAngle(0.f, 0.f, 1.f, deg(22.5 * g)));
    }
    DtsSequence wave;
    wave.name = "wave";
    wave.flags = cook::kDtsSeqCyclic;
    wave.keyframes = kKeys;
    wave.duration = 1.f;
    wave.ground_frames = 4;
    wave.trigger_count = 2;
    wave.rotation_matters.set(1u);
    wave.rotation_matters.set(2u);
    wave.translation_matters.set(0u);
    shape.sequences = {wave};
    shape.triggers = {DtsTrigger{cook::kDtsTriggerOn | 1u, 0.25f}, DtsTrigger{1u, 0.75f}};
    return shape;
}

cook::DsqFile fixtureArmDsq() {
    cook::DsqFile dsq;
    dsq.version = 26;
    dsq.nodes = {"tip", "lower", "upper", "root"};
    dsq.shape_object_count = 1;
    cook::DtsKeyframes& keys = dsq.keyframes;
    // walk: one-shot, 5 keys over 0.8 s; lower (dsq node 1) bends, tip (dsq node 0) scales (aligned).
    for (s32 k = 0; k < 5; ++k) {
        keys.node_rotations.push_back(axisAngle(0.f, 1.f, 0.f, deg(20.0 * k)));
        keys.node_aligned_scales.insert(keys.node_aligned_scales.end(),
                                        {1.f + 0.1f * static_cast<f32>(k), 1.f, 1.f - 0.05f * static_cast<f32>(k)});
    }
    // idle: cyclic, 4 keys over 2 s; upper (dsq node 2) sways, root (dsq node 3) breathes (uniform scale).
    for (s32 k = 0; k < 4; ++k) {
        keys.node_rotations.push_back(axisAngle(0.f, 0.f, 1.f, deg(10.0) * std::sin(2.0 * kPi * k / 4.0)));
        keys.node_uniform_scales.push_back(1.f + 0.02f * static_cast<f32>(k));
    }
    DtsSequence walk;
    walk.name = "walk";
    walk.flags = cook::kDtsSeqAlignedScale;
    walk.keyframes = 5;
    walk.duration = 0.8f;
    walk.base_rotation = 0;
    walk.base_scale = 0;
    walk.first_trigger = 0;
    walk.trigger_count = 2;
    walk.rotation_matters.set(1u);
    walk.scale_matters.set(0u);
    DtsSequence idle;
    idle.name = "idle";
    idle.flags = cook::kDtsSeqCyclic | cook::kDtsSeqUniformScale;
    idle.keyframes = 4;
    idle.duration = 2.f;
    idle.base_rotation = 5;
    idle.base_scale = 0;
    idle.rotation_matters.set(2u);
    idle.scale_matters.set(3u);
    dsq.sequences = {walk, idle};
    dsq.triggers = {DtsTrigger{cook::kDtsTriggerOn | 1u, 0.25f}, DtsTrigger{cook::kDtsTriggerOn | 2u, 0.75f}};
    return dsq;
}

std::vector<u8> fixtureCrateAlbedoRgba(u32& width, u32& height) {
    width = height = 512u;
    std::vector<u8> rgba(static_cast<usize>(width) * height * 4u);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const bool dark = ((x / 64u) + (y / 64u)) % 2u != 0u;
            u8* p = &rgba[(static_cast<usize>(y) * width + x) * 4u];
            p[0] = dark ? 120u : 150u;
            p[1] = dark ? 85u : 110u;
            p[2] = dark ? 50u : 70u;
            p[3] = 255u;
        }
    }
    return rgba;
}

std::vector<FixtureFile> buildTorqueFixtures(std::string* error) {
    std::vector<FixtureFile> files;
    std::string why;
    cook::DtsWriteOptions v24;
    v24.version = 24;
    std::vector<u8> crate = cook::write_dts(fixtureCrateShape(), v24, &why);
    if (crate.empty()) {
        if (error != nullptr) {
            *error = "crate.dts: " + why;
        }
        return {};
    }
    cook::DtsWriteOptions v26;
    v26.version = 26;
    std::vector<u8> arm = cook::write_dts(fixtureArmShape(), v26, &why);
    if (arm.empty()) {
        if (error != nullptr) {
            *error = "arm.dts: " + why;
        }
        return {};
    }
    std::vector<u8> dsq = cook::write_dsq(fixtureArmDsq(), &why);
    if (dsq.empty()) {
        if (error != nullptr) {
            *error = "arm_anims.dsq: " + why;
        }
        return {};
    }
    u32 w = 0, h = 0;
    const std::vector<u8> albedo = fixtureCrateAlbedoRgba(w, h);
    files.push_back({"shapes/arm.dts", std::move(arm)});
    files.push_back({"shapes/arm_anims.dsq", std::move(dsq)});
    files.push_back({"shapes/crate.dts", std::move(crate)});
    files.push_back({"shapes/crate_mat.png", encodePngRgba8(albedo.data(), w, h)});
    std::sort(files.begin(), files.end(), [](const FixtureFile& a, const FixtureFile& b) { return a.path < b.path; });
    return files;
}

bool writeTorqueFixtures(const std::filesystem::path& root, std::string* error) {
    const std::vector<FixtureFile> files = buildTorqueFixtures(error);
    if (files.empty()) {
        return false;
    }
    for (const FixtureFile& f : files) {
        if (!cook::write_binary_file((root / f.path).string(), f.bytes, error)) {
            return false;
        }
    }
    return true;
}

std::string fixtureManifest(const std::vector<FixtureFile>& files) {
    std::string out;
    for (const FixtureFile& f : files) {
        out += sha256Hex(f.bytes.data(), f.bytes.size()) + "  " + f.path + "\n";
    }
    return out;
}

std::vector<std::string> committedTorqueFixtureNames() { return {"fixture.cs", "fixture.mis"}; }

} // namespace fuse::remaster::legacy_ts
