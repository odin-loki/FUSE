// E18 (GAP-PHYS-HULL-MESH part 2): the collision cook, FMSH -> .fusecol.
//   * whole-mesh hull contains every vertex; --hull-per-submesh gives one hull per part
//   * the triangle mesh keeps every triangle; its BVH ray casts equal brute force
//   * deterministic bytes; the runtime loader (fuse::physics::readCollisionAssetFile) parses them back
//     byte-identically; hull-only / mesh-only modes
//   * `fuse_cook --collision` CLI (argv[1] = fuse_cook path) writes the same bytes as the API
#include <fuse/cook/collision_cook.hpp>
#include <fuse/cook/mesh_cook.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u8;
using fuse::cook::CookedMesh;
namespace physics = fuse::physics;

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::vector<u8> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void add_box(CookedMesh& mesh, f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz) {
    const u32 base = mesh.vertex_count();
    CookedMesh::Submesh sub{};
    sub.index_offset = static_cast<u32>(mesh.indices.size());
    sub.vertex_offset = base;
    for (u32 c = 0; c < 8u; ++c) {
        mesh.positions.push_back(cx + ((c & 1u) != 0u ? hx : -hx));
        mesh.positions.push_back(cy + ((c & 2u) != 0u ? hy : -hy));
        mesh.positions.push_back(cz + ((c & 4u) != 0u ? hz : -hz));
        mesh.normals.insert(mesh.normals.end(), {0.f, 1.f, 0.f});
        mesh.uvs.insert(mesh.uvs.end(), {0.f, 0.f});
    }
    const u32 faces[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
    for (const auto& f : faces) {
        mesh.indices.insert(mesh.indices.end(), {base + f[0], base + f[1], base + f[2], base + f[0], base + f[2], base + f[3]});
    }
    sub.index_count = static_cast<u32>(mesh.indices.size()) - sub.index_offset;
    mesh.submeshes.push_back(sub);
}

/// A two-part prop: a table top and a leg (two boxes, two submeshes).
CookedMesh make_prop() {
    CookedMesh mesh;
    add_box(mesh, 0.f, 1.f, 0.f, 1.f, 0.05f, 0.6f);
    add_box(mesh, 0.f, 0.475f, 0.f, 0.08f, 0.475f, 0.08f);
    for (int axis = 0; axis < 3; ++axis) {
        mesh.bounds_min[axis] = 1e30f;
        mesh.bounds_max[axis] = -1e30f;
    }
    for (u32 v = 0; v < mesh.vertex_count(); ++v) {
        for (u32 axis = 0; axis < 3u; ++axis) {
            mesh.bounds_min[axis] = std::min(mesh.bounds_min[axis], mesh.positions[v * 3u + axis]);
            mesh.bounds_max[axis] = std::max(mesh.bounds_max[axis], mesh.positions[v * 3u + axis]);
        }
    }
    return mesh;
}

void test_cook() {
    const CookedMesh prop = make_prop();
    physics::CollisionAsset whole;
    fuse::cook::CollisionCookReport report{};
    std::string error;
    check(fuse::cook::cook_collision(prop, {}, whole, &report, &error), "cook_collision (both)");
    check(whole.hulls.size() == 1u && whole.meshes.size() == 1u, "default: one hull and one triangle mesh");
    if (!whole.hulls.empty()) {
        const physics::HullView view = whole.hulls[0].view();
        bool inside = true;
        for (u32 v = 0; v < prop.vertex_count(); ++v) {
            const physics::vec3 p{prop.positions[v * 3u], prop.positions[v * 3u + 1u], prop.positions[v * 3u + 2u]};
            inside = inside && physics::hullMaxFaceDistance(view, p) <= 1e-5f;
        }
        check(inside, "the whole-mesh hull contains every vertex");
    }
    check(!whole.meshes.empty() && whole.meshes[0].triangleCount() == prop.indices.size() / 3u,
          "the triangle mesh keeps every triangle");
    std::printf("collision cook (both): %u hull(s), %u hull vertices, %u triangles, BVH %u nodes depth %u\n",
                report.hull_count, report.hull_vertices, report.mesh_triangles, report.bvh_nodes, report.bvh_depth);

    fuse::cook::CollisionCookOptions parts{};
    parts.mode = fuse::cook::CollisionCookMode::Hull;
    parts.hull_per_submesh = true;
    physics::CollisionAsset pieces;
    check(fuse::cook::cook_collision(prop, parts, pieces, nullptr, &error) && pieces.hulls.size() == 2u &&
              pieces.meshes.empty() && pieces.hulls[0].vertices.size() == 8u && pieces.hulls[1].vertices.size() == 8u,
          "--hull-per-submesh: one 8-vertex hull per box part, no mesh");

    fuse::cook::CollisionCookOptions meshOnly{};
    meshOnly.mode = fuse::cook::CollisionCookMode::TriMesh;
    physics::CollisionAsset level;
    check(fuse::cook::cook_collision(prop, meshOnly, level, nullptr, &error) && level.hulls.empty() &&
              level.meshes.size() == 1u,
          "mesh mode: triangle mesh only");

    // Deterministic bytes; the runtime parser reads them back byte-identically.
    const std::vector<u8> a = physics::serializeCollisionAsset(whole);
    physics::CollisionAsset again;
    fuse::cook::cook_collision(prop, {}, again);
    const std::vector<u8> b = physics::serializeCollisionAsset(again);
    physics::CollisionAsset parsed;
    const bool ok = physics::deserializeCollisionAsset(a.data(), a.size(), parsed, &error);
    check(a == b, "collision cook bytes are deterministic");
    check(ok && physics::serializeCollisionAsset(parsed) == a, "the runtime loader parses the cooked bytes back");

    // BVH ray casts equal brute force on the cooked mesh.
    u32 mismatch = 0;
    for (u32 i = 0; i < 400u; ++i) {
        const f32 x = -1.2f + 2.4f * static_cast<f32>(i % 20u) / 19.f;
        const f32 z = -0.8f + 1.6f * static_cast<f32>(i / 20u) / 19.f;
        physics::TriMeshRayHit h1{};
        physics::TriMeshRayHit h2{};
        const bool r1 = whole.meshes[0].rayCast({x, 3.f, z}, {0.f, -1.f, 0.f}, 10.f, h1);
        const bool r2 = whole.meshes[0].rayCastBruteForce({x, 3.f, z}, {0.f, -1.f, 0.f}, 10.f, h2);
        mismatch += (r1 != r2 || (r1 && (h1.t != h2.t || h1.triangle != h2.triangle))) ? 1u : 0u;
    }
    check(mismatch == 0u, "cooked BVH ray casts == brute force");

    // A mesh without triangles is refused.
    CookedMesh empty;
    physics::CollisionAsset none;
    check(!fuse::cook::cook_collision(empty, {}, none), "a mesh without triangles is refused");
}

void test_files_and_cli(const char* fuse_cook) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_collision_cook_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string fmsh = (dir / "prop.fusemesh").string();
    {
        const std::vector<u8> bytes = fuse::cook::serialize_cooked_mesh(make_prop());
        std::ofstream file(fmsh, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const std::string api = (dir / "api.fusecol").string();
    fuse::cook::CollisionCookReport report{};
    const fuse::cook::CookStubWriteResult cooked = fuse::cook::cook_collision_file(fmsh, api, {}, &report);
    check(cooked.ok && report.bytes > 0u, "cook_collision_file writes a .fusecol");
    physics::CollisionAsset loaded;
    std::string error;
    check(physics::readCollisionAssetFile(api, loaded, &error) && loaded.hulls.size() == 1u && loaded.meshes.size() == 1u,
          "the .fusecol file loads");
    check(!fuse::cook::cook_collision_file((dir / "missing.fusemesh").string(), api).ok, "a missing FMSH fails the cook");

    if (fuse_cook == nullptr) {
        std::printf("  fuse_cook CLI check skipped (no executable path given)\n");
        return;
    }
    const std::string cli = (dir / "cli.fusecol").string();
    const std::string command =
        "\"" + std::string(fuse_cook) + "\" --collision --input \"" + fmsh + "\" --output \"" + cli + "\"";
    check(std::system(command.c_str()) == 0, "fuse_cook --collision");
    check(read_file(cli) == read_file(api), "fuse_cook --collision writes the API's bytes");
    const std::string hulls = (dir / "hulls.fusecol").string();
    const std::string partsCommand = "\"" + std::string(fuse_cook) + "\" --collision --collision-mode hull --hull-per-submesh --input \"" +
                                     fmsh + "\" --output \"" + hulls + "\"";
    physics::CollisionAsset parts;
    check(std::system(partsCommand.c_str()) == 0 && physics::readCollisionAssetFile(hulls, parts) &&
              parts.hulls.size() == 2u && parts.meshes.empty(),
          "fuse_cook --collision --collision-mode hull --hull-per-submesh");
    std::filesystem::remove_all(dir);
}

} // namespace

int main(int argc, char** argv) {
    test_cook();
    test_files_and_cli(argc > 1 ? argv[1] : nullptr);
    if (g_failures == 0) {
        std::printf("fuse_asset_collision_cook: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_asset_collision_cook: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
