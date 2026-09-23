// B7.5 Terrain gate tests (docs/plans/FUSE_MASTER_PLAN.md, B7.5 + B7.10 "Terrain" rows).
//
// Every check compares against an independent reference: analytic surfaces, the documented noise
// parameters' Lipschitz bounds, direct texel reads, or a from-scratch mesh rebuild.

#include <fuse/core/sanitizer.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/terrain/chunk_grid.hpp>
#include <fuse/terrain/chunk_mesh.hpp>
#include <fuse/terrain/heightfield.hpp>
#include <fuse/terrain/queries.hpp>
#include <fuse/terrain/terrain.hpp>
#include <fuse/terrain/terrain_noise.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using namespace fuse::terrain;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectLe(f64 actual, f64 limit, const char* message) {
    if (!(actual <= limit)) {
        std::fprintf(stderr, "FAIL: %s (%.6g > limit %.6g)\n", message, actual, limit);
        ++g_failures;
    }
}

template <typename Body>
void withScheduler(u32 workers, Body&& body) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
    body();
    scheduler.shutdown();
}

f64 nowMs() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<f64, std::milli>(clock::now().time_since_epoch()).count();
}

f32 chunkDistance(const ChunkGrid& grid, u32 index, vec3 camera) {
    const f32 size = grid.base_chunk_stride();
    const TerrainChunk& chunk = grid.chunk(index);
    const f32 cx = (static_cast<f32>(chunk.chunk_coord.x) + 0.5f) * size;
    const f32 cz = (static_cast<f32>(chunk.chunk_coord.y) + 0.5f) * size;
    return std::sqrt((cx - camera.x) * (cx - camera.x) + (cz - camera.z) * (cz - camera.z));
}

// ---------------------------------------------------------------------------------------------
// Gate: "Heightfield generates without artifacts at 4096x4096 resolution"
// ---------------------------------------------------------------------------------------------
void testGenerate4096WithoutArtifacts() {
    TerrainDesc desc{};
    desc.resolution = 4096;
    desc.world_size = 4096.f;
    desc.max_height = 512.f;
    desc.chunk_resolution = 256;
    desc.async_loading = false;

    Terrain terrain{};
    terrain.init(desc);
    const f64 t0 = nowMs();
    terrain.generate(1234);
    const f64 genMs = nowMs() - t0;
    const std::vector<f32>& h = terrain.heightfield().heights();
    expectTrue(h.size() == 4096u * 4096u, "4096^2 heightfield allocated");

    // Analytic bounds from the documented noise construction: octave i has lattice cell c_i texels,
    // amplitude p^i, lattice values in [-1, 1] and quintic fade (max |f'| = 15/8, max |f''| = 10/sqrt(3)).
    const TerrainNoiseParams params = make_terrain_noise_params(desc);
    f64 ampSum = 0.0;
    f64 slopeSum = 0.0;
    f64 curvSum = 0.0;
    f64 amp = 1.0;
    for (u32 i = 0; i < params.octaves; ++i) {
        const f64 cell = static_cast<f64>(std::max(params.base_cell_texels >> i, 1u));
        ampSum += amp;
        slopeSum += amp * 2.0 * (15.0 / 8.0) / cell;
        curvSum += amp * 2.0 * (10.0 / std::sqrt(3.0)) / (cell * cell);
        amp *= params.persistence;
    }
    const f64 scale = 0.5 * desc.max_height / ampSum;
    const f64 slopeBound = scale * slopeSum * 1.001 + 1e-3;
    const f64 curvBound = scale * curvSum * 1.001 + 2e-3;

    f64 maxStep = 0.0;
    f64 maxCurv = 0.0;
    f64 mean = 0.0;
    f64 meanSq = 0.0;
    bool finiteInRange = true;
    const u32 n = desc.resolution;
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            const f32 v = h[static_cast<size_t>(z) * n + x];
            if (!std::isfinite(v) || v < 0.f || v > desc.max_height) {
                finiteInRange = false;
            }
            mean += v;
            meanSq += static_cast<f64>(v) * v;
            if (x + 1 < n) {
                maxStep = std::max(maxStep, static_cast<f64>(std::fabs(h[z * n + x + 1] - v)));
            }
            if (z + 1 < n) {
                maxStep = std::max(maxStep, static_cast<f64>(std::fabs(h[(z + 1) * n + x] - v)));
            }
            if (x > 0 && x + 1 < n) {
                maxCurv = std::max(maxCurv,
                                   static_cast<f64>(std::fabs(h[z * n + x + 1] - 2.f * v + h[z * n + x - 1])));
            }
            if (z > 0 && z + 1 < n) {
                maxCurv = std::max(maxCurv, static_cast<f64>(std::fabs(h[(z + 1) * n + x] - 2.f * v +
                                                                        h[(z - 1) * n + x])));
            }
        }
    }
    const f64 count = static_cast<f64>(n) * n;
    mean /= count;
    const f64 stddev = std::sqrt(std::max(meanSq / count - mean * mean, 0.0));

    expectTrue(finiteInRange, "4096^2 heights finite and within [0, max_height]");
    expectLe(maxStep, slopeBound, "4096^2 max texel-to-texel step within analytic Lipschitz bound (no spikes)");
    expectLe(maxCurv, curvBound, "4096^2 max second difference within analytic C2 bound (no lattice seams)");
    expectTrue(stddev > 0.05 * desc.max_height, "4096^2 terrain is not degenerate (stddev > 5% max_height)");

    // Independent scalar reference for random texels.
    std::mt19937 rng(7);
    std::uniform_int_distribution<u32> texel(0, n - 1);
    f64 maxRefErr = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const u32 x = texel(rng);
        const u32 z = texel(rng);
        const f32 ref = terrain_noise_height(1234, params, desc.max_height, x, z);
        maxRefErr = std::max(maxRefErr, static_cast<f64>(std::fabs(ref - h[static_cast<size_t>(z) * n + x])));
    }
    expectLe(maxRefErr, 1e-3, "bulk generator matches per-texel reference evaluation");

    // Determinism: same seed -> byte-identical; different seed -> different field.
    std::vector<f32> again(h.size());
    generate_terrain_heights(1234, params, n, desc.max_height, again.data());
    expectTrue(std::memcmp(again.data(), h.data(), h.size() * sizeof(f32)) == 0,
               "4096^2 generation is byte-identical for the same seed");
    generate_terrain_heights(4321, params, n, desc.max_height, again.data());
    expectTrue(std::memcmp(again.data(), h.data(), h.size() * sizeof(f32)) != 0, "different seed differs");

    std::printf("  generate 4096^2: %.1f ms, octaves=%u base_cell=%u, max step %.4f m (bound %.4f), "
                "max 2nd diff %.6f m (bound %.6f), stddev %.1f m\n",
                genMs, params.octaves, params.base_cell_texels, maxStep, slopeBound, maxCurv, curvBound, stddev);
    terrain.destroy();
}

// ---------------------------------------------------------------------------------------------
// Gate: "get_height returns correct value — matches heightmap texel read within 0.01f"
// ---------------------------------------------------------------------------------------------
void testGetHeightMatchesTexel4096() {
    TerrainDesc desc{};
    desc.resolution = 4096;
    desc.world_size = 4096.f;
    desc.max_height = 512.f;
    desc.chunk_resolution = 256;
    desc.async_loading = false;

    Terrain terrain{};
    terrain.init(desc);
    terrain.generate(99);
    const Heightfield& field = terrain.heightfield();
    const f32 mpt = field.meters_per_texel();

    std::mt19937 rng(11);
    std::uniform_int_distribution<u32> texel(0, desc.resolution - 1);
    f64 maxErr = 0.0;
    f64 maxMidErr = 0.0;
    auto check = [&](u32 x, u32 z) {
        const f32 got = terrain.get_height(static_cast<f32>(x) * mpt, static_cast<f32>(z) * mpt);
        maxErr = std::max(maxErr, static_cast<f64>(std::fabs(got - field.get_height_texel(x, z))));
        if (x + 1 < desc.resolution && z + 1 < desc.resolution) {
            // Cell centre: bilinear reference is the mean of the four corner texels.
            const f32 ref = 0.25f * (field.get_height_texel(x, z) + field.get_height_texel(x + 1, z) +
                                     field.get_height_texel(x, z + 1) + field.get_height_texel(x + 1, z + 1));
            const f32 mid = terrain.get_height((static_cast<f32>(x) + 0.5f) * mpt, (static_cast<f32>(z) + 0.5f) * mpt);
            maxMidErr = std::max(maxMidErr, static_cast<f64>(std::fabs(mid - ref)));
        }
    };
    for (int i = 0; i < 50000; ++i) {
        check(texel(rng), texel(rng));
    }
    const u32 last = desc.resolution - 1;
    for (u32 i = 0; i < desc.resolution; i += 127) {
        check(i, 0);
        check(0, i);
        check(i, last);
        check(last, i);
    }
    check(last, last);
    expectLe(maxErr, 0.01, "get_height at texel centres matches texel read within 0.01 (4096^2)");
    expectLe(maxMidErr, 0.01, "get_height at cell centres matches bilinear reference within 0.01");
    std::printf("  get_height vs texel: max err %.6f m, cell-centre max err %.6f m\n", maxErr, maxMidErr);
    terrain.destroy();
}

// ---------------------------------------------------------------------------------------------
// Heightfield sampling / normals / ray cast against an analytic surface.
// ---------------------------------------------------------------------------------------------
void testSamplingNormalsRaycastVsAnalytic() {
    TerrainDesc desc{};
    desc.resolution = 257;
    desc.world_size = 256.f;
    desc.max_height = 64.f;
    Heightfield field{};
    field.init(desc);

    const f32 A = 8.f;
    const f32 k = 2.f * 3.14159265f / 64.f;
    auto analytic = [&](f32 x, f32 z) { return 20.f + A * std::sin(k * x) * std::cos(k * z); };
    const f32 mpt = field.meters_per_texel();
    for (u32 z = 0; z < desc.resolution; ++z) {
        for (u32 x = 0; x < desc.resolution; ++x) {
            field.set_height(x, z, analytic(static_cast<f32>(x) * mpt, static_cast<f32>(z) * mpt));
        }
    }

    std::mt19937 rng(3);
    std::uniform_real_distribution<f32> pos(8.f, 248.f);
    // Bilinear interpolation error <= h^2/8 * (|f_xx| + |f_zz|) + h^2/4 |f_xz|  with |f''| <= A k^2.
    const f64 interpBound = mpt * mpt * (0.25 + 0.25) * A * k * k * 1.05;
    f64 maxHeightErr = 0.0;
    f64 maxAngleDeg = 0.0;
    for (int i = 0; i < 5000; ++i) {
        const f32 x = pos(rng);
        const f32 z = pos(rng);
        maxHeightErr = std::max(maxHeightErr, static_cast<f64>(std::fabs(field.sample_height(x, z) - analytic(x, z))));
        const f32 dhdx = A * k * std::cos(k * x) * std::cos(k * z);
        const f32 dhdz = -A * k * std::sin(k * x) * std::sin(k * z);
        const vec3 ref = vec3{-dhdx, 1.f, -dhdz}.normalized();
        const vec3 got = field.sample_normal(x, z);
        const f64 cosAngle = std::clamp(static_cast<f64>(ref.dot(got)), -1.0, 1.0);
        maxAngleDeg = std::max(maxAngleDeg, std::acos(cosAngle) * 180.0 / 3.141592653589793);
    }
    expectLe(maxHeightErr, interpBound, "sample_height within bilinear interpolation bound of analytic surface");
    expectLe(maxAngleDeg, 1.0, "sample_normal within 1 degree of analytic normal");

    // Ray cast on a tilted plane h = 0.25 x + 10 vs exact intersection.
    for (u32 z = 0; z < desc.resolution; ++z) {
        for (u32 x = 0; x < desc.resolution; ++x) {
            field.set_height(x, z, 0.25f * static_cast<f32>(x) * mpt + 10.f);
        }
    }
    f64 maxHitErr = 0.0;
    bool allHit = true;
    std::uniform_real_distribution<f32> rayPos(8.f, 180.f); // hits stay inside the 256 m field
    for (int i = 0; i < 500; ++i) {
        const vec3 origin{rayPos(rng), 120.f, rayPos(rng)};
        const vec3 dir = vec3{0.3f, -1.f, 0.2f}.normalized();
        const HeightfieldRayHit hit = raycast_heightfield(field, origin, dir, 400.f);
        // origin.y + t*dy = 0.25*(origin.x + t*dx) + 10
        const f32 t = (0.25f * origin.x + 10.f - origin.y) / (dir.y - 0.25f * dir.x);
        const vec3 ref = origin + dir * t;
        if (!hit.hit) {
            allHit = false;
            continue;
        }
        maxHitErr = std::max(maxHitErr, static_cast<f64>((hit.position - ref).length()));
    }
    expectTrue(allHit, "every ray hits the tilted plane");
    expectLe(maxHitErr, 0.02, "ray hit within 2 cm of analytic plane intersection");
    std::printf("  analytic: height err %.5f (bound %.5f), normal err %.4f deg, ray hit err %.5f m\n", maxHeightErr,
                interpBound, maxAngleDeg, maxHitErr);
}

// ---------------------------------------------------------------------------------------------
// Chunk footprints tile the world exactly at every LOD.
// ---------------------------------------------------------------------------------------------
void testChunkFootprintsTileWorld() {
    TerrainDesc desc{};
    desc.resolution = 513;
    desc.world_size = 512.f;
    desc.chunk_resolution = 32;
    desc.lod_levels = 4;
    desc.async_loading = false;
    ChunkGrid grid{};
    grid.init(desc);
    const u32 per_axis = grid.chunks_per_axis();
    const f32 size = desc.world_size / static_cast<f32>(per_axis);
    bool tiles = true;
    for (u32 lod = 0; lod < desc.lod_levels; ++lod) {
        for (u32 i = 0; i < grid.chunk_count(); ++i) {
            const ivec2 c = grid.chunk(i).chunk_coord;
            const AABB b = grid.chunk_world_bounds(c, lod);
            tiles = tiles && std::fabs(b.min.x - static_cast<f32>(c.x) * size) < 1e-3f &&
                    std::fabs(b.min.z - static_cast<f32>(c.y) * size) < 1e-3f &&
                    std::fabs(b.max.x - b.min.x - size) < 1e-3f && std::fabs(b.max.z - b.min.z - size) < 1e-3f;
        }
    }
    expectTrue(tiles, "chunk footprints tile the world with LOD-independent extents");
    grid.update_lod({100.f, 0.f, 100.f}, 1.f / 60.f);
    bool lodBoundsOk = true;
    for (u32 i = 0; i < grid.chunk_count(); ++i) {
        const TerrainChunk& chunk = grid.chunk(i);
        lodBoundsOk = lodBoundsOk && std::fabs(chunk.world_bounds.max.x - chunk.world_bounds.min.x - size) < 1e-3f;
    }
    expectTrue(lodBoundsOk, "chunk world_bounds keep footprint after LOD update");
    grid.destroy();
}

TerrainDesc makeStreamingDesc() {
    TerrainDesc desc{};
    desc.resolution = 1025;
    desc.world_size = 1024.f;
    desc.max_height = 96.f;
    desc.chunk_resolution = 32;
    desc.lod_levels = 4;
    desc.load_radius = 200.f;
    desc.max_async_in_flight = 8;
    return desc;
}

/// Checks shared edges between every pair of resident neighbours; returns mismatch count.
u32 checkSeams(const Terrain& terrain, f64& maxGap, u32& mixedPairs, u32& pairs) {
    const ChunkGrid& grid = terrain.chunk_grid();
    u32 bad = 0;
    for (u32 i = 0; i < grid.chunk_count(); ++i) {
        const ChunkMesh* a = terrain.chunk_mesh(i);
        if (a == nullptr) {
            continue;
        }
        const ivec2 c = grid.chunk(i).chunk_coord;
        const struct {
            ivec2 offset;
            ChunkEdge mine;
            ChunkEdge theirs;
        } links[2] = {{{1, 0}, ChunkEdge::PosX, ChunkEdge::NegX}, {{0, 1}, ChunkEdge::PosZ, ChunkEdge::NegZ}};
        for (const auto& link : links) {
            const u32 j = grid.chunk_index_at({c.x + link.offset.x, c.y + link.offset.y});
            const ChunkMesh* b = j == ~0u ? nullptr : terrain.chunk_mesh(j);
            if (b == nullptr) {
                continue;
            }
            ++pairs;
            if (a->lod != b->lod) {
                ++mixedPairs;
            }
            for (u32 s = 0; s <= 256; ++s) {
                const f32 t = static_cast<f32>(s) / 256.f;
                const f64 gap = std::fabs(chunk_mesh_edge_height(*a, link.mine, t) -
                                          chunk_mesh_edge_height(*b, link.theirs, t));
                maxGap = std::max(maxGap, gap);
                if (gap > 1e-3) {
                    ++bad;
                }
            }
            // Vertices both meshes share must be bit-identical.
            const ChunkMesh& coarse = a->cells_per_edge <= b->cells_per_edge ? *a : *b;
            const ChunkMesh& fine = a->cells_per_edge <= b->cells_per_edge ? *b : *a;
            const bool coarseIsA = &coarse == a;
            const ChunkEdge coarseEdge = coarseIsA ? link.mine : link.theirs;
            const ChunkEdge fineEdge = coarseIsA ? link.theirs : link.mine;
            const u32 ratio = fine.cells_per_edge / coarse.cells_per_edge;
            for (u32 k = 0; k <= coarse.cells_per_edge; ++k) {
                auto edgeVertex = [](const ChunkMesh& m, ChunkEdge e, u32 idx) {
                    const u32 last = m.cells_per_edge;
                    switch (e) {
                    case ChunkEdge::NegX: return m.positions[m.vertex_index(0, idx)];
                    case ChunkEdge::PosX: return m.positions[m.vertex_index(last, idx)];
                    case ChunkEdge::NegZ: return m.positions[m.vertex_index(idx, 0)];
                    default: return m.positions[m.vertex_index(idx, last)];
                    }
                };
                const vec3 p = edgeVertex(coarse, coarseEdge, k);
                const vec3 q = edgeVertex(fine, fineEdge, k * ratio);
                if (p.x != q.x || p.y != q.y || p.z != q.z) {
                    ++bad;
                }
            }
        }
    }
    return bad;
}

// ---------------------------------------------------------------------------------------------
// Gate: "LOD system loads and unloads chunks correctly as camera moves — no missing geometry"
// (synchronous path) + crack-free seams between LODs + mesh correctness.
// ---------------------------------------------------------------------------------------------
void testLodStreamingNoMissingGeometrySync() {
    TerrainDesc desc = makeStreamingDesc();
    desc.async_loading = false;
    Terrain terrain{};
    terrain.init(desc);
    terrain.generate(77);
    const ChunkGrid& grid = terrain.chunk_grid();
    const f32 loadRadius = grid.effective_load_radius();
    const f32 unloadRadius = loadRadius * 1.25f;

    u32 missing = 0;
    u32 stale = 0;
    u32 seamBad = 0;
    u32 meshMismatch = 0;
    u32 lodMismatch = 0;
    f64 maxGap = 0.0;
    u32 mixedPairs = 0;
    u32 pairs = 0;
    u32 loadedEver = 0;
    u32 unloadedEver = 0;
    std::vector<bool> wasResident(grid.chunk_count(), false);
    f64 maxFrameMs = 0.0;

    // Camera sweeps diagonally across the world and back along a different line.
    const u32 frames = 700;
    for (u32 frame = 0; frame < frames; ++frame) {
        const f32 t = static_cast<f32>(frame) / static_cast<f32>(frames - 1);
        const vec3 camera = t < 0.5f ? vec3{100.f + t * 2.f * 800.f, 0.f, 150.f + t * 2.f * 700.f}
                                     : vec3{900.f - (t - 0.5f) * 2.f * 700.f, 0.f, 850.f};
        const f64 t0 = nowMs();
        terrain.update_lod(camera, 1.f / 60.f);
        maxFrameMs = std::max(maxFrameMs, nowMs() - t0);

        for (u32 i = 0; i < grid.chunk_count(); ++i) {
            const TerrainChunk& chunk = grid.chunk(i);
            const f32 d = chunkDistance(grid, i, camera);
            const bool resident = is_resident_state(chunk.residency);
            if (d < loadRadius && !resident) {
                ++missing;
            }
            if (d > unloadRadius && resident) {
                ++stale;
            }
            if (resident != wasResident[i]) {
                (resident ? loadedEver : unloadedEver) += 1;
                wasResident[i] = resident;
            }
            const ChunkMesh* mesh = terrain.chunk_mesh(i);
            if (resident != (mesh != nullptr)) {
                ++meshMismatch;
            }
            if (mesh != nullptr && mesh->lod != chunk.lod) {
                ++lodMismatch;
            }
        }
        if (frame % 50 == 0) {
            seamBad += checkSeams(terrain, maxGap, mixedPairs, pairs);
        }
    }

    expectTrue(missing == 0, "every chunk inside load radius is resident every frame (no missing geometry)");
    expectTrue(stale == 0, "no chunk beyond unload radius stays resident");
    expectTrue(loadedEver > 100 && unloadedEver > 50, "camera path streams chunks in and out");
    expectTrue(meshMismatch == 0, "every resident chunk has a mesh; non-resident chunks have none");
    expectTrue(lodMismatch == 0, "chunk meshes are rebuilt at the chunk's current LOD");
    expectTrue(mixedPairs > 0, "seam check exercised neighbours at different LODs");
    expectTrue(seamBad == 0, "shared edges between resident chunks are crack-free across LODs");
    std::printf("  sync LOD stream: loads %u unloads %u, seam pairs %u (mixed LOD %u), max seam gap %.2e m, "
                "max update %.3f ms\n",
                loadedEver, unloadedEver, pairs, mixedPairs, maxGap, maxFrameMs);

    // Control: without stitching the same mixed-LOD edge would crack, so the seam check is sensitive.
    f64 unstitchedGap = 0.0;
    for (u32 i = 0; i < grid.chunk_count() && unstitchedGap == 0.0; ++i) {
        const ChunkMesh* a = terrain.chunk_mesh(i);
        const u32 j = grid.chunk_index_at({grid.chunk(i).chunk_coord.x + 1, grid.chunk(i).chunk_coord.y});
        const ChunkMesh* b = j == ~0u ? nullptr : terrain.chunk_mesh(j);
        if (a == nullptr || b == nullptr || a->cells_per_edge == b->cells_per_edge) {
            continue;
        }
        ChunkMeshDesc raw{};
        raw.chunk_coord = a->chunk_coord;
        raw.chunk_size = grid.base_chunk_stride();
        raw.chunk_resolution = desc.chunk_resolution;
        raw.lod = a->lod;
        for (u32& lod : raw.neighbor_lod) {
            lod = a->lod;
        }
        ChunkMesh rawA{};
        build_chunk_mesh(terrain.heightfield(), raw, rawA);
        raw.chunk_coord = b->chunk_coord;
        raw.lod = b->lod;
        for (u32& lod : raw.neighbor_lod) {
            lod = b->lod;
        }
        ChunkMesh rawB{};
        build_chunk_mesh(terrain.heightfield(), raw, rawB);
        for (u32 s = 0; s <= 256; ++s) {
            const f32 t = static_cast<f32>(s) / 256.f;
            unstitchedGap = std::max(unstitchedGap,
                                     static_cast<f64>(std::fabs(chunk_mesh_edge_height(rawA, ChunkEdge::PosX, t) -
                                                                chunk_mesh_edge_height(rawB, ChunkEdge::NegX, t))));
        }
    }
    expectTrue(unstitchedGap > 1e-2, "control: unstitched mixed-LOD edge shows a crack the check would catch");

    // Each resident mesh equals an independent rebuild; non-stitched vertices equal heightfield samples.
    for (u32 i = 0; i < grid.chunk_count(); ++i) {
        const ChunkMesh* mesh = terrain.chunk_mesh(i);
        if (mesh == nullptr) {
            continue;
        }
        const u32 cells = mesh->cells_per_edge;
        for (u32 iz = 1; iz < cells; ++iz) {
            for (u32 ix = 1; ix < cells; ++ix) {
                const vec3 p = mesh->positions[mesh->vertex_index(ix, iz)];
                if (std::fabs(p.y - terrain.get_height(p.x, p.z)) > 1e-4f) {
                    ++meshMismatch;
                }
            }
        }
    }
    expectTrue(meshMismatch == 0, "interior mesh vertices equal heightfield samples");
    terrain.destroy();
}

// ---------------------------------------------------------------------------------------------
// Gate (async path): chunk loads run on JobScheduler workers and apply on the game thread; the
// area around the camera never has missing geometry while moving; everything settles cleanly.
// ---------------------------------------------------------------------------------------------
void testLodStreamingAsync() {
    withScheduler(4, [] {
        TerrainDesc desc = makeStreamingDesc();
        desc.async_loading = true;
        Terrain terrain{};
        terrain.init(desc);
        terrain.generate(5);
        const ChunkGrid& grid = terrain.chunk_grid();
        const f32 loadRadius = grid.effective_load_radius();
        const f32 innerRadius = loadRadius - 96.f; // lead distance the queue must keep ahead of the camera

        vec3 camera{150.f, 0.f, 150.f};
        for (int i = 0; i < 2000 && (grid.in_flight_request_count() > 0 || grid.queued_load_count() > 0 ||
                                     grid.resident_chunk_count() == 0);
             ++i) {
            terrain.update_lod(camera, 1.f / 60.f);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }

        u32 missingInner = 0;
        u32 maxInFlight = 0;
        const f32 speed = 1.5f; // metres per frame
        for (u32 frame = 0; frame < 450; ++frame) {
            camera.x += speed;
            camera.z += speed * 0.8f;
            terrain.update_lod(camera, 1.f / 60.f);
            maxInFlight = std::max(maxInFlight, grid.in_flight_request_count());
            for (u32 i = 0; i < grid.chunk_count(); ++i) {
                if (chunkDistance(grid, i, camera) < innerRadius &&
                    (!is_resident_state(grid.chunk(i).residency) || terrain.chunk_mesh(i) == nullptr)) {
                    ++missingInner;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        for (int i = 0; i < 5000 && (grid.in_flight_request_count() > 0 || grid.queued_load_count() > 0 ||
                                     grid.queued_unload_count() > 0);
             ++i) {
            terrain.update_lod(camera, 1.f / 60.f);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        terrain.update_lod(camera, 1.f / 60.f);

        u32 missing = 0;
        u32 stale = 0;
        u32 transitional = 0;
        for (u32 i = 0; i < grid.chunk_count(); ++i) {
            const f32 d = chunkDistance(grid, i, camera);
            const ChunkResidencyState state = grid.chunk(i).residency;
            missing += (d < loadRadius && !is_resident_state(state)) ? 1u : 0u;
            stale += (d > loadRadius * 1.25f && is_resident_state(state)) ? 1u : 0u;
            transitional += is_transitional_state(state) ? 1u : 0u;
        }
        expectTrue(missingInner == 0, "async: chunks near the camera stay resident with meshes while moving");
        expectTrue(maxInFlight <= desc.max_async_in_flight, "async: in-flight loads respect max_async_in_flight");
        expectTrue(missing == 0 && stale == 0, "async: settled residency matches load/unload radii");
        expectTrue(transitional == 0, "async: no chunk left mid-transition after settling");
        expectTrue(grid.in_flight_request_count() == 0, "async: no in-flight jobs after settling");
        expectTrue(grid.residency_set().size() == grid.resident_chunk_count(), "async: residency set mirrors chunks");
        f64 maxGap = 0.0;
        u32 mixed = 0;
        u32 pairs = 0;
        expectTrue(checkSeams(terrain, maxGap, mixed, pairs) == 0, "async: seams crack-free after streaming");
        std::printf("  async LOD stream: max in-flight %u, inner-radius misses %u, resident %u\n", maxInFlight,
                    missingInner, grid.resident_chunk_count());
        terrain.destroy();
    });
}

// ---------------------------------------------------------------------------------------------
// Gate: "Terrain deform correctly updates affected chunk mesh within 1 frame"
// ---------------------------------------------------------------------------------------------
void testDeformUpdatesMeshWithinOneFrame() {
    TerrainDesc desc = makeStreamingDesc();
    desc.async_loading = false;
    Terrain terrain{};
    terrain.init(desc);
    terrain.generate(21);
    const ChunkGrid& grid = terrain.chunk_grid();
    const vec3 camera{512.f, 0.f, 512.f};
    terrain.update_lod(camera, 1.f / 60.f);
    terrain.update_lod(camera, 1.f / 60.f);

    std::vector<ChunkMesh> before(grid.chunk_count());
    for (u32 i = 0; i < grid.chunk_count(); ++i) {
        if (const ChunkMesh* mesh = terrain.chunk_mesh(i)) {
            before[i] = *mesh;
        }
    }
    const u64 buildsBefore = terrain.mesh_build_count();

    // Crater centred on a chunk corner so four chunks share the edit.
    const f32 size = grid.base_chunk_stride();
    const vec3 centre{size * 16.f, 0.f, size * 16.f};
    const f32 heightBefore = terrain.get_height(centre.x, centre.z);
    terrain.deform(centre, 10.f, -6.f);
    terrain.update_lod(camera, 1.f / 60.f); // exactly one frame

    u32 changed = 0;
    u32 wrong = 0;
    u32 untouchedRebuilt = 0;
    u32 expectedAffected = 0;
    for (u32 i = 0; i < grid.chunk_count(); ++i) {
        const ChunkMesh* mesh = terrain.chunk_mesh(i);
        if (mesh == nullptr) {
            continue;
        }
        // Reference: rebuild from the current heightfield with the same LOD/stitching.
        ChunkMeshDesc ref{};
        ref.chunk_coord = grid.chunk(i).chunk_coord;
        ref.chunk_size = size;
        ref.chunk_resolution = desc.chunk_resolution;
        ref.lod = grid.chunk(i).lod;
        grid.neighbor_lods(i, ref.neighbor_lod);
        ChunkMesh expected{};
        build_chunk_mesh(terrain.heightfield(), ref, expected);
        bool same = expected.positions.size() == mesh->positions.size();
        for (size_t v = 0; same && v < expected.positions.size(); ++v) {
            same = expected.positions[v].y == mesh->positions[v].y;
        }
        wrong += same ? 0u : 1u;

        bool differsFromBefore = before[i].positions.size() != mesh->positions.size();
        for (size_t v = 0; !differsFromBefore && v < mesh->positions.size(); ++v) {
            differsFromBefore = before[i].positions[v].y != mesh->positions[v].y;
        }
        changed += differsFromBefore ? 1u : 0u;

        const AABB& b = grid.chunk(i).world_bounds;
        const bool touches = b.max.x >= centre.x - 12.f && b.min.x <= centre.x + 12.f && b.max.z >= centre.z - 12.f &&
                             b.min.z <= centre.z + 12.f;
        expectedAffected += touches ? 1u : 0u;
        if (!touches && differsFromBefore) {
            ++untouchedRebuilt;
        }
    }
    const u64 rebuilds = terrain.mesh_build_count() - buildsBefore;
    const f32 heightAfter = terrain.get_height(centre.x, centre.z);

    expectTrue(std::fabs((heightAfter - heightBefore) - (-6.f)) < 0.05f, "deform lowers centre by amount");
    expectTrue(wrong == 0, "after one update every resident mesh equals a rebuild from current heights");
    expectTrue(changed == 4u && expectedAffected == 4u, "exactly the four chunks sharing the crater changed");
    expectTrue(untouchedRebuilt == 0, "chunks away from the edit keep their meshes");
    expectTrue(rebuilds == 4u, "only affected chunk meshes were rebuilt in that frame");
    expectTrue(terrain.height_dirty_chunk_count() == 0, "no resident chunk left height-dirty after the frame");
    std::printf("  deform: %u chunk meshes changed, %llu rebuilt in 1 frame\n", changed,
                static_cast<unsigned long long>(rebuilds));
    terrain.destroy();
}

// ---------------------------------------------------------------------------------------------
// Gate: "Terrain-SVO cave correctly renders below terrain surface — SVO ray march transitions
// from heightfield" (CPU reference of the transition; the GPU image itself is a manual check).
// ---------------------------------------------------------------------------------------------
void testSvoCaveRayMarchTransition() {
#if defined(FUSE_TERRAIN_HAS_SVO) && FUSE_TERRAIN_HAS_SVO
    TerrainDesc desc{};
    desc.resolution = 257;
    desc.world_size = 256.f;
    desc.max_height = 64.f;
    desc.chunk_resolution = 32;
    desc.has_svo_caves = true;
    desc.svo_depth = 8; // 1 m voxels
    desc.async_loading = false;
    Terrain terrain{};
    terrain.init(desc);
    terrain.heightfield().fill(40.f);

    const vec3 down{0.f, -1.f, 0.f};
    vec3 hit{};
    vec3 normal{};
    f32 distance = 0.f;
    expectTrue(terrain.ray_cast({128.f, 100.f, 128.f}, down, 200.f, hit, normal, distance) &&
                   std::fabs(hit.y - 40.f) < 0.01f,
               "before carving, ray stops on heightfield surface");

    // Sphere cave breaking the surface: opening radius sqrt(10^2 - 4^2) at y = 40.
    const vec3 c{128.f, 36.f, 128.f};
    const f32 r = 10.f;
    terrain.carve_cave(c, r);
    const f32 voxel = terrain.caves().voxel_size();
    const f32 tol = voxel * 1.75f; // voxelised sphere boundary within one voxel diagonal
    expectTrue(terrain.caves().carved_voxel_count() > 3000u, "cave voxels written to the SVO");

    // Vertical rays through the opening reach the cave floor: y = c.y - sqrt(r^2 - d^2).
    u32 floorBad = 0;
    f64 maxFloorErr = 0.0;
    for (f32 d : {0.3f, 3.f, 6.f, 8.f}) {
        const bool ok = terrain.ray_cast({c.x + d, 100.f, c.z + 0.2f}, down, 200.f, hit, normal, distance);
        const f32 expected = c.y - std::sqrt(r * r - d * d - 0.04f);
        const f64 err = std::fabs(hit.y - expected);
        maxFloorErr = std::max(maxFloorErr, err);
        floorBad += (!ok || err > tol || normal.y < 0.5f) ? 1u : 0u;
    }
    expectTrue(floorBad == 0u, "rays through the cave mouth hit the SVO cave floor with upward normals");

    // Away from the cave the heightfield surface is unchanged.
    expectTrue(terrain.ray_cast({40.f, 100.f, 200.f}, down, 200.f, hit, normal, distance) &&
                   std::fabs(hit.y - 40.f) < 0.01f && normal.y > 0.999f,
               "rays away from the cave still hit the heightfield surface");
    // Just outside the opening the surface still caps the rock.
    expectTrue(terrain.ray_cast({c.x + 11.f, 100.f, c.z}, down, 200.f, hit, normal, distance) &&
                   std::fabs(hit.y - 40.f) < 0.01f,
               "ray beside the cave mouth hits the surface");

    // Oblique ray: enters through the mouth, crosses the cave and hits the far wall.
    const vec3 o{c.x - 2.f, 60.f, c.z};
    const vec3 dir = vec3{0.3f, -1.f, 0.f}.normalized();
    const vec3 oc = o - c;
    const f32 b = oc.dot(dir);
    const f32 tFar = -b + std::sqrt(b * b - (oc.dot(oc) - r * r));
    const vec3 expectedHit = o + dir * tFar;
    const bool obliqueOk = terrain.ray_cast(o, dir, 200.f, hit, normal, distance);
    const f64 wallErr = (hit - expectedHit).length();
    const vec3 inward = (c - expectedHit).normalized();
    expectTrue(obliqueOk && wallErr <= tol * 1.5f, "oblique ray transitions heightfield -> SVO and hits far cave wall");
    expectTrue(normal.dot(inward) > 0.95f, "cave wall normal faces into the cave");

    // Occupancy: cave air below the surface is hollow; rock around it and air above are as expected.
    expectTrue(!terrain.is_solid({c.x, c.y, c.z}), "cave centre is hollow");
    expectTrue(terrain.is_solid({c.x + 20.f, 30.f, c.z}), "rock beside the cave is solid");
    expectTrue(!terrain.is_solid({c.x + 20.f, 45.f, c.z}), "air above the surface is not solid");
    expectTrue(terrain.get_height(c.x, c.z) == 40.f, "heightfield itself is untouched by cave carving");
    std::printf("  svo cave: voxel %.2f m, floor err %.3f m, wall err %.3f m\n", voxel, maxFloorErr, wallErr);
    terrain.destroy();
#else
    std::printf("  svo cave: skipped (built without fuse_scene SVO)\n");
#endif
}

// ---------------------------------------------------------------------------------------------
// Determinism: same seed + same camera path -> identical residency and meshes.
// ---------------------------------------------------------------------------------------------
void testDeterministicStreaming() {
    auto run = [](std::vector<f32>& signature) {
        TerrainDesc desc = makeStreamingDesc();
        desc.async_loading = false;
        Terrain terrain{};
        terrain.init(desc);
        terrain.generate(8);
        for (u32 frame = 0; frame < 120; ++frame) {
            terrain.update_lod({200.f + frame * 3.f, 0.f, 300.f + frame * 2.f}, 1.f / 60.f);
        }
        for (u32 i = 0; i < terrain.chunk_grid().chunk_count(); ++i) {
            if (const ChunkMesh* mesh = terrain.chunk_mesh(i)) {
                signature.push_back(static_cast<f32>(i));
                for (const vec3& p : mesh->positions) {
                    signature.push_back(p.y);
                }
            }
        }
        terrain.destroy();
    };
    std::vector<f32> a;
    std::vector<f32> b;
    run(a);
    run(b);
    expectTrue(!a.empty() && a.size() == b.size() &&
                   std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0,
               "terrain streaming + meshes are deterministic for identical inputs");
}

// ---------------------------------------------------------------------------------------------
// Frame budget: LOD/mesh update stays inside a frame slice while streaming (enforced under NDEBUG).
// ---------------------------------------------------------------------------------------------
void testUpdateFrameBudget() {
    TerrainDesc desc = makeStreamingDesc();
    desc.async_loading = false;
    Terrain terrain{};
    terrain.init(desc);
    terrain.generate(9);
    terrain.update_lod({300.f, 0.f, 300.f}, 1.f / 60.f);
    std::vector<f64> samples;
    for (u32 frame = 0; frame < 400; ++frame) {
        const vec3 camera{300.f + frame * 1.f, 0.f, 300.f + frame * 0.5f};
        const f64 t0 = nowMs();
        terrain.update_lod(camera, 1.f / 60.f);
        samples.push_back(nowMs() - t0);
    }
    std::sort(samples.begin(), samples.end());
    const f64 p99 = samples[samples.size() * 99 / 100];
    const f64 worst = samples.back();
    std::printf("  update_lod while streaming (1024 chunks): p99 %.3f ms, worst %.3f ms\n", p99, worst);
#ifdef NDEBUG
    if (fuse::core::timingBudgetsEnforcedNoted()) {
        expectLe(p99, 2.0, "update_lod p99 under 2 ms while streaming (NDEBUG)");
    }
#endif
    terrain.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();
    std::printf("fuse_b7_terrain_gates\n");
    testGenerate4096WithoutArtifacts();
    testGetHeightMatchesTexel4096();
    testSamplingNormalsRaycastVsAnalytic();
    testChunkFootprintsTileWorld();
    testLodStreamingNoMissingGeometrySync();
    testLodStreamingAsync();
    testDeformUpdatesMeshWithinOneFrame();
    testSvoCaveRayMarchTransition();
    testDeterministicStreaming();
    testUpdateFrameBudget();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b7_terrain_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b7_terrain_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
