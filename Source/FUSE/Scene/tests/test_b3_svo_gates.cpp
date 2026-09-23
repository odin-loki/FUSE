// B3.9 SVO gate rows (master plan):
//  - insert/get round-trips 1M voxels at depth 10
//  - ray cast matches brute-force voxel traversal for 10k random rays
//  - carve produces correct surface voxel transitions (re-query the carved region)
//  - SDF query is smooth at leaf boundaries (no discontinuities)
#include <fuse/scene/svo.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <unordered_set>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

using fuse::scene::f32;
using fuse::scene::ivec3;
using fuse::scene::SVO;
using fuse::scene::SVODesc;
using fuse::scene::vec3;

void testMillionVoxelRoundTrip() {
    SVO svo;
    SVODesc desc{};
    desc.rootSize = 1024.f;
    desc.maxDepth = 10;
    svo.init(desc);

    std::mt19937 rng(1'000'000u);
    std::uniform_int_distribution<int> coord(0, 1023);
    std::unordered_set<fuse::u32> used;
    std::vector<ivec3> voxels;
    voxels.reserve(1'000'000);
    while (voxels.size() < 1'000'000u) {
        const ivec3 c{coord(rng), coord(rng), coord(rng)};
        const fuse::u32 key = static_cast<fuse::u32>(c.x) | (static_cast<fuse::u32>(c.y) << 10u) |
                              (static_cast<fuse::u32>(c.z) << 20u);
        if (used.insert(key).second) {
            voxels.push_back(c);
        }
    }
    for (std::size_t i = 0; i < voxels.size(); ++i) {
        svo.set(voxels[i], 1u + static_cast<fuse::u32>(i % 250u));
    }
    expectTrue(svo.voxelCount() == voxels.size(), "voxelCount tracks 1M inserts");

    std::size_t wrong = 0;
    for (std::size_t i = 0; i < voxels.size(); ++i) {
        wrong += svo.get(voxels[i]) == 1u + static_cast<fuse::u32>(i % 250u) ? 0u : 1u;
    }
    std::size_t falsePositives = 0;
    for (int i = 0; i < 100'000; ++i) {
        const ivec3 c{coord(rng), coord(rng), coord(rng)};
        const fuse::u32 key = static_cast<fuse::u32>(c.x) | (static_cast<fuse::u32>(c.y) << 10u) |
                              (static_cast<fuse::u32>(c.z) << 20u);
        if (used.count(key) == 0u && svo.get(c) != 0u) {
            ++falsePositives;
        }
    }
    std::printf("SVO 1M round-trip: %zu wrong, %zu false positives, %zu nodes (%zu bricks), %.1f MB\n", wrong,
                falsePositives, svo.nodeCount(), svo.brickCount(),
                static_cast<double>(svo.memoryBytes()) / (1024.0 * 1024.0));
    expectTrue(wrong == 0u, "every one of 1M voxels reads back its material");
    // Per-voxel leaves needed ~4.06M 48-byte nodes (~195 MB) here; brick leaves keep it compact.
    expectTrue(svo.memoryBytes() < 40u * 1024u * 1024u, "1M scattered voxels fit in < 40 MB");
    expectTrue(falsePositives == 0u, "unset voxels read as empty");

    for (std::size_t i = 0; i < 1000u; ++i) {
        svo.set(voxels[i], 0u);
    }
    expectTrue(svo.voxelCount() == voxels.size() - 1000u && svo.get(voxels[0]) == 0u, "clears are counted");
}

/// Brute force: nearest entry distance over every solid voxel's AABB (slab test).
bool bruteForceRay(const std::vector<ivec3>& solid, f32 size, vec3 origin, vec3 dir, f32 maxDistance, ivec3& hit,
                   f32& hitT) {
    hitT = std::numeric_limits<f32>::max();
    bool found = false;
    for (const ivec3& v : solid) {
        const f32 mn[3] = {v.x * size, v.y * size, v.z * size};
        const f32 o[3] = {origin.x, origin.y, origin.z};
        const f32 d[3] = {dir.x, dir.y, dir.z};
        f32 t0 = 0.f;
        f32 t1 = std::numeric_limits<f32>::max();
        bool miss = false;
        for (int a = 0; a < 3 && !miss; ++a) {
            if (std::abs(d[a]) < 1e-12f) {
                miss = o[a] < mn[a] || o[a] > mn[a] + size;
                continue;
            }
            f32 ta = (mn[a] - o[a]) / d[a];
            f32 tb = (mn[a] + size - o[a]) / d[a];
            if (ta > tb) {
                std::swap(ta, tb);
            }
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            miss = t0 > t1;
        }
        if (!miss && t0 <= maxDistance && t0 < hitT) {
            hitT = t0;
            hit = v;
            found = true;
        }
    }
    return found;
}

void testRayCastMatchesBruteForce() {
    SVO svo;
    SVODesc desc{};
    desc.rootSize = 64.f; // 1-unit voxels
    desc.maxDepth = 6;
    svo.init(desc);

    std::mt19937 rng(10'000u);
    std::uniform_int_distribution<int> coord(0, 63);
    std::vector<ivec3> solid;
    for (int i = 0; i < 4000; ++i) {
        const ivec3 c{coord(rng), coord(rng), coord(rng)};
        if (svo.get(c) == 0u) {
            svo.set(c, 1u);
            solid.push_back(c);
        }
    }

    std::uniform_real_distribution<f32> pos(-20.f, 84.f);
    std::uniform_real_distribution<f32> unit(-1.f, 1.f);
    int mismatches = 0;
    int hits = 0;
    for (int r = 0; r < 10'000; ++r) {
        const vec3 origin{pos(rng), pos(rng), pos(rng)};
        vec3 dir{unit(rng), unit(rng), unit(rng)};
        if (dir.length() < 1e-3f) {
            continue;
        }
        dir = dir.normalized();
        ivec3 expectedVoxel{};
        f32 expectedT = 0.f;
        const bool expected = bruteForceRay(solid, 1.f, origin, dir, 200.f, expectedVoxel, expectedT);

        ivec3 voxel{};
        vec3 normal{};
        f32 t = 0.f;
        const bool actual = svo.rayCast(origin, dir, 200.f, voxel, normal, t);
        hits += expected ? 1 : 0;
        // Compare distance (ties between voxels sharing an entry point are equally correct).
        if (actual != expected || (expected && std::abs(t - expectedT) > 1e-3f)) {
            ++mismatches;
        }
    }
    std::printf("SVO ray cast: 10000 rays, %d hits, %d mismatches vs brute force\n", hits, mismatches);
    expectTrue(hits > 500, "ray set hits voxels");
    expectTrue(mismatches == 0, "DDA ray cast matches brute-force traversal");
}

void testCarveTransitions() {
    SVO svo;
    SVODesc desc{};
    desc.rootSize = 64.f;
    desc.maxDepth = 6; // 1-unit voxels
    svo.init(desc);
    svo.fill({10, 10, 10}, {40, 40, 40}, 3u);
    const std::size_t before = svo.voxelCount();

    const vec3 center{25.f, 25.f, 25.f};
    const f32 radius = 7.5f;
    svo.carve(center, radius);

    int wrongState = 0;
    int wrongSign = 0;
    int removed = 0;
    for (int z = 8; z <= 42; ++z) {
        for (int y = 8; y <= 42; ++y) {
            for (int x = 8; x <= 42; ++x) {
                const bool wasSolid = x >= 10 && x <= 40 && y >= 10 && y <= 40 && z >= 10 && z <= 40;
                const vec3 c{x + 0.5f, y + 0.5f, z + 0.5f};
                const bool insideSphere = (c - center).length() < radius;
                const bool expectSolid = wasSolid && !insideSphere;
                const bool isSolid = svo.get({x, y, z}) != 0u;
                wrongState += isSolid != expectSolid ? 1 : 0;
                removed += wasSolid && !isSolid ? 1 : 0;
                // Stored distance sign must agree with occupancy at the voxel centre.
                const f32 sdf = svo.sdfQuery(c);
                wrongSign += (isSolid ? sdf > 1e-4f : sdf <= 0.f) ? 1 : 0;
            }
        }
    }
    std::printf("SVO carve: removed %d voxels, %d state errors, %d sign errors\n", removed, wrongState, wrongSign);
    expectTrue(wrongState == 0, "carve clears exactly the voxels whose centres lie inside the sphere");
    expectTrue(wrongSign == 0, "SDF sign matches occupancy after carve");
    expectTrue(svo.voxelCount() == before - static_cast<std::size_t>(removed), "voxelCount follows carve");

    // Carved surface: a voxel whose centre sits just outside the sphere (distance ~7.53 vs r 7.5)
    // now stores the sphere distance instead of the flat -0.5 of an untouched solid voxel.
    const f32 surface = svo.sdfQuery({25.5f, 25.5f, 32.5f});
    expectTrue(surface <= 0.f && surface > -0.5f, "boundary voxel stores the refined carve distance");
}

void testDenseFillMemory() {
    SVO svo;
    SVODesc desc{};
    desc.rootSize = 1024.f;
    desc.maxDepth = 10;
    svo.init(desc);
    // 256^3 = 16.8M voxels: whole bricks collapse to uniform records, the ragged edge stays dense.
    svo.fill({3, 3, 3}, {258, 258, 258}, 5u);
    const bool corners = svo.get({3, 3, 3}) == 5u && svo.get({258, 258, 258}) == 5u && svo.get({2, 3, 3}) == 0u &&
                         svo.get({259, 258, 258}) == 0u;
    std::printf("SVO dense fill: %zu voxels, %zu nodes (%zu bricks), %.2f MB\n", svo.voxelCount(), svo.nodeCount(),
                svo.brickCount(), static_cast<double>(svo.memoryBytes()) / (1024.0 * 1024.0));
    expectTrue(svo.voxelCount() == 256u * 256u * 256u && corners, "dense fill writes exactly the box");
    expectTrue(svo.memoryBytes() < 8u * 1024u * 1024u, "dense 256^3 fill stays under 8 MB");
}

void testSdfContinuityAcrossLeaves() {
    SVO svo;
    SVODesc desc{};
    desc.rootSize = 64.f;
    desc.maxDepth = 6;
    svo.init(desc);
    svo.fill({20, 20, 20}, {35, 35, 35}, 1u);
    svo.carve({27.f, 27.f, 27.f}, 5.f);

    // March a dense line across many leaf boundaries; neighbouring samples must stay close.
    const f32 step = 0.01f;
    f32 worstJump = 0.f;
    f32 previous = svo.sdfQuery({10.f, 27.3f, 27.7f});
    for (f32 x = 10.f + step; x < 50.f; x += step) {
        const f32 value = svo.sdfQuery({x, 27.3f, 27.7f});
        worstJump = std::max(worstJump, std::abs(value - previous));
        previous = value;
    }
    // Trilinear samples are 1 unit apart and bounded by a voxel, so a step of 0.01 moves < 0.02.
    std::printf("SVO SDF continuity: worst jump %.5f over step %.2f\n", worstJump, step);
    expectTrue(worstJump < 0.02f, "SDF has no discontinuities at leaf boundaries");
}

} // namespace

int main() {
    testMillionVoxelRoundTrip();
    testRayCastMatchesBruteForce();
    testCarveTransitions();
    testSdfContinuityAcrossLeaves();
    testDenseFillMemory();

    if (g_failures == 0) {
        std::printf("fuse_b3_svo_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b3_svo_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
