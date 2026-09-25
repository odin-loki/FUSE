// Asset plan W0.2 gate (docs/plans/FUSE_ASSET_PLAN.md §1.4, §5.1, §5.3, §6 Wave 0): meshoptimizer in the
// mesh cook. Suites (argv[1]):
//   lod       discrete LOD chain: error strictly increasing per level, every level cuts >= 40 % of the
//             previous level's triangles (§5.3 asset_budget_mesh), deterministic, LOD table round trip;
//   meshlets  the FMSH meshlet table equals what the renderer's WP-1.2 cook (build_meshlets_from_cooked,
//             the `.fusemeshlet` sidecar path) produces for the same mesh: SUBM / MSHL / MTRI sections
//             byte-identical to the FMLT chunks, MVRT equal after mapping through VSRC;
//   codec     meshopt vertex / index codec: round trip bit-exact against the raw encoding for every
//             stream format, size reduction reported, corrupt payloads refused;
//   dag       cluster DAG section equals the renderer's WP-5.2 FMLT 1.1 chunks, round trips (raw and
//             codec), corrupt DAGs refused;
//   compat    W0.1 v2 / v1 files unchanged and loading, unknown sections skipped, reserved flags
//             refused, cook_mesh_file end to end.
#include <fuse/cook/mesh_cook.hpp>

#if defined(FUSE_COOK_TEST_HAS_GEOMETRY)
#include <fuse/renderer/geometry/dag/cluster_dag.hpp>
#include <fuse/renderer/geometry/dag/fmlt_chunks.hpp>
#include <fuse/renderer/geometry/meshlet_cook_hook.hpp>
#endif

#if defined(FUSE_COOK_TEST_HAS_MESHOPT)
#include <meshoptimizer.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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

u32 le32(const std::vector<u8>& b, usize at) {
    return static_cast<u32>(b[at]) | (static_cast<u32>(b[at + 1]) << 8) | (static_cast<u32>(b[at + 2]) << 16) |
           (static_cast<u32>(b[at + 3]) << 24);
}

void set32(std::vector<u8>& b, usize at, u32 v) {
    for (u32 i = 0; i < 4u; ++i) {
        b[at + i] = static_cast<u8>((v >> (i * 8u)) & 0xFFu);
    }
}

std::vector<u8> reseal(std::vector<u8> bytes) {
    u64 hash = 14695981039346656037ull;
    for (usize i = 0; i + 8u < bytes.size(); ++i) {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    for (u32 i = 0; i < 8u; ++i) {
        bytes[bytes.size() - 8u + i] = static_cast<u8>((hash >> (i * 8u)) & 0xFFu);
    }
    return bytes;
}

constexpr u32 fourcc(const char* s) {
    return static_cast<u32>(static_cast<u8>(s[0])) | (static_cast<u32>(static_cast<u8>(s[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(s[2])) << 16) | (static_cast<u32>(static_cast<u8>(s[3])) << 24);
}

/// Payload of FMSH section `id` (located by walking the section table, which starts right after the
/// index list; the caller passes the table offset found by `section_table_offset`).
struct SectionLoc {
    usize header = 0; ///< offset of the 12-byte section header
    u32 element_bytes = 0;
    u32 element_count = 0;
};

/// Walk the FMSH v2 layout to the section table (flags bit 5). Returns 0 when absent.
usize section_table_offset(const std::vector<u8>& b) {
    const u32 flags = le32(b, 8);
    if ((flags & (1u << 5)) == 0u) {
        return 0;
    }
    const bool codec = (flags & (1u << 4)) != 0u;
    const u32 indexCount = le32(b, 16);
    const u32 submeshCount = le32(b, 20);
    const u32 streamCount = le32(b, 48);
    const u32 slotCount = le32(b, 52);
    usize c = 56u + 16u * submeshCount;
    for (u32 s = 0; s < slotCount; ++s) {
        c += 4u + ((le32(b, c) + 3u) & ~3u);
    }
    for (u32 s = 0; s < streamCount; ++s) {
        const u32 stored = codec ? le32(b, c + 12u) : le32(b, c + 8u);
        c += (codec ? 16u : 12u) + ((stored + 3u) & ~3u);
    }
    if (codec) {
        c += 4u + ((le32(b, c) + 3u) & ~3u);
    } else {
        c += static_cast<usize>(indexCount) * 4u;
    }
    return c;
}

bool find_section(const std::vector<u8>& b, u32 id, SectionLoc& loc) {
    usize c = section_table_offset(b);
    if (c == 0u) {
        return false;
    }
    const u32 count = le32(b, c);
    c += 4u;
    for (u32 s = 0; s < count; ++s) {
        const u32 eb = le32(b, c + 4u);
        const u32 ec = le32(b, c + 8u);
        if (le32(b, c) == id) {
            loc = {c, eb, ec};
            return true;
        }
        c += 12u + ((static_cast<usize>(eb) * ec + 3u) & ~static_cast<usize>(3u));
    }
    return false;
}

std::vector<u8> section_payload(const std::vector<u8>& b, u32 id) {
    SectionLoc loc;
    if (!find_section(b, id, loc)) {
        return {};
    }
    const usize at = loc.header + 12u;
    return std::vector<u8>(b.begin() + static_cast<std::ptrdiff_t>(at),
                           b.begin() + static_cast<std::ptrdiff_t>(at + static_cast<usize>(loc.element_bytes) * loc.element_count));
}

void compute_bounds(CookedMesh& mesh) {
    for (u32 axis = 0; axis < 3u; ++axis) {
        mesh.bounds_min[axis] = 1e30f;
        mesh.bounds_max[axis] = -1e30f;
        for (u32 v = 0; v < mesh.vertex_count(); ++v) {
            mesh.bounds_min[axis] = std::min(mesh.bounds_min[axis], mesh.positions[v * 3u + axis]);
            mesh.bounds_max[axis] = std::max(mesh.bounds_max[axis], mesh.positions[v * 3u + axis]);
        }
    }
}

void compute_normals(CookedMesh& mesh) {
    mesh.normals.assign(mesh.positions.size(), 0.f);
    for (usize t = 0; t + 2u < mesh.indices.size(); t += 3u) {
        const f32* a = &mesh.positions[mesh.indices[t] * 3u];
        const f32* b = &mesh.positions[mesh.indices[t + 1u] * 3u];
        const f32* c = &mesh.positions[mesh.indices[t + 2u] * 3u];
        const f32 e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const f32 e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const f32 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (u32 k = 0; k < 3u; ++k) {
            for (u32 d = 0; d < 3u; ++d) {
                mesh.normals[mesh.indices[t + k] * 3u + d] += n[d];
            }
        }
    }
    for (usize v = 0; v < mesh.normals.size(); v += 3u) {
        f32* n = &mesh.normals[v];
        const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (u32 d = 0; d < 3u; ++d) {
            n[d] = len > 0.f ? n[d] / len : (d == 2u ? 1.f : 0.f);
        }
    }
}

/// A bumpy closed torus ("rock"): no seams, no poles; two submeshes (halves of the triangle list).
CookedMesh make_rock(u32 rings, u32 sides, bool full_streams) {
    CookedMesh mesh;
    const f32 pi2 = 6.283185307f;
    for (u32 i = 0; i < rings; ++i) {
        for (u32 j = 0; j < sides; ++j) {
            const f32 u = pi2 * static_cast<f32>(i) / static_cast<f32>(rings);
            const f32 v = pi2 * static_cast<f32>(j) / static_cast<f32>(sides);
            const f32 bump = 0.08f * std::sin(5.f * u) * std::cos(3.f * v) + 0.03f * std::sin(13.f * u + 7.f * v);
            const f32 r = 0.45f + bump;
            mesh.positions.insert(mesh.positions.end(), {(1.5f + r * std::cos(v)) * std::cos(u),
                                                         (1.5f + r * std::cos(v)) * std::sin(u), r * std::sin(v) + 2.f});
            mesh.uvs.insert(mesh.uvs.end(), {static_cast<f32>(i) / static_cast<f32>(rings) * 4.f,
                                             static_cast<f32>(j) / static_cast<f32>(sides)});
        }
    }
    for (u32 i = 0; i < rings; ++i) {
        for (u32 j = 0; j < sides; ++j) {
            const u32 a = i * sides + j;
            const u32 b = ((i + 1u) % rings) * sides + j;
            const u32 c = ((i + 1u) % rings) * sides + (j + 1u) % sides;
            const u32 d = i * sides + (j + 1u) % sides;
            mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
        }
    }
    compute_normals(mesh);
    const u32 half = static_cast<u32>(mesh.indices.size() / 2u / 3u * 3u);
    mesh.submeshes.push_back({0u, half, 0u, 0u});
    mesh.submeshes.push_back({half, static_cast<u32>(mesh.indices.size()) - half, 0u, 1u});
    if (full_streams) {
        const u32 n = mesh.vertex_count();
        for (u32 v = 0; v < n; ++v) {
            const f32* nn = &mesh.normals[v * 3u];
            // Tangent: normalize(cross(n, z)) (fallback x), sign alternating by ring.
            f32 t[3] = {nn[1], -nn[0], 0.f};
            f32 tl = std::sqrt(t[0] * t[0] + t[1] * t[1]);
            if (tl < 1e-4f) {
                t[0] = 1.f;
                t[1] = 0.f;
                tl = 1.f;
            }
            mesh.tangents.insert(mesh.tangents.end(), {t[0] / tl, t[1] / tl, 0.f, ((v / 7u) % 2u) ? -1.f : 1.f});
            mesh.uv1s.insert(mesh.uv1s.end(), {mesh.uvs[v * 2u] * 0.25f, mesh.uvs[v * 2u + 1u] * 0.5f + 0.25f});
            mesh.colors.insert(mesh.colors.end(), {static_cast<u8>(v & 0xFFu), static_cast<u8>((v >> 3) & 0xFFu), 128u, 255u});
            const u16 j0 = static_cast<u16>((v * 7u) % 300u); // > 255: u16 joints
            mesh.joints.insert(mesh.joints.end(), {j0, static_cast<u16>(j0 + 1u), 0u, 0u});
            const f32 w = 0.25f + 0.5f * static_cast<f32>(v % 11u) / 10.f;
            mesh.weights.insert(mesh.weights.end(), {w, 1.f - w, 0.f, 0.f});
        }
        mesh.material_slots = {"rock_base", "rock_moss"};
    }
    compute_bounds(mesh);
    return mesh;
}

/// A noisy heightfield grid (open border).
CookedMesh make_terrain(u32 n) {
    CookedMesh mesh;
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(n - 1u);
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(n - 1u);
            const f32 h = 0.6f * std::sin(fx * 7.f) * std::cos(fy * 5.f) + 0.1f * std::sin(fx * 31.f + fy * 17.f);
            mesh.positions.insert(mesh.positions.end(), {fx * 40.f, fy * 40.f, h * 4.f});
            mesh.uvs.insert(mesh.uvs.end(), {fx, fy});
        }
    }
    for (u32 y = 0; y + 1u < n; ++y) {
        for (u32 x = 0; x + 1u < n; ++x) {
            const u32 i = y * n + x;
            mesh.indices.insert(mesh.indices.end(), {i, i + 1u, i + n, i + 1u, i + n + 1u, i + n});
        }
    }
    compute_normals(mesh);
    mesh.submeshes.push_back({0u, static_cast<u32>(mesh.indices.size()), 0u, 0u});
    compute_bounds(mesh);
    return mesh;
}

bool bits_equal(const std::vector<f32>& a, const std::vector<f32>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0);
}

bool meshlet_equal(const CookedMeshlet& a, const CookedMeshlet& b) {
    return a.vertex_offset == b.vertex_offset && a.triangle_offset == b.triangle_offset &&
           a.vertex_count == b.vertex_count && a.triangle_count == b.triangle_count && a.submesh == b.submesh &&
           std::memcmp(a.center, b.center, sizeof(a.center)) == 0 && std::memcmp(&a.radius, &b.radius, 4) == 0 &&
           std::memcmp(a.cone_apex, b.cone_apex, sizeof(a.cone_apex)) == 0 &&
           std::memcmp(a.cone_axis, b.cone_axis, sizeof(a.cone_axis)) == 0 &&
           std::memcmp(&a.cone_cutoff, &b.cone_cutoff, 4) == 0 &&
           std::memcmp(a.cone_axis_s8, b.cone_axis_s8, sizeof(a.cone_axis_s8)) == 0 &&
           a.cone_cutoff_s8 == b.cone_cutoff_s8 && std::memcmp(a.aabb_min, b.aabb_min, sizeof(a.aabb_min)) == 0 &&
           std::memcmp(a.aabb_max, b.aabb_max, sizeof(a.aabb_max)) == 0;
}

bool tables_equal(const CookedMesh& a, const CookedMesh& b) {
    if (a.lods.size() != b.lods.size() || a.lod_indices != b.lod_indices) {
        return false;
    }
    for (usize i = 0; i < a.lods.size(); ++i) {
        if (std::memcmp(&a.lods[i].error, &b.lods[i].error, 4) != 0 || a.lods[i].target_ratio != b.lods[i].target_ratio ||
            a.lods[i].ranges.size() != b.lods[i].ranges.size()) {
            return false;
        }
        for (usize r = 0; r < a.lods[i].ranges.size(); ++r) {
            if (a.lods[i].ranges[r].index_offset != b.lods[i].ranges[r].index_offset ||
                a.lods[i].ranges[r].index_count != b.lods[i].ranges[r].index_count) {
                return false;
            }
        }
    }
    const MeshletTable& ma = a.meshlets;
    const MeshletTable& mb = b.meshlets;
    if (ma.max_vertices != mb.max_vertices || ma.max_triangles != mb.max_triangles || ma.vertices != mb.vertices ||
        ma.triangles != mb.triangles || ma.meshlets.size() != mb.meshlets.size() || ma.submeshes.size() != mb.submeshes.size()) {
        return false;
    }
    for (usize i = 0; i < ma.meshlets.size(); ++i) {
        if (!meshlet_equal(ma.meshlets[i], mb.meshlets[i])) {
            return false;
        }
    }
    for (usize i = 0; i < ma.submeshes.size(); ++i) {
        if (std::memcmp(&ma.submeshes[i], &mb.submeshes[i], sizeof(MeshletTable::SubmeshRange)) != 0) {
            return false;
        }
    }
    const ClusterDagTable& da = a.cluster_dag;
    const ClusterDagTable& db = b.cluster_dag;
    if (da.leaf_cluster_count != db.leaf_cluster_count || da.level_count != db.level_count ||
        da.lod_vertices != db.lod_vertices || da.lod_triangles != db.lod_triangles ||
        da.group_members != db.group_members || da.lod_clusters.size() != db.lod_clusters.size() ||
        da.groups.size() != db.groups.size() || da.links.size() != db.links.size()) {
        return false;
    }
    for (usize i = 0; i < da.lod_clusters.size(); ++i) {
        if (!meshlet_equal(da.lod_clusters[i], db.lod_clusters[i])) {
            return false;
        }
    }
    // Group / Link are padding-free u32 / f32 records.
    return (da.groups.empty() || std::memcmp(da.groups.data(), db.groups.data(), da.groups.size() * sizeof(ClusterDagTable::Group)) == 0) &&
           (da.links.empty() || std::memcmp(da.links.data(), db.links.data(), da.links.size() * sizeof(ClusterDagTable::Link)) == 0);
}

bool meshes_equal(const CookedMesh& a, const CookedMesh& b) {
    return bits_equal(a.positions, b.positions) && bits_equal(a.normals, b.normals) && bits_equal(a.uvs, b.uvs) &&
           bits_equal(a.tangents, b.tangents) && bits_equal(a.uv1s, b.uv1s) && a.colors == b.colors &&
           a.joints == b.joints && bits_equal(a.weights, b.weights) && a.indices == b.indices &&
           a.material_slots == b.material_slots && a.submeshes.size() == b.submeshes.size() &&
           std::memcmp(a.bounds_min, b.bounds_min, sizeof(a.bounds_min)) == 0 &&
           std::memcmp(a.bounds_max, b.bounds_max, sizeof(a.bounds_max)) == 0 && tables_equal(a, b);
}

u64 lod_triangles(const MeshLod& lod) {
    u64 total = 0;
    for (const MeshLod::Range& r : lod.ranges) {
        total += r.index_count / 3u;
    }
    return total;
}

// ---- suites ------------------------------------------------------------------------------------------

void check_lod_chain(const char* name, CookedMesh mesh, const MeshLodOptions& options) {
    std::string error;
    check(build_mesh_lods(mesh, options, &error), std::string(name) + ": build_mesh_lods: " + error);
    check(mesh.lods.size() == options.ratios.size(), std::string(name) + ": every LOD level kept (" +
                                                         std::to_string(mesh.lods.size()) + ")");
    u64 previous = mesh.indices.size() / 3u;
    f32 previousError = 0.f;
    const f32 fov = 1.0471976f; // 60 deg
    std::printf("[lod] %s: LOD0 %llu tris\n", name, static_cast<unsigned long long>(previous));
    u32 expectedOffset = 0;
    for (usize i = 0; i < mesh.lods.size(); ++i) {
        const MeshLod& lod = mesh.lods[i];
        const u64 tris = lod_triangles(lod);
        const f64 cut = 1.0 - static_cast<f64>(tris) / static_cast<f64>(previous);
        std::printf("[lod] %s: LOD%zu ratio %.2f  %llu tris  cut %.1f %%  error %.6g  1px@1080p switch %.2f\n", name,
                    i + 1u, static_cast<f64>(lod.target_ratio), static_cast<unsigned long long>(tris), cut * 100.0,
                    static_cast<f64>(lod.error), static_cast<f64>(lod_switch_distance(lod.error, fov, 1080.f)));
        check(cut >= 0.4, std::string(name) + ": LOD" + std::to_string(i + 1u) + " cuts >= 40 % of the previous level");
        check(lod.error > previousError && std::isfinite(lod.error),
              std::string(name) + ": LOD" + std::to_string(i + 1u) + " error strictly greater than the previous level");
        check(lod.ranges.size() == mesh.submeshes.size(), std::string(name) + ": one range per submesh");
        for (const MeshLod::Range& r : lod.ranges) {
            check(r.index_offset == expectedOffset && r.index_count % 3u == 0u, std::string(name) + ": ranges tile LOD indices");
            expectedOffset += r.index_count;
        }
        previous = tris;
        previousError = lod.error;
    }
    check(expectedOffset == mesh.lod_indices.size(), std::string(name) + ": LOD indices fully covered");
    for (u32 index : mesh.lod_indices) {
        if (index >= mesh.vertex_count()) {
            check(false, std::string(name) + ": LOD index in range");
            break;
        }
    }
    // Deterministic + round trip.
    CookedMesh again = mesh;
    check(build_mesh_lods(again, options), std::string(name) + ": rebuild");
    const std::vector<u8> bytes = serialize_cooked_mesh(mesh);
    check(serialize_cooked_mesh(again) == bytes, std::string(name) + ": LOD build deterministic");
    CookedMesh loaded;
    check(deserialize_cooked_mesh(bytes.data(), bytes.size(), loaded, &error), std::string(name) + ": LOD mesh loads: " + error);
    check(meshes_equal(mesh, loaded), std::string(name) + ": LOD table round trip");
}

void suite_lod() {
    check(mesh_optimizer_available(), "meshoptimizer linked into the cook");
    check_lod_chain("rock", make_rock(96, 64, false), MeshLodOptions{});
    check_lod_chain("terrain", make_terrain(129), MeshLodOptions{});
    MeshLodOptions locked;
    locked.lock_border = true;
    check_lod_chain("terrain_lock_border", make_terrain(129), locked);
    MeshLodOptions positionsOnly;
    positionsOnly.normal_weight = 0.f;
    check_lod_chain("rock_positions_only", make_rock(96, 64, false), positionsOnly);

    // Locked border: every open-border vertex used by LOD 0 is still used by every LOD.
    {
        CookedMesh mesh = make_terrain(65);
        check(build_mesh_lods(mesh, locked), "terrain 65 lock border build");
        std::vector<u8> used(mesh.vertex_count(), 0u);
        for (u32 index : mesh.lod_indices) {
            used[index] = 1u;
        }
        bool bordersKept = true;
        for (u32 x = 0; x < 65u; ++x) {
            bordersKept = bordersKept && used[x] != 0u && used[64u * 65u + x] != 0u && used[x * 65u] != 0u;
        }
        check(bordersKept || mesh.lods.size() == 0u, "lock_border keeps border vertices");
    }
    // A mesh that cannot shed 40 %: the chain is empty (the §5.3 gate then reports the missing LODs).
    {
        CookedMesh tri;
        tri.positions = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
        tri.uvs = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f};
        tri.indices = {0u, 1u, 2u};
        compute_normals(tri);
        tri.submeshes.push_back({0u, 3u, 0u, 0u});
        compute_bounds(tri);
        check(build_mesh_lods(tri), "single triangle LOD build succeeds");
        check(tri.lods.empty() && tri.lod_indices.empty(), "single triangle: no LOD level (cannot cut 40 %)");
        check(serialize_cooked_mesh(tri)[4] == 1u, "mesh without W0.2 content still serializes as v1");
    }
    MeshLodOptions bad;
    bad.ratios = {1.5f};
    CookedMesh rock = make_rock(16, 16, false);
    check(!build_mesh_lods(rock, bad) && rock.lods.empty(), "ratio outside (0, 1) refused");
}

#if defined(FUSE_COOK_TEST_HAS_GEOMETRY)
namespace geo = fuse::renderer::geometry;

/// Payload of FMLT chunk `id`.
std::vector<u8> fmlt_payload(const std::vector<u8>& fmlt, u32 id) {
    geo::dag::FmltFile file;
    if (!geo::dag::split_fmlt(fmlt.data(), fmlt.size(), file)) {
        return {};
    }
    const s32 at = geo::dag::fmlt_find_chunk(file, id);
    return at < 0 ? std::vector<u8>{} : file.chunks[static_cast<usize>(at)].payload;
}

std::vector<u8> u32_bytes(const std::vector<u32>& values) {
    std::vector<u8> out;
    for (u32 v : values) {
        for (u32 s = 0; s < 32u; s += 8u) {
            out.push_back(static_cast<u8>((v >> s) & 0xFFu));
        }
    }
    return out;
}

void check_meshlets_match(const char* name, CookedMesh mesh) {
    std::string error;
    check(build_mesh_meshlets(mesh, false, &error), std::string(name) + ": build_mesh_meshlets: " + error);
    // Renderer WP-1.2 cook of the same mesh (the sidecar path).
    geo::MeshletMesh renderer;
    check(geo::build_meshlets_from_cooked(mesh, geo::MeshletBuildOptions{}, renderer, &error),
          std::string(name) + ": renderer build: " + error);
    const std::vector<u8> fmlt = geo::serialize_meshlet_mesh(renderer);
    const std::vector<u8> fmsh = serialize_cooked_mesh(mesh);
    for (const char* id : {"SUBM", "MSHL", "MTRI"}) {
        const std::vector<u8> ours = section_payload(fmsh, fourcc(id));
        check(!ours.empty() && ours == fmlt_payload(fmlt, fourcc(id)),
              std::string(name) + ": FMSH " + id + " byte-identical to the renderer's .fusemeshlet chunk");
    }
    std::vector<u32> mapped;
    for (u32 v : renderer.meshlet_vertices) {
        mapped.push_back(renderer.source_vertices[v]);
    }
    check(mesh.meshlets.vertices == mapped, std::string(name) + ": MVRT == renderer MVRT through VSRC");
    check(section_payload(fmsh, fourcc("MVRT")) == u32_bytes(mapped), std::string(name) + ": MVRT section bytes");
    check(mesh.meshlets.max_vertices == 64u && mesh.meshlets.max_triangles == 124u, std::string(name) + ": 64 v / 124 t limits");
    u32 worstV = 0;
    u32 worstT = 0;
    for (const CookedMeshlet& m : mesh.meshlets.meshlets) {
        worstV = std::max(worstV, m.vertex_count);
        worstT = std::max(worstT, m.triangle_count);
    }
    check(worstV <= 64u && worstT <= 124u, std::string(name) + ": meshlets within limits");
    // Each meshlet triangle, expanded through the table, is a triangle of the source submesh (same
    // multiset per submesh up to rotation).
    for (u32 s = 0; s < mesh.submeshes.size(); ++s) {
        auto canon = [](u32 a, u32 b, u32 c) {
            const u32 m = std::min({a, b, c});
            return m == a ? std::array<u32, 3>{a, b, c} : m == b ? std::array<u32, 3>{b, c, a} : std::array<u32, 3>{c, a, b};
        };
        std::vector<std::array<u32, 3>> src;
        std::vector<std::array<u32, 3>> fromMeshlets;
        const CookedMesh::Submesh& sub = mesh.submeshes[s];
        for (u32 i = 0; i < sub.index_count; i += 3u) {
            const u32* t = &mesh.indices[sub.index_offset + i];
            src.push_back(canon(t[0], t[1], t[2]));
        }
        for (const CookedMeshlet& m : mesh.meshlets.meshlets) {
            if (m.submesh != s) {
                continue;
            }
            for (u32 t = 0; t < m.triangle_count; ++t) {
                const u32 p = mesh.meshlets.triangles[m.triangle_offset + t];
                const u32* mv = &mesh.meshlets.vertices[m.vertex_offset];
                fromMeshlets.push_back(canon(mv[p & 0xFFu], mv[(p >> 8) & 0xFFu], mv[(p >> 16) & 0xFFu]));
            }
        }
        std::sort(src.begin(), src.end());
        std::sort(fromMeshlets.begin(), fromMeshlets.end());
        check(src == fromMeshlets, std::string(name) + ": meshlets cover exactly the submesh triangles");
    }
    CookedMesh loaded;
    check(deserialize_cooked_mesh(fmsh.data(), fmsh.size(), loaded, &error), std::string(name) + ": meshlet mesh loads: " + error);
    check(meshes_equal(mesh, loaded), std::string(name) + ": meshlet table round trip");
    std::printf("[meshlets] %s: %zu meshlets, %zu meshlet vertices, %zu triangles\n", name, mesh.meshlets.meshlets.size(),
                mesh.meshlets.vertices.size(), mesh.meshlets.triangles.size());
}

void suite_meshlets() {
    check(mesh_meshlets_available(), "renderer geometry libraries linked into the cook");
    check_meshlets_match("rock", make_rock(96, 64, false));
    check_meshlets_match("terrain", make_terrain(97));
    CookedMesh unused = make_terrain(33); // an unreferenced vertex: VSRC mapping must skip it
    unused.positions.insert(unused.positions.end(), {100.f, 100.f, 100.f});
    unused.normals.insert(unused.normals.end(), {0.f, 0.f, 1.f});
    unused.uvs.insert(unused.uvs.end(), {0.f, 0.f});
    compute_bounds(unused);
    check_meshlets_match("terrain_unused_vertex", unused);

    // Corrupt meshlet sections are refused.
    CookedMesh mesh = make_terrain(33);
    check(build_mesh_meshlets(mesh, false), "terrain 33 meshlets");
    const std::vector<u8> good = serialize_cooked_mesh(mesh);
    SectionLoc mvrt;
    SectionLoc mtri;
    SectionLoc mshl;
    check(find_section(good, fourcc("MVRT"), mvrt) && find_section(good, fourcc("MTRI"), mtri) &&
              find_section(good, fourcc("MSHL"), mshl),
          "sections located");
    CookedMesh out;
    std::vector<u8> bad = good;
    set32(bad, mvrt.header + 12u, mesh.vertex_count());
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "MVRT index out of range refused");
    bad = good;
    set32(bad, mtri.header + 12u, 0x00FFFFFFu);
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "micro-index out of range refused");
    bad = good;
    set32(bad, mshl.header + 12u + 96u, 1u); // second meshlet's vertex_offset breaks the running sum
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "unpacked meshlet offsets refused");
    bad = good;
    set32(bad, mshl.header, fourcc("MSHX")); // MSHL renamed: now unknown, the meshlet set is incomplete
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "incomplete meshlet sections refused");
}

void check_dag_match(const char* name, CookedMesh mesh) {
    std::string error;
    check(build_mesh_meshlets(mesh, true, &error), std::string(name) + ": build with DAG: " + error);
    geo::dag::ClusterDagMesh renderer;
    check(geo::build_meshlets_from_cooked(mesh, geo::MeshletBuildOptions{}, renderer.base, &error),
          std::string(name) + ": renderer meshlets: " + error);
    check(geo::dag::build_cluster_dag(renderer.base, geo::dag::DagBuildOptions{}, renderer.dag, &error),
          std::string(name) + ": renderer DAG: " + error);
    check(geo::dag::validate_cluster_dag(renderer.base, renderer.dag, &error), std::string(name) + ": renderer DAG valid: " + error);
    const std::vector<u8> fmlt = geo::dag::serialize_cluster_dag_mesh(renderer);
    const std::vector<u8> fmsh = serialize_cooked_mesh(mesh);
    for (const char* id : {"DAGH", "DMSH", "DMTR", "DGRP", "DGMB", "DCLK"}) {
        const std::vector<u8> ours = section_payload(fmsh, fourcc(id));
        check(!ours.empty() && ours == fmlt_payload(fmlt, fourcc(id)),
              std::string(name) + ": FMSH " + id + " byte-identical to the renderer's FMLT 1.1 chunk");
    }
    std::vector<u32> mapped;
    for (u32 v : renderer.dag.lod_meshlet_vertices) {
        mapped.push_back(renderer.base.source_vertices[v]);
    }
    check(mesh.cluster_dag.lod_vertices == mapped && section_payload(fmsh, fourcc("DMVR")) == u32_bytes(mapped),
          std::string(name) + ": DMVR == renderer DMVR through VSRC");
    for (bool codec : {false, true}) {
        MeshEncoding encoding;
        encoding.meshopt_codec = codec;
        const std::vector<u8> bytes = serialize_cooked_mesh(mesh, encoding);
        CookedMesh loaded;
        check(deserialize_cooked_mesh(bytes.data(), bytes.size(), loaded, &error),
              std::string(name) + (codec ? ": codec" : ": raw") + " DAG mesh loads: " + error);
        check(meshes_equal(mesh, loaded), std::string(name) + (codec ? ": codec" : ": raw") + " DAG round trip");
        if (!meshes_equal(mesh, loaded)) {
            std::fprintf(stderr, "pos %d nrm %d uv %d idx %d tables %d\n", bits_equal(mesh.positions, loaded.positions),
                         bits_equal(mesh.normals, loaded.normals), bits_equal(mesh.uvs, loaded.uvs), mesh.indices == loaded.indices,
                         tables_equal(mesh, loaded));
        }
        check(serialize_cooked_mesh(loaded, encoding) == bytes, std::string(name) + ": DAG re-serializes identically");
    }
    std::printf("[dag] %s: %u leaves, %zu LOD clusters, %zu groups, %u levels\n", name, mesh.cluster_dag.leaf_cluster_count,
                mesh.cluster_dag.lod_clusters.size(), mesh.cluster_dag.groups.size(), mesh.cluster_dag.level_count);
}

void suite_dag() {
    check_dag_match("rock", make_rock(96, 64, false));
    check_dag_match("terrain", make_terrain(97));

    CookedMesh mesh = make_terrain(65);
    check(build_mesh_meshlets(mesh, true), "terrain 65 DAG");
    const std::vector<u8> good = serialize_cooked_mesh(mesh);
    SectionLoc dgmb;
    SectionLoc dclk;
    SectionLoc dagh;
    check(find_section(good, fourcc("DGMB"), dgmb) && find_section(good, fourcc("DCLK"), dclk) &&
              find_section(good, fourcc("DAGH"), dagh),
          "DAG sections located");
    CookedMesh out;
    std::vector<u8> bad = good;
    set32(bad, dgmb.header + 12u, le32(good, dgmb.header + 16u)); // a cluster listed twice
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "cluster in two groups refused");
    bad = good;
    set32(bad, dclk.header + 12u + 40u, 0xFFFFFFF0u); // link group out of range
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "link to a missing group refused");
    bad = good;
    set32(bad, dagh.header + 12u, 1u); // leaf count disagrees with the meshlet table
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "DAGH leaf count mismatch refused");
    // DAG without meshlets is refused.
    CookedMesh noMeshlets = mesh;
    noMeshlets.meshlets = MeshletTable{};
    const std::vector<u8> orphan = serialize_cooked_mesh(noMeshlets);
    check(!deserialize_cooked_mesh(orphan.data(), orphan.size(), out), "DAG without meshlets refused");
}
#endif

void codec_case(const char* name, const CookedMesh& mesh, const MeshEncoding& base) {
    MeshEncoding codec = base;
    codec.meshopt_codec = true;
    const std::vector<u8> raw = serialize_cooked_mesh(mesh, base);
    const std::vector<u8> enc = serialize_cooked_mesh(mesh, codec);
    CookedMesh fromRaw;
    CookedMesh fromEnc;
    std::string error;
    check(deserialize_cooked_mesh(raw.data(), raw.size(), fromRaw, &error), std::string(name) + ": raw loads: " + error);
    check(deserialize_cooked_mesh(enc.data(), enc.size(), fromEnc, &error), std::string(name) + ": codec loads: " + error);
    check((le32(enc, 8) & (1u << 4)) != 0u, std::string(name) + ": codec flag set");
    check(meshes_equal(fromRaw, fromEnc), std::string(name) + ": codec decode bit-exact vs raw decode");
    check(serialize_cooked_mesh(mesh, codec) == enc, std::string(name) + ": codec encoding deterministic");
#if defined(FUSE_COOK_TEST_HAS_MESHOPT)
    // Independent of the cook's reader: decode every codec payload with meshoptimizer directly and
    // compare with the raw file's payload bytes.
    {
        const u32 vertexCount = le32(raw, 12);
        const u32 indexCount = le32(raw, 16);
        // Raw payload (offset, element bytes, semantic) per stream; v1 raw files have the fixed layout.
        struct RawStream {
            usize at;
            u32 element;
        };
        std::vector<RawStream> rawStreams;
        usize rc = 0;
        if (le32(raw, 4) == 1u) {
            rc = 48u + 16u * le32(raw, 20);
            for (u32 element : {12u, 12u, 8u}) {
                rawStreams.push_back({rc, element});
                rc += static_cast<usize>(vertexCount) * element;
            }
        } else {
            rc = 56u + 16u * le32(raw, 20);
            for (u32 slot = 0; slot < le32(raw, 52); ++slot) {
                rc += 4u + ((le32(raw, rc) + 3u) & ~3u);
            }
            for (u32 st = 0; st < le32(raw, 48); ++st) {
                const u32 bytes = le32(raw, rc + 8u);
                rawStreams.push_back({rc + 12u, bytes / std::max(vertexCount, 1u)});
                rc += 12u + ((bytes + 3u) & ~3u);
            }
        }
        check(le32(enc, 48) == rawStreams.size(), std::string(name) + ": same stream count");
        usize ec = 56u + 16u * le32(enc, 20);
        for (u32 slot = 0; slot < le32(enc, 52); ++slot) {
            ec += 4u + ((le32(enc, ec) + 3u) & ~3u);
        }
        bool exact = le32(enc, 48) == rawStreams.size();
        for (usize st = 0; st < rawStreams.size() && exact; ++st) {
            const u32 element = rawStreams[st].element;
            const u32 stride = (element + 3u) & ~3u;
            const u32 encoded = le32(enc, ec + 12u);
            exact = le32(enc, ec + 8u) == element * vertexCount;
            std::vector<u8> padded(static_cast<usize>(vertexCount) * stride + 4u);
            exact = exact && meshopt_decodeVertexBuffer(padded.data(), vertexCount, stride, enc.data() + ec + 16u, encoded) == 0;
            for (u32 v = 0; v < vertexCount && exact; ++v) {
                exact = std::memcmp(padded.data() + static_cast<usize>(v) * stride,
                                    raw.data() + rawStreams[st].at + static_cast<usize>(v) * element, element) == 0;
            }
            ec += 16u + ((encoded + 3u) & ~3u);
        }
        check(exact, std::string(name) + ": every vertex stream decodes bit-exactly to the raw payload");
        std::vector<u32> indices(indexCount + 3u);
        const u32 encodedIndices = le32(enc, ec);
        const u8* blob = enc.data() + ec + 4u;
        const bool triangleCodec = (blob[0] & 0xF0u) == 0xE0u;
        const int status = triangleCodec ? meshopt_decodeIndexBuffer(indices.data(), indexCount, 4u, blob, encodedIndices)
                                         : meshopt_decodeIndexSequence(indices.data(), indexCount, 4u, blob, encodedIndices);
        check(status == 0 && std::memcmp(indices.data(), raw.data() + rc, static_cast<usize>(indexCount) * 4u) == 0,
              std::string(name) + ": index list decodes bit-exactly to the raw payload");
        std::printf("[codec] %s: indices %u B -> %u B (%.2f B/tri, %s codec)\n", name, indexCount * 4u, encodedIndices,
                    static_cast<f64>(encodedIndices) / static_cast<f64>(std::max(indexCount / 3u, 1u)),
                    triangleCodec ? "triangle" : "sequence");
    }
#endif
    const f64 reduction = 100.0 * (1.0 - static_cast<f64>(enc.size()) / static_cast<f64>(raw.size()));
    std::printf("[codec] %s: raw %zu B, meshopt %zu B, reduction %.1f %%\n", name, raw.size(), enc.size(), reduction);
    check(enc.size() < raw.size(), std::string(name) + ": codec is smaller than raw");
}

void suite_codec() {
    check(mesh_optimizer_available(), "meshoptimizer linked into the cook");
    CookedMesh rock = make_rock(96, 64, false);
    codec_case("rock f32", rock, MeshEncoding{});
    MeshEncoding quant;
    quant.quantize_positions = true; // Unorm16x3: 6-byte elements, padded to 8 for the codec
    quant.quantize_normals = true;
    codec_case("rock quantised", rock, quant);
    CookedMesh full = make_rock(64, 48, true); // tangent, uv1, colour, joints u16, weights
    codec_case("rock all streams", full, MeshEncoding{});
    MeshEncoding w8 = quant;
    w8.weights_unorm8 = true;
    codec_case("rock all streams quantised unorm8", full, w8);
    for (u16& j : full.joints) {
        j = static_cast<u16>(j % 200u); // u8 joints
    }
    codec_case("rock all streams u8 joints", full, w8);
    CookedMesh terrain = make_terrain(129);
    check(build_mesh_lods(terrain), "terrain LODs for codec");
    codec_case("terrain + LODs", terrain, MeshEncoding{});

    // Corrupt codec payloads are refused (and never read out of bounds).
    MeshEncoding codec;
    codec.meshopt_codec = true;
    const std::vector<u8> enc = serialize_cooked_mesh(rock, codec);
    const usize firstPayload = 56u + 16u * rock.submeshes.size() + 16u; // first stream entry is 16 bytes
    CookedMesh out;
    std::vector<u8> bad = enc;
    bad[firstPayload] = 0x00u; // vertex codec header byte (0xA0 | version)
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "corrupt vertex codec header refused");
    bad = enc;
    set32(bad, 56u + 16u * rock.submeshes.size() + 12u, 3u); // encoded size too small
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "short vertex codec payload refused");
    // Truncating the index codec payload: rebuild the file with a shorter encoded index blob.
    const usize table = section_table_offset(enc); // 0: no sections, compute index blob offset manually
    check(table == 0u, "rock codec file has no sections");
    usize c = 56u + 16u * rock.submeshes.size();
    for (u32 s = 0; s < le32(enc, 48); ++s) {
        c += 16u + ((le32(enc, c + 12u) + 3u) & ~3u);
    }
    const u32 indexBlob = le32(enc, c);
    bad = std::vector<u8>(enc.begin(), enc.begin() + static_cast<std::ptrdiff_t>(c));
    std::vector<u8> tail(enc.begin() + static_cast<std::ptrdiff_t>(c + 4u), enc.begin() + static_cast<std::ptrdiff_t>(c + 4u + indexBlob / 2u));
    const usize at = bad.size();
    bad.resize(at + 4u);
    set32(bad, at, static_cast<u32>(tail.size()));
    bad.insert(bad.end(), tail.begin(), tail.end());
    while (bad.size() % 4u != 0u) {
        bad.push_back(0u);
    }
    bad.resize(bad.size() + 8u);
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "truncated index codec payload refused");
}

void suite_compat() {
    std::string error;
    // W0.1 content without W0.2 sections: no W0.2 flag bits, loads.
    CookedMesh full = make_rock(16, 12, true);
    const std::vector<u8> w01 = serialize_cooked_mesh(full);
    check(le32(w01, 4) == 2u && (le32(w01, 8) & ~1u) == 0u, "W0.1 v2 file carries no W0.2 flag bits");
    CookedMesh out;
    check(deserialize_cooked_mesh(w01.data(), w01.size(), out, &error), "W0.1 v2 file loads: " + error);
    CookedMesh plain = make_terrain(9);
    check(serialize_cooked_mesh(plain)[4] == 1u, "plain mesh still FMSH v1");
    std::vector<u8> bad = w01;
    set32(bad, 8u, 1u << 2);
    check(!deserialize_cooked_mesh(reseal(bad).data(), bad.size(), out), "reserved flag bit 2 refused");

    // Unknown sections are skipped.
    CookedMesh lod = make_terrain(33);
    check(build_mesh_lods(lod), "terrain 33 LODs");
    const std::vector<u8> good = serialize_cooked_mesh(lod);
    const usize table = section_table_offset(good);
    check(table != 0u, "section table present");
    std::vector<u8> extended(good.begin(), good.end() - 8);
    set32(extended, table, le32(good, table) + 1u);
    const u32 extra[] = {fourcc("XTRA"), 2u, 3u};
    for (u32 v : extra) {
        extended.resize(extended.size() + 4u);
        set32(extended, extended.size() - 4u, v);
    }
    extended.insert(extended.end(), {1u, 2u, 3u, 4u, 5u, 6u, 0u, 0u});
    extended.resize(extended.size() + 8u);
    extended = reseal(extended);
    CookedMesh withExtra;
    check(deserialize_cooked_mesh(extended.data(), extended.size(), withExtra, &error), "unknown section skipped: " + error);
    check(meshes_equal(withExtra, lod), "unknown section does not change the mesh");
    // Duplicate known section refused: rename the XTRA section to LODT.
    std::vector<u8> dup = extended;
    set32(dup, extended.size() - 8u - 8u - 12u, fourcc("LODT"));
    check(!deserialize_cooked_mesh(reseal(dup).data(), dup.size(), out), "duplicate / mis-sized known section refused");

#if defined(FUSE_HAS_ASSIMP)
    // cook_mesh_file end to end: OBJ -> LODs + meshlets + DAG + codec.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_asset_fmsh_meshopt";
    std::filesystem::create_directories(dir);
    const std::string obj = (dir / "terrain.obj").string();
    {
        const CookedMesh t = make_terrain(49);
        std::ofstream f(obj);
        for (u32 v = 0; v < t.vertex_count(); ++v) {
            f << "v " << t.positions[v * 3u] << ' ' << t.positions[v * 3u + 1u] << ' ' << t.positions[v * 3u + 2u] << '\n';
        }
        for (usize i = 0; i < t.indices.size(); i += 3u) {
            f << "f " << t.indices[i] + 1u << ' ' << t.indices[i + 1u] + 1u << ' ' << t.indices[i + 2u] + 1u << '\n';
        }
    }
    MeshCookOptions options;
    options.optimize.lods = true;
    options.optimize.cluster_dag = true;
    options.encoding.meshopt_codec = true;
    const std::string cooked = (dir / "terrain.fusemesh").string();
    const CookStubWriteResult result = cook_mesh_file(obj, cooked, options);
    check(result.ok, "cook_mesh_file with W0.2 options: " + result.note);
    CookedMesh loaded;
    check(load_cooked_mesh(cooked, loaded, &error), "cooked W0.2 mesh loads: " + error);
    check(loaded.lods.size() == 3u && !loaded.meshlets.empty() && !loaded.cluster_dag.empty(),
          "cooked mesh carries LODs, meshlets and the DAG");
    const CookStubWriteResult plainCook = cook_mesh_file(obj, (dir / "plain.fusemesh").string());
    check(plainCook.ok, "default cook still works");
#endif
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "lod") {
        suite_lod();
    }
    if (all || suite == "codec") {
        suite_codec();
    }
    if (all || suite == "compat") {
        suite_compat();
    }
#if defined(FUSE_COOK_TEST_HAS_GEOMETRY)
    if (all || suite == "meshlets") {
        suite_meshlets();
    }
    if (all || suite == "dag") {
        suite_dag();
    }
#else
    if (suite == "meshlets" || suite == "dag") {
        std::fprintf(stderr, "FAIL: renderer geometry libraries not in this build\n");
        return 1;
    }
#endif
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_asset_mesh_meshopt %s: OK\n", suite.c_str());
    return 0;
}
