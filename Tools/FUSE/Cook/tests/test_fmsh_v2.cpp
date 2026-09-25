// Asset plan W0.1 gate (docs/plans/FUSE_ASSET_PLAN.md §1.4, §5.1, §6 Wave 0): FMSH v2 streams —
// tangent (oct + sign), uv1, colour0 (RGBA8), skin joints (u8/u16 × 4) + weights (unorm8/16 × 4),
// quantised positions (unorm16 in bounds) and normals (oct snorm16), per-submesh material slot names —
// round-trip within their quantisation bounds; FMSH v1 files still load and a mesh without v2 content
// still serializes as v1 byte for byte; malformed v2 files are refused; the assimp import fills the
// new streams (OBJ tangents / material names, glTF skin).
#include <fuse/cook/mesh_cook.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace fuse;
using namespace fuse::cook;

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::string temp_path(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_asset_fmsh_v2";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

u32 le32(const std::vector<u8>& b, usize at) {
    return static_cast<u32>(b[at]) | (static_cast<u32>(b[at + 1]) << 8) | (static_cast<u32>(b[at + 2]) << 16) |
           (static_cast<u32>(b[at + 3]) << 24);
}

u64 fnv(const u8* data, usize size) {
    u64 hash = 14695981039346656037ull;
    for (usize i = 0; i < size; ++i) {
        hash = (hash ^ data[i]) * 1099511628211ull;
    }
    return hash;
}

/// Re-seal a tampered blob so only the intended defect is tested (not the checksum).
std::vector<u8> reseal(std::vector<u8> bytes) {
    const u64 hash = fnv(bytes.data(), bytes.size() - 8u);
    for (u32 i = 0; i < 8u; ++i) {
        bytes[bytes.size() - 8u + i] = static_cast<u8>((hash >> (i * 8u)) & 0xFFu);
    }
    return bytes;
}

void put32(std::vector<u8>& out, u32 v) {
    for (u32 s = 0; s < 32u; s += 8u) {
        out.push_back(static_cast<u8>((v >> s) & 0xFFu));
    }
}

void putf(std::vector<u8>& out, f32 v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, 4);
    put32(out, bits);
}

/// A small non-trivial mesh: a bent grid, two submeshes, all v2 streams filled.
CookedMesh make_mesh(u32 jointBase) {
    CookedMesh mesh;
    const u32 n = 6;
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const f32 fx = static_cast<f32>(x) / (n - 1u);
            const f32 fy = static_cast<f32>(y) / (n - 1u);
            mesh.positions.insert(mesh.positions.end(), {fx * 3.f - 1.f, std::sin(fx * 2.f) * 0.5f, fy * 2.f + 10.f});
            f32 nx = -std::cos(fx * 2.f) * 0.5f;
            f32 ny = 1.f;
            f32 nz = (x + y) % 5u == 0u ? -0.7f : 0.1f; // some normals in the lower hemisphere
            const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
            mesh.normals.insert(mesh.normals.end(), {nx / len, ny / len, nz / len});
            mesh.uvs.insert(mesh.uvs.end(), {fx, fy});
            mesh.uv1s.insert(mesh.uv1s.end(), {fx * 0.5f + 0.25f, 1.f - fy});
            // Tangent orthogonal to the normal.
            f32 t[3] = {1.f, 0.f, 0.f};
            const f32 d = t[0] * nx / len;
            t[0] -= d * nx / len;
            t[1] -= d * ny / len;
            t[2] -= d * nz / len;
            const f32 tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            mesh.tangents.insert(mesh.tangents.end(), {t[0] / tl, t[1] / tl, t[2] / tl, (x % 2u) ? -1.f : 1.f});
            mesh.colors.insert(mesh.colors.end(), {static_cast<u8>(x * 40u), static_cast<u8>(y * 40u), 200u, 255u});
            const u16 j0 = static_cast<u16>(jointBase + x);
            mesh.joints.insert(mesh.joints.end(), {j0, static_cast<u16>(j0 + 1u), static_cast<u16>(jointBase + y), 0u});
            const f32 w0 = 0.5f + 0.1f * fx;
            const f32 w1 = 0.3f - 0.1f * fx + 0.037f;
            const f32 w2 = 1.f / 7.f;
            mesh.weights.insert(mesh.weights.end(), {w0, w1, w2, 1.f - w0 - w1 - w2});
        }
    }
    for (u32 y = 0; y + 1u < n; ++y) {
        for (u32 x = 0; x + 1u < n; ++x) {
            const u32 i = y * n + x;
            mesh.indices.insert(mesh.indices.end(), {i, i + 1u, i + n, i + 1u, i + n + 1u, i + n});
        }
    }
    const u32 half = static_cast<u32>(mesh.indices.size() / 2u / 3u * 3u);
    mesh.submeshes.push_back({0u, half, 0u, 0u});
    mesh.submeshes.push_back({half, static_cast<u32>(mesh.indices.size()) - half, 0u, 1u});
    mesh.material_slots = {"bark", "leaves_cutout"};
    for (u32 axis = 0; axis < 3u; ++axis) {
        mesh.bounds_min[axis] = 1e30f;
        mesh.bounds_max[axis] = -1e30f;
        for (u32 v = 0; v < mesh.vertex_count(); ++v) {
            mesh.bounds_min[axis] = std::min(mesh.bounds_min[axis], mesh.positions[v * 3u + axis]);
            mesh.bounds_max[axis] = std::max(mesh.bounds_max[axis], mesh.positions[v * 3u + axis]);
        }
    }
    return mesh;
}

f32 max_abs_diff(const std::vector<f32>& a, const std::vector<f32>& b) {
    if (a.size() != b.size()) {
        return 1e30f;
    }
    f32 worst = 0.f;
    for (usize i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    }
    return worst;
}

f32 max_angle_deg(const std::vector<f32>& a, const std::vector<f32>& b, u32 stride) {
    // atan2(|a × b|, a · b) in double: acos of a float dot product cannot resolve angles < ~0.03 deg.
    f64 worst = 0.0;
    for (usize v = 0; v * stride < a.size(); ++v) {
        const f64 ax = a[v * stride], ay = a[v * stride + 1u], az = a[v * stride + 2u];
        const f64 bx = b[v * stride], by = b[v * stride + 1u], bz = b[v * stride + 2u];
        const f64 cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
        const f64 angle = std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz);
        worst = std::max(worst, angle * 57.29577951308232);
    }
    return static_cast<f32>(worst);
}

void test_v1_compatibility() {
    CookedMesh mesh = make_mesh(0);
    mesh.tangents.clear();
    mesh.uv1s.clear();
    mesh.colors.clear();
    mesh.joints.clear();
    mesh.weights.clear();
    mesh.material_slots.clear();
    const std::vector<u8> bytes = serialize_cooked_mesh(mesh);
    check(le32(bytes, 4) == 1u, "mesh without v2 content serializes as FMSH v1");

    // Hand-built v1 blob (layout of the original writer, independent of serialize_cooked_mesh).
    std::vector<u8> v1 = {'F', 'M', 'S', 'H'};
    put32(v1, 1u);
    put32(v1, 0u);
    put32(v1, mesh.vertex_count());
    put32(v1, static_cast<u32>(mesh.indices.size()));
    put32(v1, static_cast<u32>(mesh.submeshes.size()));
    for (f32 v : mesh.bounds_min) {
        putf(v1, v);
    }
    for (f32 v : mesh.bounds_max) {
        putf(v1, v);
    }
    for (const CookedMesh::Submesh& s : mesh.submeshes) {
        put32(v1, s.index_offset);
        put32(v1, s.index_count);
        put32(v1, s.vertex_offset);
        put32(v1, s.material_index);
    }
    for (f32 v : mesh.positions) {
        putf(v1, v);
    }
    for (f32 v : mesh.normals) {
        putf(v1, v);
    }
    for (f32 v : mesh.uvs) {
        putf(v1, v);
    }
    for (u32 i : mesh.indices) {
        put32(v1, i);
    }
    const u64 hash = fnv(v1.data(), v1.size());
    for (u32 i = 0; i < 8u; ++i) {
        v1.push_back(static_cast<u8>((hash >> (i * 8u)) & 0xFFu));
    }
    check(v1 == bytes, "v1 serialization is byte-identical to the original layout");
    CookedMesh loaded;
    std::string error;
    check(deserialize_cooked_mesh(v1.data(), v1.size(), loaded, &error), "v1 blob still loads: " + error);
    check(loaded.positions == mesh.positions && loaded.normals == mesh.normals && loaded.uvs == mesh.uvs &&
              loaded.indices == mesh.indices && loaded.submeshes.size() == 2u && loaded.tangents.empty() &&
              loaded.weights.empty() && loaded.material_slots.empty(),
          "v1 fields load");
    std::vector<u8> bumped = v1;
    bumped[4] = 3u;
    check(!deserialize_cooked_mesh(bumped.data(), bumped.size(), loaded), "unknown version refused");
}

void test_v2_full_precision_round_trip() {
    const CookedMesh mesh = make_mesh(0);
    const std::vector<u8> bytes = serialize_cooked_mesh(mesh);
    check(le32(bytes, 4) == 2u, "mesh with v2 streams serializes as FMSH v2");
    check(le32(bytes, 48) == 8u, "eight streams (pos, normal, uv0, tangent, uv1, colour, joints, weights)");
    CookedMesh back;
    std::string error;
    check(deserialize_cooked_mesh(bytes.data(), bytes.size(), back, &error), "v2 loads: " + error);
    check(back.positions == mesh.positions && back.normals == mesh.normals && back.uvs == mesh.uvs &&
              back.uv1s == mesh.uv1s && back.colors == mesh.colors && back.joints == mesh.joints &&
              back.indices == mesh.indices && back.material_slots == mesh.material_slots,
          "lossless streams round trip exactly");
    check(back.submeshes.size() == 2u && back.submeshes[1].material_index == 1u, "submeshes round trip");
    check(max_angle_deg(back.tangents, mesh.tangents, 4u) < 0.01f, "tangent direction within 0.01 deg (oct16)");
    bool signs = true;
    for (u32 v = 0; v < mesh.vertex_count(); ++v) {
        signs = signs && back.tangents[v * 4u + 3u] == mesh.tangents[v * 4u + 3u];
    }
    check(signs, "bitangent signs round trip");
    // Each weight rounds by ≤ ½ step; the residue that makes the sum exact lands on the largest one.
    check(max_abs_diff(back.weights, mesh.weights) <= 2.5f / 65535.f + 1e-6f, "unorm16 weights within 2.5 steps");
    for (u32 v = 0; v < back.vertex_count(); ++v) {
        f32 sum = 0.f;
        for (u32 k = 0; k < 4u; ++k) {
            sum += back.weights[v * 4u + k];
        }
        check(std::fabs(sum - 1.f) < 1e-5f, "weights sum to 1");
    }
    check(bytes == serialize_cooked_mesh(mesh), "v2 serialization deterministic");
    check(serialize_cooked_mesh(back) == bytes, "load → serialize reproduces the bytes (full precision)");
    // Joints ≥ 256 switch the joint stream to u16 × 4.
    const CookedMesh wide = make_mesh(300);
    const std::vector<u8> wideBytes = serialize_cooked_mesh(wide);
    CookedMesh wideBack;
    check(deserialize_cooked_mesh(wideBytes.data(), wideBytes.size(), wideBack, &error) && wideBack.joints == wide.joints,
          "u16 joints round trip: " + error);
    check(wideBytes.size() == bytes.size() + wide.vertex_count() * 4u, "u16 joints take 8 bytes per vertex");
}

void test_v2_quantised_round_trip() {
    const CookedMesh mesh = make_mesh(0);
    MeshEncoding encoding;
    encoding.quantize_positions = true;
    encoding.quantize_normals = true;
    encoding.weights_unorm8 = true;
    const std::vector<u8> bytes = serialize_cooked_mesh(mesh, encoding);
    const std::vector<u8> full = serialize_cooked_mesh(mesh);
    check(bytes.size() < full.size(), "quantised FMSH is smaller");
    check((le32(bytes, 8) & 1u) == 1u, "quantised-positions flag set");
    CookedMesh back;
    std::string error;
    check(deserialize_cooked_mesh(bytes.data(), bytes.size(), back, &error), "quantised v2 loads: " + error);
    f32 maxExtent = 0.f;
    for (u32 axis = 0; axis < 3u; ++axis) {
        maxExtent = std::max(maxExtent, mesh.bounds_max[axis] - mesh.bounds_min[axis]);
    }
    const f32 posErr = max_abs_diff(back.positions, mesh.positions);
    std::printf("  quantised: position error %.3g (extent %.3g), normal error %.4f deg\n", posErr, maxExtent,
                max_angle_deg(back.normals, mesh.normals, 3u));
    check(posErr <= maxExtent / 65535.f * 0.5f + 1e-5f, "unorm16 positions within half a step of the bounds");
    check(max_angle_deg(back.normals, mesh.normals, 3u) < 0.01f, "oct16 normals within 0.01 deg");
    check(max_abs_diff(back.weights, mesh.weights) <= 2.5f / 255.f + 1e-6f, "unorm8 weights within 2.5 steps");
    for (u32 v = 0; v < back.vertex_count(); ++v) {
        u32 sum = 0;
        for (u32 k = 0; k < 4u; ++k) {
            sum += static_cast<u32>(std::lround(back.weights[v * 4u + k] * 255.f));
        }
        check(sum == 255u, "unorm8 weights sum exactly to 255");
    }
    for (u32 axis = 0; axis < 3u; ++axis) {
        check(back.bounds_min[axis] == mesh.bounds_min[axis] && back.bounds_max[axis] == mesh.bounds_max[axis],
              "bounds exact");
    }
    check(bytes == serialize_cooked_mesh(mesh, encoding), "quantised serialization deterministic");
    // Re-quantising decoded positions is stable (no drift over cook/load cycles).
    const std::vector<u8> again = serialize_cooked_mesh(back, encoding);
    CookedMesh back2;
    check(deserialize_cooked_mesh(again.data(), again.size(), back2) &&
              max_abs_diff(back2.positions, back.positions) <= maxExtent / 65535.f * 0.5f + 1e-5f,
          "quantised positions stable over cycles");
    // Quantisation alone (no v2 streams) still produces v2.
    CookedMesh plain = mesh;
    plain.tangents.clear();
    plain.uv1s.clear();
    plain.colors.clear();
    plain.joints.clear();
    plain.weights.clear();
    plain.material_slots.clear();
    MeshEncoding posOnly;
    posOnly.quantize_positions = true;
    const std::vector<u8> plainBytes = serialize_cooked_mesh(plain, posOnly);
    CookedMesh plainBack;
    check(le32(plainBytes, 4) == 2u && deserialize_cooked_mesh(plainBytes.data(), plainBytes.size(), plainBack) &&
              plainBack.normals == plain.normals && plainBack.tangents.empty(),
          "quantised positions-only mesh");
}

void test_v2_rejections() {
    const CookedMesh mesh = make_mesh(0);
    const std::vector<u8> good = serialize_cooked_mesh(mesh);
    CookedMesh out;
    std::string error;
    check(deserialize_cooked_mesh(good.data(), good.size(), out), "baseline v2 loads");
    std::vector<u8> flipped = good;
    flipped[100] ^= 1u;
    check(!deserialize_cooked_mesh(flipped.data(), flipped.size(), out), "checksum mismatch refused");
    std::vector<u8> truncated(good.begin(), good.end() - 12);
    check(!deserialize_cooked_mesh(truncated.data(), truncated.size(), out), "truncated v2 refused");

    // Locate the stream table (after header, submeshes and slot names).
    usize cursor = 56u + 16u * mesh.submeshes.size();
    for (const std::string& name : mesh.material_slots) {
        cursor += 4u + ((name.size() + 3u) & ~static_cast<usize>(3u));
    }
    std::vector<usize> streamAt;
    for (u32 s = 0; s < le32(good, 48); ++s) {
        streamAt.push_back(cursor);
        cursor += 12u + ((le32(good, cursor + 8u) + 3u) & ~3u);
    }
    auto tamper = [&](usize at, u32 value) {
        std::vector<u8> bytes = good;
        for (u32 i = 0; i < 4u; ++i) {
            bytes[at + i] = static_cast<u8>((value >> (i * 8u)) & 0xFFu);
        }
        return reseal(bytes);
    };
    auto refused = [&](const std::vector<u8>& bytes, const std::string& what) {
        check(!deserialize_cooked_mesh(bytes.data(), bytes.size(), out, &error), what);
    };
    refused(tamper(streamAt[3], 99u), "unknown stream semantic refused");
    refused(tamper(streamAt[3], 1u), "duplicate position stream refused");
    refused(tamper(streamAt[3] + 4u, 1u), "tangent with a float format refused");
    refused(tamper(streamAt[3] + 8u, 4u), "stream byte length mismatch refused");
    refused(tamper(streamAt[7], 5u), "weights replaced by a second uv1 → duplicate refused");
    refused(tamper(8u, 6u), "unknown flags refused");
    refused(tamper(52u, 1u), "material slot count not matching submeshes refused");
    // Weights that do not sum to 1.
    {
        std::vector<u8> bytes = good;
        const usize weights = streamAt[7] + 12u;
        bytes[weights] = static_cast<u8>(bytes[weights] + 1u);
        refused(reseal(bytes), "skin weights not summing to 1 refused");
    }
    // Joints without weights: drop the weight stream by reducing the stream count.
    {
        std::vector<u8> bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(streamAt[7]));
        const usize indicesAt = cursor;
        bytes.insert(bytes.end(), good.begin() + static_cast<std::ptrdiff_t>(indicesAt), good.end());
        bytes[48] = 7u;
        refused(reseal(bytes), "joints without weights refused");
    }
    // Bad tangent sign.
    {
        std::vector<u8> bytes = good;
        bytes[streamAt[3] + 12u + 4u] = 5u;
        refused(reseal(bytes), "tangent sign other than ±1 refused");
    }
    // Index out of range.
    refused(tamper(good.size() - 12u, mesh.vertex_count()), "index out of range refused");
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

void test_assimp_import() {
#if defined(FUSE_HAS_ASSIMP)
    // OBJ with uvs and two materials: tangents from assimp, material slot names, v1 by default.
    const std::string mtl = temp_path("quad.mtl");
    write_text(mtl, "newmtl stone_wall\nKd 0.5 0.5 0.5\nnewmtl moss\nKd 0.2 0.6 0.2\n");
    const std::string obj = temp_path("quad.obj");
    write_text(obj,
               "mtllib quad.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 2 0 0\nv 2 1 0\n"
               "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt 2 0\nvt 2 1\n"
               "vn 0 0 1\n"
               "usemtl stone_wall\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n"
               "usemtl moss\nf 2/2/1 5/5/1 6/6/1\nf 2/2/1 6/6/1 3/3/1\n");
    const std::string v1Out = temp_path("quad_v1.fusemesh");
    check(cook_mesh_file(obj, v1Out).ok, "default OBJ cook");
    std::string error;
    std::ifstream v1In(v1Out, std::ios::binary);
    const std::vector<u8> v1Bytes((std::istreambuf_iterator<char>(v1In)), std::istreambuf_iterator<char>());
    check(v1Bytes.size() > 8u && le32(v1Bytes, 4) == 1u, "default cook still writes FMSH v1");

    MeshCookOptions options;
    options.import_tangents = true;
    options.import_material_names = true;
    options.import_uv1 = true;    // absent in the source: stream omitted, not fabricated
    options.import_colors = true; // absent in the source: stream omitted
    options.encoding.quantize_positions = true;
    const std::string v2Out = temp_path("quad_v2.fusemesh");
    const CookStubWriteResult cooked = cook_mesh_file(obj, v2Out, options);
    check(cooked.ok, "v2 OBJ cook: " + cooked.note);
    CookedMesh v2;
    check(load_cooked_mesh(v2Out, v2, &error), "v2 OBJ loads: " + error);
    check(v2.tangents.size() == v2.vertex_count() * 4u && v2.vertex_count() > 0u, "OBJ tangents imported");
    check(v2.uv1s.empty() && v2.colors.empty() && v2.weights.empty(), "absent streams not fabricated");
    bool orthogonal = true;
    for (u32 v = 0; v < v2.vertex_count(); ++v) {
        const f32* t = &v2.tangents[v * 4u];
        const f32* n = &v2.normals[v * 3u];
        orthogonal = orthogonal && std::fabs(t[0] * n[0] + t[1] * n[1] + t[2] * n[2]) < 1e-3f &&
                     std::fabs(t[0] - 1.f) < 1e-3f; // u increases along +X in this quad
    }
    check(orthogonal, "OBJ tangents follow +U and are orthogonal to the normals");
    check(v2.material_slots.size() == v2.submeshes.size() && v2.submeshes.size() == 2u, "one slot name per submesh");
    const bool names = v2.material_slots.size() == 2u &&
                       ((v2.material_slots[0] == "stone_wall" && v2.material_slots[1] == "moss") ||
                        (v2.material_slots[0] == "moss" && v2.material_slots[1] == "stone_wall"));
    check(names, "material slot names from the MTL");

    // glTF skin (assimp's own test asset, MIT): joints + weights, ≤ 4 per vertex, summing to 1.
    const std::string gltf = std::string(FUSE_SOURCE_DIR) + "/Engine/lib/assimp/test/models/glTF2/simple_skin/simple_skin.gltf";
    if (std::filesystem::exists(gltf)) {
        MeshCookOptions skin;
        skin.import_skin = true;
        const std::string skinOut = temp_path("simple_skin.fusemesh");
        const CookStubWriteResult skinned = cook_mesh_file(gltf, skinOut, skin);
        check(skinned.ok, "glTF skin cook: " + skinned.note);
        CookedMesh s;
        check(load_cooked_mesh(skinOut, s, &error), "skinned FMSH loads: " + error);
        check(s.vertex_count() == 10u && s.joints.size() == 40u && s.weights.size() == 40u, "skin streams sized");
        bool sums = true;
        bool twoJoints = true;
        bool blended = false;
        for (u32 v = 0; v < s.vertex_count(); ++v) {
            f32 sum = 0.f;
            for (u32 k = 0; k < 4u; ++k) {
                sum += s.weights[v * 4u + k];
                twoJoints = twoJoints && s.joints[v * 4u + k] < 2u;
            }
            sums = sums && std::fabs(sum - 1.f) < 1e-4f;
            blended = blended || (s.weights[v * 4u] > 0.4f && s.weights[v * 4u] < 0.9f && s.weights[v * 4u + 1u] > 0.1f);
        }
        check(sums, "imported skin weights sum to 1");
        check(twoJoints, "simple_skin uses joints 0 and 1");
        check(blended, "simple_skin has blended vertices");
    } else {
        std::printf("  simple_skin.gltf not found, skin import check skipped\n");
    }
#else
    std::printf("  assimp not linked, import checks skipped\n");
#endif
}

} // namespace

int main() {
    test_v1_compatibility();
    test_v2_full_precision_round_trip();
    test_v2_quantised_round_trip();
    test_v2_rejections();
    test_assimp_import();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_asset_fmsh_v2: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_asset_fmsh_v2: all checks passed\n");
    return EXIT_SUCCESS;
}
