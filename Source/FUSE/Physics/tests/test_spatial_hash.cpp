#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/physics_data.hpp>

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(std::size_t actual, std::size_t expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %zu, got %zu)\n", message, expected, actual);
        ++g_failures;
    }
}

bool pairListsContainAll(
    const std::vector<fuse::physics::broadphase::CandidatePair>& superset,
    const std::vector<fuse::physics::broadphase::CandidatePair>& subset) {
    auto canonical = [](fuse::physics::broadphase::CandidatePair pair) {
        if (pair.bodyA > pair.bodyB) {
            std::swap(pair.bodyA, pair.bodyB);
        }
        return pair;
    };

    std::vector<fuse::physics::broadphase::CandidatePair> left = superset;
    for (auto& pair : left) {
        pair = canonical(pair);
    }
    std::sort(left.begin(), left.end(), [](const auto& a, const auto& b) {
        return a.bodyA < b.bodyA || (a.bodyA == b.bodyA && a.bodyB < b.bodyB);
    });

    for (const fuse::physics::broadphase::CandidatePair& rawPair : subset) {
        const fuse::physics::broadphase::CandidatePair pair = canonical(rawPair);
        const auto it = std::lower_bound(left.begin(), left.end(), pair, [](const auto& a, const auto& b) {
            return a.bodyA < b.bodyA || (a.bodyA == b.bodyA && a.bodyB < b.bodyB);
        });
        if (it == left.end() || it->bodyA != pair.bodyA || it->bodyB != pair.bodyB) {
            return false;
        }
    }
    return true;
}

bool pairListsEqual(const std::vector<fuse::physics::broadphase::CandidatePair>& lhs,
                    const std::vector<fuse::physics::broadphase::CandidatePair>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    auto canonical = [](fuse::physics::broadphase::CandidatePair pair) {
        if (pair.bodyA > pair.bodyB) {
            std::swap(pair.bodyA, pair.bodyB);
        }
        return pair;
    };

    std::vector<fuse::physics::broadphase::CandidatePair> left = lhs;
    std::vector<fuse::physics::broadphase::CandidatePair> right = rhs;
    for (auto& pair : left) {
        pair = canonical(pair);
    }
    for (auto& pair : right) {
        pair = canonical(pair);
    }

    std::sort(left.begin(), left.end(), [](const auto& a, const auto& b) {
        return a.bodyA < b.bodyA || (a.bodyA == b.bodyA && a.bodyB < b.bodyB);
    });
    std::sort(right.begin(), right.end(), [](const auto& a, const auto& b) {
        return a.bodyA < b.bodyA || (a.bodyA == b.bodyA && a.bodyB < b.bodyB);
    });

    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].bodyA != right[i].bodyA || left[i].bodyB != right[i].bodyB) {
            return false;
        }
    }
    return true;
}

template <typename Body>
void withScheduler(fuse::u32 workers, Body&& body) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
    body();
    scheduler.shutdown();
}

void testSpatialHashFunction() {
    const fuse::u32 a = fuse::physics::broadphase::spatialHash(1, 2, 3, 1021u);
    const fuse::u32 b = fuse::physics::broadphase::spatialHash(1, 2, 3, 1021u);
    const fuse::u32 c = fuse::physics::broadphase::spatialHash(4, 5, 6, 1021u);
    expectTrue(a == b, "spatial hash is deterministic");
    expectTrue(a != c, "spatial hash varies by cell");
}

void testBroadphaseFindsOverlappingPair() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 128;
    params.bodyCount = bodies.count();

    const auto pairs = fuse::physics::broadphase::runBroadphase(bodies, shapes, params);
    expectTrue(!pairs.empty(), "broadphase emits candidate pair for overlapping spheres");
}

void testBodiesStraddlingCells() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({1.9f, 0.f, 0.f}, 1.f);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 256;
    params.bodyCount = bodies.count();

    const auto pairs = fuse::physics::broadphase::runBroadphase(bodies, shapes, params);
    expectTrue(!pairs.empty(), "bodies straddling cells still emit candidate pairs");
}

std::vector<fuse::physics::broadphase::CandidatePair> bruteForcePairs(
    const fuse::physics::RigidBodySoA& bodies,
    const fuse::physics::CollisionShapeSoA& shapes) {
    std::vector<fuse::physics::broadphase::CandidatePair> pairs;
    for (fuse::u32 shapeA = 0; shapeA < shapes.count(); ++shapeA) {
        const fuse::u32 bodyA = shapes.bodyIndices[shapeA];
        if (bodyA >= bodies.count()) {
            continue;
        }
        const fuse::f32 radiusA = shapes.params[shapeA].x;
        const fuse::physics::vec3 posA = bodies.positions[bodyA];

        for (fuse::u32 shapeB = shapeA + 1; shapeB < shapes.count(); ++shapeB) {
            const fuse::u32 bodyB = shapes.bodyIndices[shapeB];
            if (bodyB >= bodies.count() || bodyA == bodyB) {
                continue;
            }
            const fuse::f32 radiusB = shapes.params[shapeB].x;
            const fuse::physics::vec3 posB = bodies.positions[bodyB];
            const fuse::f32 dx = posB.x - posA.x;
            const fuse::f32 dy = posB.y - posA.y;
            const fuse::f32 dz = posB.z - posA.z;
            const fuse::f32 distSq = dx * dx + dy * dy + dz * dz;
            const fuse::f32 reach = radiusA + radiusB;
            if (distSq <= reach * reach) {
                fuse::u32 a = bodyA;
                fuse::u32 b = bodyB;
                if (a > b) {
                    std::swap(a, b);
                }
                pairs.push_back({a, b});
            }
        }
    }
    return pairs;
}

void populateRandomSpheres(fuse::u32 count,
                           fuse::physics::RigidBodySoA& bodies,
                           fuse::physics::CollisionShapeSoA& shapes) {
    bodies.clear();
    shapes.clear();
    for (fuse::u32 i = 0; i < count; ++i) {
        const fuse::f32 x = static_cast<fuse::f32>((i * 17u) % 100u) * 0.25f;
        const fuse::f32 y = static_cast<fuse::f32>((i * 31u) % 100u) * 0.25f;
        const fuse::f32 z = static_cast<fuse::f32>((i * 7u) % 100u) * 0.25f;
        bodies.addBody({x, y, z}, 1.f);
        shapes.addShape(fuse::physics::CollisionShapeType::Sphere, i, {0.5f, 0.f, 0.f});
    }
}

void testBroadphaseMatchesBruteForce() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    constexpr fuse::u32 kSphereCount = 256u;
    populateRandomSpheres(kSphereCount, bodies, shapes);

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 2048;
    params.bodyCount = bodies.count();

    const auto hashPairs = fuse::physics::broadphase::runBroadphase(bodies, shapes, params);
    const auto brutePairs = bruteForcePairs(bodies, shapes);
    expectTrue(pairListsContainAll(hashPairs, brutePairs),
               "spatial hash includes all brute-force overlapping pairs");
    expectTrue(!brutePairs.empty(), "random scene produces at least one true overlap");
}

void testBroadphaseParallelParity() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    populateRandomSpheres(128u, bodies, shapes);

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 1024;
    params.bodyCount = bodies.count();

    std::vector<fuse::physics::broadphase::CandidatePair> singleThreaded;
    withScheduler(0u, [&] {
        singleThreaded = fuse::physics::broadphase::runBroadphase(bodies, shapes, params);
    });

    std::vector<fuse::physics::broadphase::CandidatePair> multiThreaded;
    withScheduler(4u, [&] {
        multiThreaded = fuse::physics::broadphase::runBroadphase(bodies, shapes, params);
    });

    expectTrue(pairListsEqual(singleThreaded, multiThreaded),
               "parallel broadphase matches single-thread scheduler output");
}

void testBroadphaseEmptyScene() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 128;

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectEq(buffer.activeCount, 0u, "empty scene emits zero candidate pairs");
    expectTrue(buffer.toVector().empty(), "empty scene SoA buffer vector is empty");
}

void testAabbOverlapStub() {
    const fuse::physics::aabb boxA = fuse::physics::broadphase::aabbFromSphere({0.f, 0.f, 0.f}, 1.f);
    const fuse::physics::aabb boxB = fuse::physics::broadphase::aabbFromSphere({1.5f, 0.f, 0.f}, 1.f);
    const fuse::physics::aabb boxC = fuse::physics::broadphase::aabbFromSphere({3.f, 0.f, 0.f}, 1.f);

    expectTrue(fuse::physics::broadphase::aabbOverlap(boxA, boxB), "touching AABBs overlap");
    expectTrue(!fuse::physics::broadphase::aabbOverlap(boxA, boxC), "separated AABBs do not overlap");
    expectTrue(fuse::physics::broadphase::sphereAabbOverlap({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f),
               "sphere AABB overlap stub matches touching spheres");
    expectTrue(!fuse::physics::broadphase::sphereAabbOverlap({0.f, 0.f, 0.f}, 1.f, {3.f, 0.f, 0.f}, 1.f),
               "sphere AABB overlap stub rejects separated spheres");
}

void testPairBufferSoAClearReuse() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.reserve(8u);
    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    expectEq(buffer.activeCount, 2u, "pair buffer push increments active count");

    const std::size_t capacityAfterPush = buffer.bodyA.capacity();
    buffer.clear();
    expectEq(buffer.activeCount, 0u, "pair buffer clear resets active count");
    expectTrue(buffer.isEmpty(), "cleared pair buffer reports empty");
    expectTrue(buffer.bodyA.capacity() >= capacityAfterPush,
               "pair buffer clear preserves reserved capacity");

    buffer.push(4u, 5u);
    expectEq(buffer.activeCount, 1u, "pair buffer reuses storage after clear");
    expectEq(buffer.toVector().size(), 1u, "pair buffer toVector after reuse");
    expectTrue(buffer.containsCanonicalPair(4u, 5u), "containsCanonicalPair finds pushed pair");
}

void testPairBufferSlotCompact() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(4u);
    buffer.writeSlot(0u, 1u, 2u);
    buffer.writeSlot(2u, 3u, 4u);
    expectEq(buffer.compact(), 2u, "slot compact gathers valid pair slots");
    expectTrue(buffer.containsCanonicalPair(1u, 2u), "compact preserves first slot pair");
    expectTrue(buffer.containsCanonicalPair(3u, 4u), "compact preserves second slot pair");
    expectEq(buffer.toVector().size(), 2u, "toVector reflects compacted active count");
}

void testPairBufferMaxCapacityClamp() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(2u);
    expectTrue(buffer.push(0u, 1u), "push accepts pair under capacity");
    expectTrue(buffer.push(2u, 3u), "push accepts second pair at capacity");
    expectTrue(!buffer.push(4u, 5u), "push rejects pair beyond max capacity");
    expectEq(buffer.droppedCount, 1u, "dropped count tracks clamped pushes");
    expectEq(buffer.activeCount, 2u, "active count stops at max capacity");
}

void testPairBufferApplyMaxCapacityClamp() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.push(2u, 3u);
    buffer.push(0u, 1u);
    buffer.push(4u, 5u);
    expectEq(buffer.activeCount, 3u, "buffer holds three pairs before post clamp");

    buffer.setMaxCapacity(2u);
    expectEq(buffer.applyMaxCapacityClamp(), 2u, "applyMaxCapacityClamp truncates to max capacity");
    expectEq(buffer.activeCount, 2u, "active count reflects post clamp");
    expectEq(buffer.droppedCount, 1u, "dropped count tracks post clamp excess");
    expectTrue(buffer.isSortedCanonical(), "applyMaxCapacityClamp leaves canonical sort order");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "clamp keeps lowest canonical pair");
    expectTrue(buffer.containsCanonicalPair(2u, 3u), "clamp keeps second canonical pair");
}

void testPairBufferCompactAndClamp() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(2u, 2u, 3u);
    expectEq(buffer.compactAndClamp(), 1u, "compactAndClamp gathers then clamps");
    expectEq(buffer.activeCount, 1u, "compactAndClamp active count");
    expectTrue(buffer.isSortedCanonical(), "compactAndClamp output is canonically sorted");
}

void testBroadphaseEmptyShapesGuard() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 128;

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "bodies without shapes emit zero pairs");
    expectEq(buffer.activeCount, 0u, "empty shapes guard leaves zero active pairs");
}

void testBroadphaseMaxCapacityIntegration() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 2, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 4.f;
    params.tableSize = 64;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectEq(buffer.activeCount, 1u, "broadphase honors maxCapacity after dedupe");
    expectTrue(buffer.droppedCount >= 1u, "broadphase records dropped pairs beyond maxCapacity");
}

void testRefineBroadphasePairsParallel() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 2, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 128;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.push(0u, 1u);
    buffer.push(0u, 2u);
    expectEq(buffer.activeCount, 2u, "refine test starts with two candidate pairs");

    fuse::physics::broadphase::PairBufferSoA singleThreaded = buffer;
    fuse::physics::broadphase::PairBufferSoA multiThreaded = buffer;
    withScheduler(0u, [&] {
        fuse::physics::broadphase::refineBroadphasePairsParallel(bodies, shapes, singleThreaded);
    });
    withScheduler(4u, [&] {
        fuse::physics::broadphase::refineBroadphasePairsParallel(bodies, shapes, multiThreaded);
    });

    expectEq(singleThreaded.activeCount, 1u, "refine removes separated pair");
    expectTrue(singleThreaded.containsCanonicalPair(0u, 1u), "refine keeps overlapping pair");
    expectTrue(pairListsEqual(singleThreaded.toVector(), multiThreaded.toVector()),
               "parallel refine matches single-thread output");
}

void testRefineBroadphaseEmptyBufferGuard() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    fuse::physics::broadphase::refineBroadphasePairsParallel(bodies, shapes, buffer);
    expectTrue(buffer.isEmpty(), "refine on empty buffer is a no-op");
}

void testBroadphase2DParallelParity() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    populateRandomSpheres(64u, bodies, shapes);

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 512;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA singleThreaded;
    fuse::physics::broadphase::PairBufferSoA multiThreaded;
    withScheduler(0u, [&] {
        fuse::physics::broadphase::runBroadphase2DIntoBuffer(bodies, shapes, params, singleThreaded);
    });
    withScheduler(4u, [&] {
        fuse::physics::broadphase::runBroadphase2DIntoBuffer(bodies, shapes, params, multiThreaded);
    });

    expectTrue(pairListsEqual(singleThreaded.toVector(), multiThreaded.toVector()),
               "2D SoA broadphase matches single-thread output");
    expectEq(singleThreaded.activeCount, multiThreaded.activeCount, "2D SoA pair counts match");
}

void testBroadphaseSoABufferParity() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    populateRandomSpheres(128u, bodies, shapes);

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 1024;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA singleThreaded;
    fuse::physics::broadphase::PairBufferSoA multiThreaded;
    withScheduler(0u, [&] {
        fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, singleThreaded);
    });
    withScheduler(4u, [&] {
        fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, multiThreaded);
    });

    expectTrue(pairListsEqual(singleThreaded.toVector(), multiThreaded.toVector()),
               "SoA buffer parallel broadphase matches single-thread output");
    expectEq(singleThreaded.activeCount, multiThreaded.activeCount, "SoA buffer pair counts match");
}

void testBroadphasePairCount() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 2, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 128;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectEq(buffer.activeCount, 1u, "three spheres with one overlap emits one pair");
}

void testBroadphaseLargeScene() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    constexpr fuse::u32 kSphereCount = 1024u;
    populateRandomSpheres(kSphereCount, bodies, shapes);

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 4096;
    params.bodyCount = bodies.count();

    std::vector<fuse::physics::broadphase::CandidatePair> singleThreaded;
    withScheduler(0u, [&] {
        singleThreaded = fuse::physics::broadphase::runBroadphase(bodies, shapes, params);
    });

    std::vector<fuse::physics::broadphase::CandidatePair> multiThreaded;
    withScheduler(4u, [&] {
        multiThreaded = fuse::physics::broadphase::runBroadphase(bodies, shapes, params);
    });

    expectTrue(pairListsEqual(singleThreaded, multiThreaded),
               "1k-scene parallel broadphase matches single-thread output");
    expectEq(singleThreaded.size(), multiThreaded.size(), "1k-scene pair counts match");
}

} // namespace

int main() {
    testSpatialHashFunction();
    testAabbOverlapStub();
    testPairBufferSoAClearReuse();
    testPairBufferSlotCompact();
    testPairBufferMaxCapacityClamp();
    testPairBufferApplyMaxCapacityClamp();
    testPairBufferCompactAndClamp();
    testBroadphaseEmptyScene();
    testBroadphaseEmptyShapesGuard();
    testBroadphaseFindsOverlappingPair();
    testBodiesStraddlingCells();
    testBroadphasePairCount();
    testBroadphaseMaxCapacityIntegration();
    testBroadphaseMatchesBruteForce();
    testBroadphaseParallelParity();
    testBroadphaseSoABufferParity();
    testBroadphase2DParallelParity();
    testRefineBroadphasePairsParallel();
    testRefineBroadphaseEmptyBufferGuard();
    testBroadphaseLargeScene();

    if (g_failures == 0) {
        std::printf("fuse_physics_broadphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_broadphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
