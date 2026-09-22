// B3.9 BVH gate rows (master plan):
//  - SAH build on 100k random AABBs < 500 ms (enforced in optimised builds)
//  - ray cast closest hit for 100k random rays vs 10k objects == brute force
//  - frustum query on 10k objects == O(n) brute force
//  - refit after 1k transform updates is correct (no stale bounds)
//  - BVH frustum cull of 10k objects < 0.1 ms (reported; enforced in optimised builds)
#include <fuse/ecs/math/mat.hpp>
#include <fuse/spatial/bvh.hpp>
#include <fuse/spatial/frustum.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

using fuse::spatial::AABB;
using fuse::spatial::BVH;
using fuse::spatial::BVHLeaf;
using fuse::ecs::vec3;

std::vector<BVHLeaf> randomLeaves(fuse::u32 count, std::mt19937& rng, float worldHalf, float maxSize) {
    std::uniform_real_distribution<float> pos(-worldHalf, worldHalf);
    std::uniform_real_distribution<float> size(0.05f, maxSize);
    std::vector<BVHLeaf> leaves(count);
    for (fuse::u32 i = 0; i < count; ++i) {
        const float x = pos(rng), y = pos(rng), z = pos(rng);
        const float sx = size(rng), sy = size(rng), sz = size(rng);
        leaves[i].primitive_index = i;
        leaves[i].aabb = {{x - sx, y - sy, z - sz, 0.f}, {x + sx, y + sy, z + sz, 0.f}};
    }
    return leaves;
}

double millisSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void testSahBuildTime() {
    std::mt19937 rng(100000u);
    const std::vector<BVHLeaf> leaves = randomLeaves(100'000, rng, 500.f, 2.f);
    BVH bvh;
    const auto start = std::chrono::steady_clock::now();
    bvh.build(leaves);
    const double ms = millisSince(start);
    std::printf("SAH build 100k AABBs: %.1f ms, %zu nodes\n", ms, bvh.node_count());
    expectTrue(bvh.leaf_count() == leaves.size(), "every AABB stored once");
#if defined(NDEBUG)
    expectTrue(ms < 500.0, "SAH build of 100k AABBs < 500 ms");
#endif
}

void testRayCastMatchesBruteForce() {
    std::mt19937 rng(10000u);
    const std::vector<BVHLeaf> leaves = randomLeaves(10'000, rng, 100.f, 1.5f);
    BVH bvh;
    bvh.build(leaves);

    std::uniform_real_distribution<float> unit(-1.f, 1.f);
#if defined(NDEBUG)
    constexpr fuse::u32 kRays = 100'000;
#else
    constexpr fuse::u32 kRays = 10'000; // brute force is O(rays * objects); keep Debug CI quick
#endif
    fuse::u32 mismatches = 0;
    fuse::u32 hits = 0;
    for (fuse::u32 r = 0; r < kRays; ++r) {
        const vec3 origin{unit(rng) * 120.f, unit(rng) * 120.f, unit(rng) * 120.f, 0.f};
        vec3 dir{unit(rng), unit(rng), unit(rng), 0.f};
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len < 1e-3f) {
            continue;
        }
        dir = {dir.x / len, dir.y / len, dir.z / len, 0.f};
        const float maxT = 400.f;

        float bruteT = maxT;
        bool bruteHit = false;
        for (const BVHLeaf& leaf : leaves) {
            const float t = leaf.aabb.ray_intersect(origin, dir);
            if (t >= 0.f && t < bruteT) {
                bruteT = t;
                bruteHit = true;
            }
        }

        BVHLeaf hit{};
        float bvhT = 0.f;
        const bool bvhHit = bvh.ray_cast(origin, dir, maxT, hit, bvhT);
        hits += bruteHit ? 1u : 0u;
        if (bvhHit != bruteHit || (bruteHit && std::abs(bvhT - bruteT) > 1e-4f)) {
            ++mismatches;
        }
    }
    std::printf("ray cast: %u rays, %u hits, %u mismatches vs brute force\n", kRays, hits, mismatches);
    expectTrue(hits > kRays / 20u, "ray set actually hits objects");
    expectTrue(mismatches == 0u, "closest hit identical to brute force");
}

void testFrustumAndRefit() {
    std::mt19937 rng(424242u);
    std::vector<BVHLeaf> leaves = randomLeaves(10'000, rng, 60.f, 1.f);
    BVH bvh;
    bvh.build(leaves);

    const fuse::ecs::mat4 viewProjection =
        fuse::ecs::perspective(70.f, 16.f / 9.f, 0.1f, 80.f) *
        fuse::ecs::look_at({0.f, 0.f, -70.f, 0.f}, {0.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f});
    const fuse::spatial::Frustum frustum = fuse::spatial::extract_frustum(viewProjection);

    auto bruteForce = [&](const std::vector<BVHLeaf>& source) {
        std::vector<fuse::u32> ids;
        for (const BVHLeaf& leaf : source) {
            if (fuse::spatial::test_aabb_frustum(frustum, leaf.aabb.min, leaf.aabb.max)) {
                ids.push_back(leaf.primitive_index);
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    auto viaBvh = [&]() {
        std::vector<BVHLeaf> results;
        bvh.query_frustum(frustum, results);
        std::vector<fuse::u32> ids;
        for (const BVHLeaf& leaf : results) {
            ids.push_back(leaf.primitive_index);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    };

    const std::vector<fuse::u32> expected = bruteForce(leaves);
    expectTrue(!expected.empty() && expected.size() < leaves.size(), "frustum sees part of the scene");
    expectTrue(viaBvh() == expected, "frustum query identical to O(n) brute force");

    // Timing: median of repeated culls.
    std::vector<double> samples;
    std::vector<BVHLeaf> scratch;
    scratch.reserve(leaves.size());
    for (int i = 0; i < 101; ++i) {
        scratch.clear();
        const auto start = std::chrono::steady_clock::now();
        bvh.query_frustum(frustum, scratch);
        samples.push_back(millisSince(start));
    }
    std::sort(samples.begin(), samples.end());
    std::printf("frustum cull 10k: %zu visible, median %.4f ms\n", expected.size(), samples[50]);
#if defined(NDEBUG)
    expectTrue(samples[50] < 0.1, "BVH frustum cull of 10k objects < 0.1 ms");
#endif

    // Refit: move 1k objects (some into view, some out), refit, compare with brute force again.
    std::uniform_int_distribution<fuse::u32> pick(0, static_cast<fuse::u32>(leaves.size()) - 1u);
    std::uniform_real_distribution<float> jump(-60.f, 60.f);
    bool updatesAccepted = true;
    for (int i = 0; i < 1000; ++i) {
        const fuse::u32 index = pick(rng);
        const float dx = jump(rng), dy = jump(rng), dz = jump(rng);
        AABB& box = leaves[index].aabb;
        box = {{box.min.x + dx, box.min.y + dy, box.min.z + dz, 0.f}, {box.max.x + dx, box.max.y + dy, box.max.z + dz, 0.f}};
        updatesAccepted = updatesAccepted && bvh.update_leaf_aabb(index, box);
    }
    bvh.refit();
    expectTrue(updatesAccepted, "update_leaf_aabb accepts every build index");
    expectTrue(viaBvh() == bruteForce(leaves), "after 1k updates + refit, frustum query == brute force");

    const fuse::spatial::BVHNode& root = bvh.nodes()[0];
    bool rootEnclosesAll = true;
    for (const BVHLeaf& leaf : leaves) {
        rootEnclosesAll = rootEnclosesAll && root.aabb.min.x <= leaf.aabb.min.x && root.aabb.max.x >= leaf.aabb.max.x &&
                          root.aabb.min.y <= leaf.aabb.min.y && root.aabb.max.y >= leaf.aabb.max.y &&
                          root.aabb.min.z <= leaf.aabb.min.z && root.aabb.max.z >= leaf.aabb.max.z;
    }
    expectTrue(rootEnclosesAll, "refitted root bounds enclose every moved object (no stale bounds)");
    expectTrue(!bvh.update_leaf_aabb(static_cast<fuse::u32>(leaves.size()), {}), "out-of-range update rejected");
}

} // namespace

int main() {
    testSahBuildTime();
    testRayCastMatchesBruteForce();
    testFrustumAndRefit();

    if (g_failures == 0) {
        std::printf("fuse_b3_bvh_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b3_bvh_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
