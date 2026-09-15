#include <fuse/ecs/math/mat.hpp>
#include <fuse/spatial/bvh.hpp>

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

fuse::spatial::AABB make_aabb(fuse::f32 x, fuse::f32 y, fuse::f32 z, fuse::f32 size) {
    return {{x, y, z, 0.f}, {x + size, y + size, z + size, 0.f}};
}

void testRayCastHitsClosest() {
    std::vector<fuse::spatial::BVHLeaf> leaves;
    leaves.push_back({fuse::spatial::BVHLeafType::Mesh, {1, 1}, 0, make_aabb(0.f, 0.f, 0.f, 1.f)});
    leaves.push_back({fuse::spatial::BVHLeafType::Mesh, {2, 1}, 0, make_aabb(5.f, 0.f, 0.f, 1.f)});

    fuse::spatial::BVH bvh;
    bvh.build(leaves);

    fuse::spatial::BVHLeaf hit{};
    fuse::f32 t = 0.f;
    const bool found =
        bvh.ray_cast({-1.f, 0.5f, 0.5f, 0.f}, {1.f, 0.f, 0.f, 0.f}, 100.f, hit, t);
    expectTrue(found, "ray cast finds a hit");
    expectTrue(hit.entity.index == 1u, "closest leaf is returned");
}

void testFrustumQueryMatchesBruteForce() {
    std::mt19937 rng(42);
    std::uniform_real_distribution<fuse::f32> dist(-20.f, 20.f);

    std::vector<fuse::spatial::BVHLeaf> leaves;
    for (fuse::u32 i = 0; i < 128; ++i) {
        const fuse::f32 x = dist(rng);
        const fuse::f32 y = dist(rng);
        const fuse::f32 z = dist(rng);
        leaves.push_back({fuse::spatial::BVHLeafType::Mesh, {i + 1, 1}, 0, make_aabb(x, y, z, 1.f)});
    }

    fuse::spatial::BVH bvh;
    bvh.build(leaves);

    const fuse::ecs::mat4 view_projection =
        fuse::ecs::perspective(75.f, 16.f / 9.f, 0.1f, 100.f);
    const fuse::spatial::Frustum frustum = fuse::spatial::extract_frustum(view_projection);

    std::vector<fuse::spatial::BVHLeaf> bvh_hits;
    bvh.query_frustum(frustum, bvh_hits);

    std::vector<fuse::spatial::BVHLeaf> brute_hits;
    for (const fuse::spatial::BVHLeaf& leaf : leaves) {
        if (fuse::spatial::test_aabb_frustum(frustum, leaf.aabb.min, leaf.aabb.max)) {
            brute_hits.push_back(leaf);
        }
    }

    expectTrue(bvh_hits.size() == brute_hits.size(), "bvh frustum query matches brute force count");
}

void testRefitPreservesHierarchy() {
    std::vector<fuse::spatial::BVHLeaf> leaves;
    leaves.push_back({fuse::spatial::BVHLeafType::Mesh, {1, 1}, 0, make_aabb(0.f, 0.f, 0.f, 1.f)});
    leaves.push_back({fuse::spatial::BVHLeafType::Mesh, {2, 1}, 0, make_aabb(4.f, 0.f, 0.f, 1.f)});

    fuse::spatial::BVH bvh;
    bvh.build(leaves);
    const fuse::usize nodes_before = bvh.node_count();
    bvh.refit();
    expectTrue(bvh.node_count() == nodes_before, "refit preserves hierarchy");
}

} // namespace

int main() {
    testRayCastHitsClosest();
    testFrustumQueryMatchesBruteForce();
    testRefitPreservesHierarchy();

    if (g_failures == 0) {
        std::printf("fuse_ecs bvh tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs bvh tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
