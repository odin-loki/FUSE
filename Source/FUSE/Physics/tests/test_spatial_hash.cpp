#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/physics_data.hpp>

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    expectTrue(!buffer.isFull(), "buffer under capacity is not full");
    expectEq(buffer.remainingCapacity(), 2u, "remainingCapacity reports headroom");
    expectTrue(!buffer.wouldRejectPush(0u, 1u), "wouldRejectPush accepts valid pair under capacity");
    expectTrue(buffer.push(0u, 1u), "push accepts pair under capacity");
    expectTrue(buffer.push(2u, 3u), "push accepts second pair at capacity");
    expectTrue(buffer.isFull(), "buffer at max capacity reports full");
    expectEq(buffer.remainingCapacity(), 0u, "remainingCapacity is zero when full");
    expectTrue(buffer.wouldRejectPush(4u, 5u), "wouldRejectPush rejects when full");
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

void testEmptyPairGuards() {
    expectTrue(fuse::physics::broadphase::isEmptyCandidatePair(1u, 1u),
               "self-pair is empty candidate pair");
    expectTrue(!fuse::physics::broadphase::isValidCandidatePair(2u, 2u),
               "self-pair fails validity guard");
    expectTrue(fuse::physics::broadphase::isValidCandidatePair(0u, 1u, 2u),
               "in-range pair passes validity guard");
    expectTrue(!fuse::physics::broadphase::isValidCandidatePair(0u, 2u, 2u),
               "out-of-range pair fails validity guard");

    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(!buffer.push(3u, 3u), "push rejects self-pair");
    expectTrue(buffer.canSkipSoAIteration(), "empty buffer skips SoA iteration");
    expectTrue(!buffer.hasValidPairs(), "empty buffer has no valid pairs");
    expectTrue(!buffer.containsCanonicalPair(0u, 0u), "contains rejects self-pair lookup");
    expectTrue(buffer.toVector().empty(), "toVector early-outs on empty buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 1u, 1u);
    buffer.writeSlot(1u, 0u, 2u);
    expectEq(buffer.compact(), 1u, "writeSlot rejects self-pair slots");
    expectTrue(buffer.slotIsValid(0u), "valid slot reports active");
    expectTrue(!buffer.slotIsValid(1u), "invalid slot reports inactive after compact");
}

void testCellClampHelpers() {
    expectEq(fuse::physics::broadphase::clampTableSize(0u), 1u, "clampTableSize minimum is one");
    expectEq(fuse::physics::broadphase::clampHashKey(17u, 0u), 0u, "clampHashKey wraps with clamped table size");
    expectEq(fuse::physics::broadphase::clampMaxCellSpanPerAxis(8u), 8u, "clampMaxCellSpanPerAxis preserves positive span");

    fuse::physics::broadphase::SpatialHashParams rawParams;
    rawParams.cellSize = 0.f;
    rawParams.tableSize = 0u;
    const fuse::physics::broadphase::SpatialHashParams safeParams =
        fuse::physics::broadphase::sanitizeSpatialHashParams(rawParams);
    expectEq(safeParams.cellSize, 1.f, "sanitizeSpatialHashParams clamps cell size");
    expectEq(safeParams.tableSize, 1u, "sanitizeSpatialHashParams clamps table size");

    expectEq(fuse::physics::broadphase::clampCellCoord(5, 0, 3), 3, "clampCellCoord clamps high bound");
    expectEq(fuse::physics::broadphase::clampCellCoord(-2, 0, 3), 0, "clampCellCoord clamps low bound");

    fuse::physics::broadphase::CellRange3 wideRange = {
        {-100, -100, -100},
        {100, 100, 100},
    };
    const fuse::physics::broadphase::CellRange3 clamped =
        fuse::physics::broadphase::clampCellRange3(wideRange, 8u);
    expectTrue(clamped.maxCell.x - clamped.minCell.x <= 8, "clampCellRange3 limits x span");
    expectTrue(clamped.maxCell.y - clamped.minCell.y <= 8, "clampCellRange3 limits y span");
    expectTrue(clamped.maxCell.z - clamped.minCell.z <= 8, "clampCellRange3 limits z span");

    const fuse::physics::broadphase::CellRange3 sphereRange =
        fuse::physics::broadphase::cellRangeFromSphere({0.f, 0.f, 0.f}, 512.f, 1.f, 4u);
    expectTrue(sphereRange.maxCell.x - sphereRange.minCell.x <= 4, "cellRangeFromSphere applies span clamp");

    expectEq(fuse::physics::broadphase::cellOccupancyCount(sphereRange), 125u,
             "cellOccupancyCount counts clamped 5x5x5 span");
    expectTrue(!fuse::physics::broadphase::exceedsCellOccupancyBudget(
                   sphereRange, fuse::physics::broadphase::cellOccupancyBudgetFromSpan(4u)),
               "clamped range fits occupancy budget derived from span");
    expectTrue(fuse::physics::broadphase::exceedsCellOccupancyBudget(wideRange, 64u),
               "unclamped wide range exceeds occupancy budget");
}

void testBroadphaseCellSpanClampIntegration() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({500.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {256.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 256;
    params.maxCellSpanPerAxis = 4u;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "clamped huge sphere does not flood pair buffer with distant body");
}

void testPairBufferSoAIterationEarlyOuts() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(buffer.isSortedCanonical(), "empty buffer is canonically sorted");
    expectTrue(buffer.canSkipDedupe(), "empty buffer skips dedupe");
    expectTrue(buffer.canSkipCompaction(), "empty buffer skips compaction");
    expectTrue(buffer.canSkipRefine(), "empty buffer skips AABB refine");
    expectEq(buffer.countValidSlots(), 0u, "countValidSlots early-outs when empty");
    expectEq(buffer.applyMaxCapacityClamp(), 0u, "applyMaxCapacityClamp early-outs when empty");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs when empty");
    expectTrue(buffer.canSkipSort(), "empty buffer skips canonical sort");
    expectTrue(buffer.canSkipRefine(), "empty buffer skips AABB refine");
    expectTrue(buffer.canSkipCompact(), "empty buffer skips compact gather");

    buffer.push(0u, 1u);
    expectTrue(buffer.hasValidPairs(), "non-empty buffer reports valid pairs");
    expectTrue(!buffer.canSkipSoAIteration(), "non-empty buffer does not skip iteration");
    expectTrue(buffer.canSkipDedupe(), "single-pair buffer skips dedupe");
    expectEq(buffer.toVector().size(), 1u, "toVector gathers valid pair after push");
}

void testCandidatePairRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::candidatePairRejectReason(1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::SelfPair),
             "self-pair reports SelfPair reject reason");
                 fuse::physics::broadphase::candidatePairRejectReason(0u, 2u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::OutOfRangeBody),
             "out-of-range pair reports OutOfRangeBody reject reason");
                 fuse::physics::broadphase::candidatePairRejectReason(0u, 1u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::None),
             "in-range pair reports None reject reason");

void testCandidatePairRejectReasonName() {
    expectTrue(std::strcmp(fuse::physics::broadphase::candidatePairRejectReasonName(
                               fuse::physics::broadphase::CandidatePairRejectReason::None),
                           "None") == 0,
               "None reject reason has stable label");
                               fuse::physics::broadphase::CandidatePairRejectReason::SelfPair),
                           "SelfPair") == 0,
               "SelfPair reject reason has stable label");
                               fuse::physics::broadphase::CandidatePairRejectReason::OutOfRangeBody),
                           "OutOfRangeBody") == 0,
               "OutOfRangeBody reject reason has stable label");
    expectTrue(std::strcmp(fuse::physics::broadphase::candidate_pair_reject_reason_name(
               "reject reason name for SelfPair");
    expectTrue(fuse::physics::broadphase::isOutOfRangeCandidatePair(0u, 2u, 2u),
               "isOutOfRangeCandidatePair detects OOB indices");
    expectTrue(!fuse::physics::broadphase::isOutOfRangeCandidatePair(0u, 1u, 2u),
               "isOutOfRangeCandidatePair accepts in-range pair");
    expectTrue(std::strcmp(fuse::physics::broadphase::candidatePairRejectReasonName(
                               fuse::physics::broadphase::CandidatePairRejectReason::AabbSeparated),
                           "AabbSeparated") == 0,
               "AabbSeparated reject reason has stable label");
                               fuse::physics::broadphase::CandidatePairRejectReason::BufferFull),
                           "BufferFull") == 0,
               "BufferFull reject reason has stable label");
}

void testRejectedCandidatePairGuards() {
    expectTrue(fuse::physics::broadphase::isRejectedCandidatePair(1u, 1u),
               "self-pair is rejected");
    expectTrue(!fuse::physics::broadphase::isRejectedCandidatePair(0u, 1u, 2u),
               "in-range pair is not rejected");
    expectTrue(fuse::physics::broadphase::isRejectedCandidatePair(0u, 2u, 2u),
               "out-of-range pair is rejected");

void testCanSkipBroadphaseEmptySetGuard() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "empty bodies and shapes skip broadphase");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
               "bodies without shapes skip broadphase");

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    expectTrue(!fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "bodies with shapes do not skip broadphase");

void testCellSpanExceedsClampGuards() {
    const fuse::physics::broadphase::CellRange3 wideRange = {
        {-10, -10, -10},
        {10, 10, 10},
    };
    expectTrue(fuse::physics::broadphase::cellSpanExceedsClamp(wideRange, 8u),
               "wide 3D range exceeds per-axis clamp");
    expectTrue(!fuse::physics::broadphase::cellSpanExceedsClamp(wideRange, 0u),
               "zero clamp limit is unlimited");

    const fuse::physics::broadphase::CellRange3 clamped =
        fuse::physics::broadphase::clampCellRange3(wideRange, 8u);
    const fuse::physics::ivec3 spanBefore = fuse::physics::broadphase::cellSpanPerAxis(wideRange);
    const fuse::physics::ivec3 spanAfter = fuse::physics::broadphase::cellSpanPerAxis(clamped);
    expectTrue(spanAfter.x < spanBefore.x, "clamp reduces x span for oversized range");
    expectTrue(spanAfter.y < spanBefore.y, "clamp reduces y span for oversized range");
    expectTrue(spanAfter.z < spanBefore.z, "clamp reduces z span for oversized range");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {20, 5}};
    expectTrue(fuse::physics::broadphase::cellSpanExceedsClamp(planeRange, 10u),
               "wide 2D range exceeds per-axis clamp");

void testCellOccupancyBudgetGuards() {
    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {3, 3, 3}};
    expectTrue(fuse::physics::broadphase::exceedsCellOccupancyBudget(unitRange, 32u),
               "64-cell occupancy exceeds budget of 32");
    expectTrue(!fuse::physics::broadphase::exceedsCellOccupancyBudget(unitRange, 0u),
               "zero budget is unlimited");

    const fuse::u32 afterClamp =
        fuse::physics::broadphase::estimateCellOccupancyCountAfterClamp(unitRange, 2u);
    expectTrue(afterClamp <= 27u, "after-clamp occupancy is bounded by span clamp");
    expectTrue(afterClamp < fuse::physics::broadphase::estimateCellOccupancyCount(unitRange),
               "clamp reduces occupancy for oversized range");

void testPairBufferLastRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);

    expectTrue(!buffer.push(0u, 0u), "push rejects self-pair");
    expectEq(static_cast<fuse::u32>(buffer.lastRejectReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::SelfPair),
             "self-pair push records SelfPair reject reason");

    expectTrue(buffer.push(0u, 1u), "push accepts first valid pair");
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::None),
             "successful push clears reject reason");

    expectTrue(!buffer.push(2u, 3u), "push rejects when buffer is full");
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::BufferFull),
             "full buffer push records BufferFull reject reason");

void testPairBufferRefineAndInvalidSlotGuards() {
    expectTrue(buffer.canSkipRefine(), "empty buffer skips refine");

    buffer.push(0u, 1u);
    expectTrue(!buffer.canSkipRefine(), "non-empty buffer does not skip refine");
    expectTrue(!buffer.hasInvalidSlots(), "dense buffer has no invalid slots");

    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    buffer.invalidateSlot(2u);
    expectTrue(buffer.hasInvalidSlots(), "invalidated slot is detected");
    expectTrue(!buffer.canSkipCompaction(), "invalid slots require compaction");

void testCandidatePairAabbRejectReason() {

    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 2, {1.f, 0.f, 0.f});

    const fuse::physics::broadphase::CandidatePair overlapping{0u, 1u};
    const fuse::physics::broadphase::CandidatePair separated{0u, 2u};
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::candidatePairRejectReason(overlapping, bodies, shapes)),
             "overlapping pair passes AABB reject reason");
                 fuse::physics::broadphase::candidatePairRejectReason(separated, bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::AabbSeparated),
             "separated pair reports AabbSeparated reject reason");
}

void testCellRangeFromAabbHelpers() {
    expectEq(fuse::physics::broadphase::clampCellSize(0.f), 1.f, "clampCellSize falls back to one");
    expectEq(fuse::physics::broadphase::clampCellSize(-2.f), 1.f, "clampCellSize rejects negative size");
    expectEq(fuse::physics::broadphase::clampCellSize(2.f), 2.f, "clampCellSize preserves positive size");

    const fuse::physics::aabb bounds = fuse::physics::broadphase::aabbFromBox({0.f, 0.f, 0.f}, {1.f, 2.f, 3.f});
    const fuse::physics::broadphase::CellRange3 aabbRange =
        fuse::physics::broadphase::cellRangeFromAabb(bounds, 1.f, 0u);
    const fuse::physics::ivec3 span = fuse::physics::broadphase::cellSpanPerAxis(aabbRange);
    expectTrue(span.x >= 2, "cellRangeFromAabb spans x for box half-extents");
    expectTrue(span.y >= 4, "cellRangeFromAabb spans y for box half-extents");
    expectTrue(span.z >= 6, "cellRangeFromAabb spans z for box half-extents");

    const fuse::physics::broadphase::CellRange3 boxRange =
        fuse::physics::broadphase::cellRangeFromBox({0.f, 0.f, 0.f}, {0.5f, 0.5f, 0.5f}, 1.f, 0u);
    expectTrue(!fuse::physics::broadphase::isEmptyCellRange(boxRange), "cellRangeFromBox is non-empty");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::isEmptyCellRange(inverted), "inverted range is empty");
    expectEq(fuse::physics::broadphase::cellSpanPerAxis(inverted).x, 0, "empty range reports zero span");

void testPairBufferCapacityGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(2u);
    expectTrue(!buffer.isFull(), "empty buffer is not full");
    expectEq(buffer.remainingCapacity(), 2u, "empty buffer reports full remaining capacity");
    expectTrue(!buffer.wouldRejectPush(0u, 1u), "wouldRejectPush accepts valid pair under capacity");

    expectTrue(buffer.push(0u, 1u), "push accepts pair under capacity");
    expectTrue(!buffer.isFull(), "partial buffer is not full");
    expectEq(buffer.remainingCapacity(), 1u, "partial buffer reports one remaining slot");

    expectTrue(buffer.push(2u, 3u), "push accepts second pair at capacity");
    expectTrue(buffer.isFull(), "buffer at max capacity reports full");
    expectEq(buffer.remainingCapacity(), 0u, "full buffer reports zero remaining capacity");
    expectTrue(buffer.wouldRejectPush(4u, 5u), "wouldRejectPush rejects when full");
    expectTrue(!buffer.push(4u, 5u), "push on full buffer is rejected");
    expectEq(buffer.droppedCount, 1u, "push on full buffer increments dropped count");

    expectTrue(!buffer.canApplyMaxCapacityClamp(), "at-capacity buffer does not need post clamp");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(2u, 3u);
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(4u, 5u);
    overflowBuffer.setMaxCapacity(2u);
    expectTrue(overflowBuffer.canApplyMaxCapacityClamp(), "overflow buffer requests post clamp");
    expectEq(overflowBuffer.applyMaxCapacityClamp(), 2u, "canApplyMaxCapacityClamp gate truncates overflow");
    expectEq(overflowBuffer.activeCount, 2u, "setMaxCapacity trims overflow in place");
    expectTrue(!overflowBuffer.canApplyMaxCapacityClamp(), "trimmed buffer does not need post clamp");
}

void testPairBufferPreparePairSlotsZeroGuard() {
    buffer.preparePairSlots(0u);
    expectTrue(buffer.canSkipSoAIteration(), "preparePairSlots(0) clears slot storage");
    expectTrue(buffer.isEmpty(), "preparePairSlots(0) leaves empty buffer");
    expectTrue(buffer.canSkipCompactAndClamp(), "zero-prepared buffer skips compactAndClamp");
    expectTrue(buffer.canSkipRefineIteration(), "zero-prepared buffer skips refine iteration");
    expectEq(buffer.compact(), 0u, "compact on zero slots returns zero");
    expectEq(buffer.countValidSlots(), 0u, "countValidSlots on zero slots returns zero");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp on zero-prepared buffer is no-op");
}

void testNormalizeSpatialHashParamsAndOccupancy() {
    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 0.f;
    params.tableSize = 0u;
    const fuse::physics::broadphase::SpatialHashParams normalized =
        fuse::physics::broadphase::normalizeSpatialHashParams(params);
    expectEq(normalized.cellSize, 1.f, "normalizeSpatialHashParams clamps cell size");
    expectEq(normalized.tableSize, 1u, "normalizeSpatialHashParams clamps table size");

    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {1, 1, 1}};
    expectEq(fuse::physics::broadphase::estimateCellOccupancyCount(unitRange), 8u,
             "estimateCellOccupancyCount multiplies 3D span");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {2, 1}};
    expectEq(fuse::physics::broadphase::estimateCellOccupancyCount(planeRange), 6u,
             "estimateCellOccupancyCount multiplies 2D span");

    fuse::physics::broadphase::CellRange3 inverted = {{3, 3, 3}, {1, 1, 1}};
    expectEq(fuse::physics::broadphase::estimateCellOccupancyCount(inverted), 0u,
             "estimateCellOccupancyCount returns zero for empty range");

void testBroadphaseNormalizedParamsGuard() {
    expectTrue(
        fuse::physics::broadphase::candidatePairRejectReason(2u, 2u) ==
            fuse::physics::broadphase::CandidatePairRejectReason::SelfPair,
        "reject reason reports self-pair");
        fuse::physics::broadphase::candidatePairRejectReason(0u, 1u, 2u) ==
            fuse::physics::broadphase::CandidatePairRejectReason::None,
        "in-range pair has no reject reason");
        fuse::physics::broadphase::candidatePairRejectReason(0u, 2u, 2u) ==
            fuse::physics::broadphase::CandidatePairRejectReason::OutOfRangeBody,
        "out-of-range pair reports reject reason");

    const fuse::physics::broadphase::CandidatePair selfPair{3u, 3u};
        fuse::physics::broadphase::candidatePairRejectReason(selfPair, 4u) ==
        "CandidatePair overload reports self-pair");
}

void testCellRangeVolumeAndClampCellSize() {
    expectTrue(fuse::physics::broadphase::clampCellSize(0.f) > 0.f, "clampCellSize rejects non-positive size");

    fuse::physics::broadphase::CellRange3 emptyRange = {{1, 1, 1}, {0, 0, 0}};
    expectTrue(fuse::physics::broadphase::cellRangeIsEmpty(emptyRange), "inverted range is empty");
    expectEq(fuse::physics::broadphase::cellRangeVolume3(emptyRange), 0u, "empty range has zero volume");

    fuse::physics::broadphase::CellRange2 unitRange = {{0, 0}, {1, 1}};
    expectEq(fuse::physics::broadphase::cellRangeVolume2(unitRange), 4u, "2x2 cell range volume");
    expectTrue(!fuse::physics::broadphase::shouldSkipCellPairGeneration(2u),
               "two occupants may generate pairs");
    expectTrue(fuse::physics::broadphase::shouldSkipCellPairGeneration(1u),
               "single occupant skips pair generation");

void testPairBufferIsFullAndSetMaxCapacityTrim() {
    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    expectEq(buffer.activeCount, 2u, "buffer holds two pairs before trim");

    buffer.setMaxCapacity(1u);
    expectTrue(buffer.isFull(), "buffer reports full at max capacity");
    expectEq(buffer.activeCount, 1u, "setMaxCapacity trims excess pairs");
    expectEq(buffer.droppedCount, 1u, "setMaxCapacity records dropped pairs");
    expectTrue(!buffer.push(4u, 5u), "push rejects when buffer is full");
    expectEq(buffer.droppedCount, 2u, "full-buffer push increments dropped count");

void testPairBufferCompactAllInvalidEarlyOut() {
    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, 1u, 1u);
    buffer.writeSlot(1u, 2u, 2u);
    expectEq(buffer.compact(), 0u, "compact early-outs when all slots invalid");
    expectTrue(buffer.isEmpty(), "all-invalid compact leaves empty buffer");
    expectTrue(buffer.canSkipSoAIteration(), "all-invalid compact enables SoA skip");

void testPairBufferCompactAlreadyPacked() {
    buffer.preparePairSlots(4u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(buffer.compact(), 2u, "already-packed prefix compacts without moving data");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "packed compact preserves first pair");
    expectTrue(buffer.containsCanonicalPair(2u, 3u), "packed compact preserves second pair");

void testPairBufferSortCanonicalEarlyOut() {
    expectTrue(buffer.isSortedCanonical(), "canonical pairs start sorted");

    const fuse::u32 firstBodyA = buffer.bodyA[0u];
    const fuse::u32 firstBodyB = buffer.bodyB[0u];
    buffer.sortCanonical();
    expectEq(buffer.bodyA[0u], firstBodyA, "sortCanonical early-out preserves first bodyA");
    expectEq(buffer.bodyB[0u], firstBodyB, "sortCanonical early-out preserves first bodyB");

void testBroadphase2DCellSpanClampIntegration() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    params.bodyCount = bodies.count();

    const auto pairs = fuse::physics::broadphase::runBroadphase(bodies, shapes, params);
    expectTrue(!pairs.empty(), "normalized zero params still find overlapping pair");

void testPairBufferCompactionEarlyOuts() {
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(buffer.countValidSlots(), 2u, "countValidSlots counts prepared valid slots");
    expectTrue(buffer.canSkipCompaction(), "all-valid slots skip compaction work");
    expectEq(buffer.compact(), 2u, "compact early-out preserves active count");
    expectEq(buffer.activeCount, 2u, "compact early-out leaves pairs intact");

void testEmptyBroadphaseInputGuards() {

    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseInput(bodies, shapes),
               "empty bodies and shapes is empty broadphase input");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphasePairGeneration(bodies, shapes),
               "canSkipBroadphasePairGeneration on empty scene");

               "bodies without shapes is empty broadphase input");
    expectTrue(fuse::physics::broadphase::isSingletonBroadphaseInput(bodies, shapes),
               "single body without matching shape count is singleton input");
               "canSkipBroadphase when shapes are missing");

    bodies.clear();
               "shapes without bodies is empty broadphase input");
               "orphan shape is singleton broadphase input");

               "one body and one shape is singleton broadphase input");
               "singleton scene skips pair generation");

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
               "two bodies with one shape is singleton by shape count");

    expectTrue(!fuse::physics::broadphase::isSingletonBroadphaseInput(bodies, shapes),
               "two bodies and two shapes is not singleton");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "populated scene does not skip broadphase");

void testCandidatePairRejectsForReasonGuards() {
    expectTrue(fuse::physics::broadphase::candidatePairRejectsForReason(
                   1u, 1u, 0u, fuse::physics::broadphase::CandidatePairRejectReason::SelfPair),
               "candidatePairRejectsForReason matches self-pair");
                   0u, 2u, 2u, fuse::physics::broadphase::CandidatePairRejectReason::OutOfRangeBody),
               "candidatePairRejectsForReason matches out-of-range");
    expectTrue(!fuse::physics::broadphase::candidatePairRejectsForReason(
                   0u, 1u, 2u, fuse::physics::broadphase::CandidatePairRejectReason::SelfPair),
               "valid pair does not reject for SelfPair");

    const fuse::physics::broadphase::CandidatePair pair{0u, 1u};
                   pair, 2u, fuse::physics::broadphase::CandidatePairRejectReason::OutOfRangeBody),
               "candidatePair overload accepts in-range pair");

void testCellOccupancyBudgetGuards() {
    const fuse::physics::broadphase::CellRange3 smallRange = {{0, 0, 0}, {1, 1, 1}};
    expectEq(fuse::physics::broadphase::estimateCellOccupancyCount(smallRange), 8u,
             "small 3D range has eight cells");
    expectTrue(fuse::physics::broadphase::cellOccupancyWithinBudget(smallRange, 8u),
               "occupancy at budget limit is within budget");
    expectTrue(!fuse::physics::broadphase::exceedsCellOccupancyBudget(smallRange, 8u),
               "occupancy at budget limit does not exceed");
    expectTrue(fuse::physics::broadphase::exceedsCellOccupancyBudget(smallRange, 7u),
               "occupancy above budget is flagged");
    expectTrue(fuse::physics::broadphase::isUnboundedCellOccupancyBudget(0u),
               "zero maxCells is unbounded occupancy budget");
    expectTrue(fuse::physics::broadphase::cellOccupancyWithinBudget(smallRange, 0u),
               "zero budget means unlimited occupancy");
    expectEq(fuse::physics::broadphase::occupancyBudgetRemaining(smallRange, 8u), 0u,
             "occupancy at budget leaves zero headroom");
    expectEq(fuse::physics::broadphase::occupancyBudgetRemaining(smallRange, 12u), 4u,
             "occupancyBudgetRemaining reports spare slots");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    expectEq(fuse::physics::broadphase::estimateCellOccupancyCount(planeRange), 8u,
             "small 2D range has eight cells");
    expectTrue(fuse::physics::broadphase::exceedsCellOccupancyBudget(planeRange, 4u),
               "2D occupancy budget guard flags overflow");
    expectEq(fuse::physics::broadphase::occupancyBudgetRemaining(planeRange, 10u), 2u,
             "2D occupancyBudgetRemaining subtracts occupied cells");

    expectTrue(fuse::physics::broadphase::cellOccupancyWithinBudget(inverted, 1u),
               "empty range is within any positive budget");
    expectEq(fuse::physics::broadphase::occupancyBudgetRemaining(inverted, 4u), 4u,
             "empty range leaves full occupancy budget");

void testEstimatePairCountForUniqueBodies() {
    expectEq(fuse::physics::broadphase::estimatePairCountForUniqueBodies(0u), 0u,
             "zero bodies yields zero pairs");
    expectEq(fuse::physics::broadphase::estimatePairCountForUniqueBodies(1u), 0u,
             "single body yields zero pairs");
    expectEq(fuse::physics::broadphase::estimatePairCountForUniqueBodies(3u), 3u,
             "three unique bodies yield three pairs");
    expectEq(fuse::physics::broadphase::estimatePairCountForUniqueBodies(4u), 6u,
             "four unique bodies yield six pairs");

void testPairBufferCanAcceptPairsGuard() {
    expectTrue(buffer.canAcceptPairs(2u), "empty buffer accepts two pairs");
    expectTrue(!buffer.canAcceptPairs(3u), "empty buffer rejects three pairs");
    expectTrue(buffer.canAcceptPairs(0u), "zero additional pairs always accepted");

    buffer.push(0u, 1u);
    expectTrue(buffer.canAcceptPairs(1u), "partial buffer accepts one more pair");
    expectTrue(!buffer.canAcceptPairs(2u), "partial buffer rejects two more pairs");
    expectTrue(!buffer.hasDroppedPairs(), "accepted pushes do not set dropped count");

    buffer.push(2u, 3u);
    expectTrue(!buffer.canAcceptPairs(1u), "full buffer rejects another pair");
    expectTrue(!buffer.push(4u, 5u), "push on full buffer fails");
    expectTrue(buffer.hasDroppedPairs(), "rejected push increments dropped count");

void testPairBufferSlotValidityBounds() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);

    expectTrue(!buffer.slotIsValid(2u), "slotIsValid rejects slot at pairSlotCount boundary");
    expectTrue(!buffer.slotIsValid(99u), "slotIsValid rejects out-of-range slot");

    buffer.invalidateSlot(2u);
    expectTrue(buffer.slotIsValid(0u), "invalidateSlot ignores out-of-range slot");
    buffer.invalidateSlot(99u);
    expectTrue(buffer.slotIsValid(0u), "invalidateSlot ignores far out-of-range slot");

    buffer.invalidateSlot(0u);
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears in-range slot");
}

void testPairBufferSlotModeDedupeGuard() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, 1u, 2u);
    buffer.writeSlot(2u, 0u, 1u);

    expectEq(buffer.activeCount, 0u, "slot-mode buffer keeps activeCount zero before compact");
    expectEq(buffer.countValidSlots(), 2u, "slot-mode buffer tracks valid slots before compact");
    expectTrue(!buffer.canSkipDedupe(), "duplicate multi-slot buffer does not skip dedupe");
    expectTrue(!buffer.isSortedCanonical(), "unsorted slot-mode buffer reports not sorted");

    buffer.compact();
    expectEq(buffer.activeCount, 2u, "compact gathers slot-mode pairs before dedupe");
    buffer.sortCanonical();
    expectTrue(buffer.isSortedCanonical(), "sortCanonical orders slot-mode pairs");
}

void testPairBufferCompactAndClampZeroGuard() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(0u);
    expectTrue(buffer.canSkipCompactAndClamp(), "zero-prepared buffer skips compactAndClamp");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp on zero-prepared buffer is no-op");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.invalidateSlot(0u);
    buffer.invalidateSlot(1u);
    expectTrue(buffer.canSkipCompactAndClamp(), "all-invalid slot buffer skips compactAndClamp");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp on all-invalid slots is no-op");
}

void testCellOccupancyPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {1, 1, 1}};
    const auto withinBudget = fuse::physics::broadphase::preflight_cell_occupancy(unitRange, 8u);
    expectTrue(withinBudget.can_insert(), "preflight allows occupancy within budget");
    expectEq(withinBudget.cellCount, 8u, "preflight reports cell count");
    expectTrue(!withinBudget.exceedsBudget, "preflight does not flag in-budget range");

    const auto overBudget = fuse::physics::broadphase::preflight_cell_occupancy(unitRange, 4u);
    expectTrue(!overBudget.can_insert(), "preflight rejects occupancy above budget");
    expectTrue(overBudget.exceedsBudget, "preflight flags over-budget range");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const auto emptyPreflight = fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(!emptyPreflight.can_insert(), "preflight rejects empty range");
    expectTrue(emptyPreflight.emptyRange, "preflight marks empty range");

    expectEq(fuse::physics::broadphase::maxCellBudgetFromSpanPerAxis(4u, false), 64u,
             "3D span budget is span cubed");
    expectEq(fuse::physics::broadphase::maxCellBudgetFromSpanPerAxis(4u, true), 16u,
             "2D span budget is span squared");
    expectEq(fuse::physics::broadphase::maxCellBudgetFromSpanPerAxis(0u, false), 0u,
             "zero span means unlimited budget");
}

void testShouldSkipShapeCellInsertionGuards() {
    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insertion(inverted, 4u),
               "should_skip rejects empty 3D range");

    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {1, 1, 1}};
    expectTrue(!fuse::physics::broadphase::should_skip_shape_cell_insertion(unitRange, 4u),
               "should_skip allows clamped in-budget 3D range");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insertion(planeRange, 2u),
               "should_skip rejects over-budget 2D range");
}

void testBroadphaseRefinePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    const auto emptyScene = fuse::physics::broadphase::preflight_broadphase_refine(bodies, shapes, buffer);
    expectTrue(emptyScene.skipped, "refine preflight skips empty scene");
    expectTrue(emptyScene.emptyScene, "refine preflight marks empty scene");
    expectTrue(fuse::physics::broadphase::should_skip_broadphase_refine(bodies, shapes, buffer),
               "should_skip matches empty scene refine preflight");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    const auto emptyBuffer = fuse::physics::broadphase::preflight_broadphase_refine(bodies, shapes, buffer);
    expectTrue(emptyBuffer.skipped, "refine preflight skips empty buffer with populated scene");
    expectTrue(emptyBuffer.emptyBuffer, "refine preflight marks empty buffer");

    buffer.push(0u, 1u);
    const auto validPreflight = fuse::physics::broadphase::preflight_broadphase_refine(bodies, shapes, buffer);
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid scene and buffer");
    expectTrue(validPreflight.can_refine(), "refine preflight can refine valid input");
    expectTrue(!fuse::physics::broadphase::should_skip_broadphase_refine(bodies, shapes, buffer),
               "should_skip allows valid refine input");
}

void testBroadphaseCanSkipIntegration() {

    params.cellSize = 2.f;
    params.tableSize = 128;

    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
               "integration scene is skippable before population");
    expectTrue(buffer.isEmpty(), "skippable broadphase leaves empty pair buffer");

void testCellOccupancyRejectReasonAndPreflight() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
                 fuse::physics::broadphase::cellOccupancyRejectReason(validRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "valid range reports None reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellOccupancyRejectReasonName(
                               fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
                           "ExceedsBudget") == 0,
               "ExceedsBudget reject reason has stable label");

                 fuse::physics::broadphase::cellOccupancyRejectReason(inverted, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
             "inverted range reports EmptyRange reject reason");

    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectTrue(withinBudget.canIterate(), "preflight accepts range within budget");
    expectEq(withinBudget.occupancyCount, 8u, "preflight reports occupancy count");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectTrue(!overBudget.canIterate(), "preflight rejects range over budget");
    expectTrue(overBudget.exceedsBudget, "preflight marks exceedsBudget");

    const fuse::physics::broadphase::CellOccupancyPreflight planePreflight =
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 4u);
    expectTrue(!planePreflight.canIterate(), "2D preflight rejects over-budget range");
    expectEq(planePreflight.occupancyCount, 8u, "2D preflight reports occupancy count");

void testBroadphasePreflightGuards() {

    const fuse::physics::broadphase::BroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphase(bodies, shapes);
    expectTrue(emptyPreflight.emptyInput, "preflight marks empty scene");
    expectTrue(!emptyPreflight.singletonInput, "preflight does not mark empty scene singleton");
    expectTrue(!emptyPreflight.canRun(), "preflight cannot run on empty scene");

    const fuse::physics::broadphase::BroadphasePreflight singletonPreflight =
    expectTrue(!singletonPreflight.emptyInput, "preflight does not mark singleton scene empty");
    expectTrue(singletonPreflight.singletonInput, "preflight marks singleton scene");
    expectTrue(!singletonPreflight.canRun(), "preflight cannot run on singleton scene");

    const fuse::physics::broadphase::BroadphasePreflight populatedPreflight =
    expectTrue(!populatedPreflight.emptyInput, "preflight does not mark populated scene empty");
    expectTrue(!populatedPreflight.singletonInput, "preflight does not mark populated scene singleton");
    expectTrue(populatedPreflight.canRun(), "preflight can run on populated scene");

void testRefineBroadphasePreflightGuards() {

    const fuse::physics::broadphase::RefineBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightRefineBroadphase(bodies, shapes, buffer);
    expectTrue(emptyPreflight.emptyBuffer, "refine preflight marks empty buffer");
    expectTrue(emptyPreflight.noValidPairs, "refine preflight marks no valid pairs");
    expectTrue(emptyPreflight.emptyInput, "refine preflight marks empty input");
    expectTrue(!emptyPreflight.canRefine(), "refine preflight cannot refine empty scene");
    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase on empty scene");


    const fuse::physics::broadphase::RefineBroadphasePreflight validPreflight =
    expectTrue(validPreflight.canRefine(), "refine preflight accepts valid scene");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase false when refine is viable");

void testDedupeBroadphasePreflightGuards() {
    const fuse::physics::broadphase::DedupeBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "dedupe preflight marks empty buffer");
    expectTrue(!emptyPreflight.canDedupe(), "dedupe preflight skips empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase false on empty buffer");

    const fuse::physics::broadphase::DedupeBroadphasePreflight singlePreflight =
    expectTrue(singlePreflight.singlePair, "dedupe preflight marks single pair");
    expectTrue(!singlePreflight.canDedupe(), "dedupe preflight skips single pair");

    const fuse::physics::broadphase::DedupeBroadphasePreflight multiPreflight =
    expectTrue(multiPreflight.canDedupe(), "dedupe preflight accepts multiple pairs");
    expectTrue(fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase true for multiple pairs");

void testPairBufferPreflightGuards() {
    buffer.setMaxCapacity(1u);

    const fuse::physics::broadphase::PairBufferPushPreflight validPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u);
    expectTrue(validPush.canPush(), "push preflight accepts valid pair under capacity");

    const fuse::physics::broadphase::PairBufferPushPreflight invalidPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 2u);
    expectTrue(invalidPush.invalidPair, "push preflight marks self-pair invalid");
    expectTrue(!invalidPush.canPush(), "push preflight rejects self-pair");

    const fuse::physics::broadphase::PairBufferPushPreflight fullPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 3u);
    expectTrue(fullPush.atCapacity, "push preflight marks full buffer");
    expectTrue(!fullPush.canPush(), "push preflight rejects push on full buffer");

    fuse::physics::broadphase::PairBufferSoA slotBuffer;
    slotBuffer.preparePairSlots(2u);
    slotBuffer.writeSlot(0u, 0u, 1u);
    const fuse::physics::broadphase::PairBufferCompactionPreflight compactionPreflight =
        fuse::physics::broadphase::preflightPairBufferCompaction(slotBuffer);
    expectTrue(compactionPreflight.needsCompaction(),
               "compaction preflight requests work when invalid slots exist");

    slotBuffer.writeSlot(1u, 2u, 3u);
    const fuse::physics::broadphase::PairBufferCompactionPreflight skipCompaction =
    expectTrue(!skipCompaction.needsCompaction(),
               "compaction preflight skips when all slots are valid");

    fuse::physics::broadphase::PairBufferSoA clampBuffer;
    clampBuffer.push(2u, 3u);
    clampBuffer.push(0u, 1u);
    clampBuffer.setMaxCapacity(1u);
    const fuse::physics::broadphase::PairBufferClampPreflight clampPreflight =
        fuse::physics::broadphase::preflightPairBufferClamp(clampBuffer);
    expectTrue(clampPreflight.needsClamp(), "clamp preflight requests overflow truncation");

    fuse::physics::broadphase::PairBufferSoA withinBuffer;
    withinBuffer.push(0u, 1u);
    withinBuffer.setMaxCapacity(2u);
    const fuse::physics::broadphase::PairBufferClampPreflight withinPreflight =
        fuse::physics::broadphase::preflightPairBufferClamp(withinBuffer);
    expectTrue(!withinPreflight.needsClamp(), "clamp preflight skips when within capacity");

void testPairBufferInvalidateInvalidPairs() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(3u);
    buffer.bodyA[0u] = 0u;
    buffer.bodyB[0u] = 1u;
    buffer.validFlags[0u] = 1u;
    buffer.bodyA[1u] = 0u;
    buffer.bodyB[1u] = 0u;
    buffer.validFlags[1u] = 1u;
    buffer.bodyA[2u] = 0u;
    buffer.bodyB[2u] = 2u;
    buffer.validFlags[2u] = 1u;
    expectEq(buffer.invalidateInvalidPairs(2u), 2u, "invalidateInvalidPairs removes self and OOB slots");
    expectEq(buffer.compact(), 1u, "compact after invalidation keeps valid pair");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "valid pair survives invalidation sweep");
}

void testPairBufferWriteSlotBodyCountGuard() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u, 2u);
    buffer.writeSlot(1u, 0u, 2u, 2u);
    expectEq(buffer.compact(), 1u, "writeSlot rejects out-of-range pair when bodyCount provided");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "writeSlot keeps in-range pair");
}

void testCanSkipBroadphaseGuards() {
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(0u, 1u),
               "canSkipBroadphase when no bodies");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(1u, 0u),
               "canSkipBroadphase when no shapes");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphase(2u, 3u),
               "canSkipBroadphase false when both sets populated");

    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs;
    expectTrue(fuse::physics::broadphase::isEmptyCandidatePairList(emptyPairs),
               "isEmptyCandidatePairList on empty vector");
    expectTrue(fuse::physics::broadphase::canSkipPairListDedupe(0u),
               "canSkipPairListDedupe on empty list");
    expectTrue(fuse::physics::broadphase::canSkipPairListDedupe(1u),
               "canSkipPairListDedupe on single pair");
    expectTrue(!fuse::physics::broadphase::canSkipPairListDedupe(2u),
               "canSkipPairListDedupe false for multiple pairs");
    expectTrue(fuse::physics::broadphase::canSkipCellPairGeneration(0u),
               "canSkipCellPairGeneration on empty cell");
    expectTrue(fuse::physics::broadphase::canSkipCellPairGeneration(1u),
               "canSkipCellPairGeneration on single occupant");
    expectTrue(!fuse::physics::broadphase::canSkipCellPairGeneration(2u),
               "canSkipCellPairGeneration false for pair-capable cell");

void testCellOccupancyBudgetGuards() {
    const fuse::physics::broadphase::CellRange3 unitCube = {{0, 0, 0}, {1, 1, 1}};
    expectEq(fuse::physics::broadphase::estimateCellOccupancyCount(unitCube), 8u,
             "unit cube occupies eight cells");
    expectTrue(!fuse::physics::broadphase::exceedsCellOccupancyBudget(unitCube, 8u),
               "unit cube within budget of eight");
    expectTrue(fuse::physics::broadphase::exceedsCellOccupancyBudget(unitCube, 4u),
               "unit cube exceeds budget of four");
    expectTrue(!fuse::physics::broadphase::exceedsCellOccupancyBudget(unitCube, 0u),
               "zero budget is unlimited");

    const fuse::physics::broadphase::CellRange3 wideRange = {{0, 0, 0}, {9, 0, 0}};
    expectTrue(fuse::physics::broadphase::exceedsMaxCellSpanPerAxis(wideRange, 4u),
               "wide range exceeds per-axis span budget");
    expectTrue(!fuse::physics::broadphase::exceedsMaxCellSpanPerAxis(wideRange, 0u),
               "zero per-axis budget is unlimited");

    const fuse::physics::broadphase::CellRange3 shrunk =
        fuse::physics::broadphase::shrinkCellRangeToOccupancyBudget(unitCube, 4u);
    expectEq(fuse::physics::broadphase::estimateCellOccupancyCount(shrunk), 4u,
             "shrinkCellRangeToOccupancyBudget fits budget");
    expectTrue(!fuse::physics::broadphase::exceedsCellOccupancyBudget(shrunk, 4u),
               "shrunk range respects occupancy budget");

void testPruneInvalidCandidatePairs() {
    std::vector<fuse::physics::broadphase::CandidatePair> pairs = {
        {0u, 1u},
        {1u, 1u},
        {0u, 2u},
        {3u, 4u},
    };
    expectEq(fuse::physics::broadphase::countValidCandidatePairs(pairs, 3u), 2u,
             "countValidCandidatePairs filters self and OOB pairs");
    expectEq(fuse::physics::broadphase::pruneInvalidCandidatePairs(pairs, 3u), 2u,
             "pruneInvalidCandidatePairs removes invalid entries");
    expectEq(pairs.size(), 2u, "prune leaves valid pairs only");
    expectTrue(fuse::physics::broadphase::isValidCandidatePair(pairs[0], 3u),
               "first pruned pair is valid");
    expectTrue(fuse::physics::broadphase::isValidCandidatePair(pairs[1], 3u),
               "second pruned pair is valid");

void testPairBufferWouldRejectPush() {
    expectTrue(buffer.wouldRejectPush(1u, 1u), "wouldRejectPush on self-pair");
    expectTrue(!buffer.wouldRejectPush(0u, 1u), "wouldRejectPush accepts valid pair");

    buffer.setMaxCapacity(1u);
    buffer.push(0u, 1u);
    expectTrue(buffer.wouldRejectPush(2u, 3u), "wouldRejectPush when buffer is full");
    expectTrue(buffer.canSkipMaxCapacityClamp(), "at-capacity buffer skips post clamp");
    expectTrue(!buffer.canApplyMaxCapacityClamp(), "canSkipMaxCapacityClamp inverse of canApply");

void testBroadphaseCellOccupancyBudgetIntegration() {
void testPairBufferSlotValidityBounds() {
void testCellOccupancyPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 smallRange = {{0, 0, 0}, {1, 1, 1}};
    const auto smallPreflight = fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 8u);
    expectEq(smallPreflight.cellCount, 8u, "preflight counts cells in small 3D range");
    expectTrue(!smallPreflight.isEmptyRange, "preflight marks non-empty range");
    expectTrue(!smallPreflight.exceedsBudget, "preflight within budget does not exceed");
    expectTrue(smallPreflight.can_populate_cells(), "preflight allows cell population within budget");

    const auto overBudgetPreflight = fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 4u);
    expectTrue(overBudgetPreflight.exceedsBudget, "preflight flags budget overflow");
    expectTrue(!overBudgetPreflight.can_populate_cells(), "preflight blocks population when over budget");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const auto emptyPreflight = fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(emptyPreflight.isEmptyRange, "preflight marks inverted range empty");
    expectEq(emptyPreflight.cellCount, 0u, "preflight empty range has zero cells");
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_population(inverted, 4u),
               "should_skip_shape_cell_population on empty range");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const auto planePreflight = fuse::physics::broadphase::preflight_cell_occupancy(planeRange, 8u);
    expectEq(planePreflight.cellCount, 8u, "preflight counts cells in 2D range");

void testBroadphaseDedupePreflightGuards() {
    const auto emptyPreflight = fuse::physics::broadphase::preflight_dedupe_pairs(buffer);
    expectTrue(emptyPreflight.skipped, "dedupe preflight skips empty buffer");
    expectTrue(!emptyPreflight.can_dedupe(), "empty buffer cannot dedupe");
    expectEq(emptyPreflight.validPairCount, 0u, "empty buffer has zero valid pairs");

    const auto singlePreflight = fuse::physics::broadphase::preflight_dedupe_pairs(buffer);
    expectTrue(singlePreflight.skipped, "dedupe preflight skips single-pair buffer");
    expectEq(singlePreflight.validPairCount, 1u, "single-pair buffer counts one valid pair");

    buffer.push(1u, 0u);
    const auto duplicatePreflight = fuse::physics::broadphase::preflight_dedupe_pairs(buffer);
    expectTrue(!duplicatePreflight.skipped, "dedupe preflight runs on duplicate pairs");
    expectTrue(duplicatePreflight.can_dedupe(), "duplicate buffer can dedupe");
    expectEq(duplicatePreflight.validPairCount, 2u, "duplicate buffer counts both slots before dedupe");

void testBroadphaseRefinePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const auto emptyScenePreflight =
        fuse::physics::broadphase::preflight_refine_broadphase_pairs(bodies, shapes, buffer);
    expectTrue(emptyScenePreflight.skipped, "refine preflight skips empty scene");
    expectTrue(emptyScenePreflight.emptyInput, "refine preflight marks empty input");
    expectTrue(fuse::physics::broadphase::should_skip_refine_broadphase_pairs(bodies, shapes, buffer),
               "should_skip_refine on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    const auto emptyBufferPreflight =
    expectTrue(emptyBufferPreflight.skipped, "refine preflight skips empty buffer with valid input");
    expectTrue(emptyBufferPreflight.emptyBuffer, "refine preflight marks empty buffer");

    const auto validPreflight =
    expectTrue(!validPreflight.skipped, "refine preflight allows populated buffer");
    expectTrue(validPreflight.can_refine(), "valid buffer can refine");
    expectEq(validPreflight.validPairCount, 1u, "refine preflight counts valid pairs");
    expectTrue(!fuse::physics::broadphase::should_skip_refine_broadphase_pairs(bodies, shapes, buffer),
               "should_skip_refine false when buffer has pairs");

    buffer.writeSlot(0u, 0u, 1u);

    expectTrue(!buffer.slotIsValid(2u), "slotIsValid rejects slot at pairSlotCount boundary");
    expectTrue(!buffer.slotIsValid(99u), "slotIsValid rejects out-of-range slot");

    buffer.invalidateSlot(2u);
    expectTrue(buffer.slotIsValid(0u), "invalidateSlot ignores out-of-range slot");
    buffer.invalidateSlot(99u);
    expectTrue(buffer.slotIsValid(0u), "invalidateSlot ignores far out-of-range slot");

    buffer.invalidateSlot(0u);
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears in-range slot");

void testPairBufferSlotModeDedupeGuard() {
    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, 1u, 2u);
    buffer.writeSlot(2u, 3u, 4u);

    expectEq(buffer.activeCount, 0u, "slot-mode buffer keeps activeCount zero before compact");
    expectEq(buffer.countValidSlots(), 2u, "slot-mode buffer tracks valid slots before compact");
    expectTrue(!buffer.canSkipDedupe(), "multi-slot buffer does not skip dedupe");
    expectTrue(!buffer.canSkipCompactAndClamp(), "valid slot-mode buffer does not skip compactAndClamp");

    expectEq(buffer.compactAndClamp(), 2u, "compactAndClamp gathers slot-mode pairs");
    expectTrue(buffer.isSortedCanonical(), "compactAndClamp leaves slot-mode buffer sorted");
    expectTrue(buffer.containsCanonicalPair(1u, 2u), "compactAndClamp preserves first slot pair");

void testPairBufferWouldRejectAdditionalPairs() {
    buffer.setMaxCapacity(2u);
    expectTrue(!buffer.wouldRejectAdditionalPairs(2u), "empty buffer accepts two pairs");
    expectTrue(buffer.wouldRejectAdditionalPairs(3u), "empty buffer rejects three pairs");
    expectTrue(!buffer.wouldRejectAdditionalPairs(0u), "zero additional pairs never rejected");

    expectTrue(buffer.wouldRejectAdditionalPairs(2u), "partial buffer rejects two more pairs");
    expectTrue(!buffer.wouldRejectAdditionalPairs(1u), "partial buffer accepts one more pair");

void testCanSkipBroadphaseRefineGuard() {

    expectTrue(fuse::physics::broadphase::canSkipBroadphaseRefine(bodies, shapes, buffer),
               "empty scene and buffer skips refine");

               "empty pair buffer skips refine on populated scene");

    buffer.push(0u, 0u);
               "self-pair buffer still skips refine when no valid slots");

    buffer.clear();
    expectTrue(!fuse::physics::broadphase::canSkipBroadphaseRefine(bodies, shapes, buffer),
               "valid pair buffer does not skip refine");

void testMaxCellSpanAxisGuards() {
    const fuse::physics::broadphase::CellRange3 range = {{0, 0, 0}, {3, 1, 2}};
    expectEq(fuse::physics::broadphase::maxCellSpanAxis(range), 4u,
             "maxCellSpanAxis reports largest 3D span");
    expectTrue(!fuse::physics::broadphase::exceedsMaxCellSpanPerAxis(range, 4u),
               "span at limit does not exceed per-axis cap");
    expectTrue(fuse::physics::broadphase::exceedsMaxCellSpanPerAxis(range, 3u),
               "span above limit exceeds per-axis cap");
    expectTrue(fuse::physics::broadphase::hasUnlimitedCellOccupancyBudget(0u),
               "zero budget means unlimited occupancy");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {5, 1}};
    expectEq(fuse::physics::broadphase::maxCellSpanAxis(planeRange), 6u,
             "maxCellSpanAxis reports largest 2D span");
    expectTrue(fuse::physics::broadphase::exceedsMaxCellSpanPerAxis(planeRange, 4u),
               "2D per-axis span guard flags overflow");

    expectEq(fuse::physics::broadphase::maxCellSpanAxis(inverted), 0u,
             "empty range reports zero max span");
    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {1, 1, 1}};
    const auto withinBudget =
        fuse::physics::broadphase::preflightCellOccupancy(unitRange, 8u);
    expectTrue(!withinBudget.skipped, "non-empty range is not skipped");
    expectEq(withinBudget.cellCount, 8u, "preflight reports occupancy count");
    expectTrue(!withinBudget.exceedsBudget, "range at budget limit does not exceed");

    const auto overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(unitRange, 7u);
    expectTrue(overBudget.exceedsBudget, "preflight flags occupancy above budget");

    const auto emptyPreflight =
        fuse::physics::broadphase::preflightCellOccupancy(inverted, 4u);
    expectTrue(emptyPreflight.skipped, "empty range preflight is skipped");
    expectEq(emptyPreflight.cellCount, 0u, "empty range reports zero cells");

    const auto planePreflight =
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 8u);
    expectEq(planePreflight.cellCount, 8u, "2D preflight reports occupancy count");

void testPerShapeCellBudgetGuard() {
    expectEq(fuse::physics::broadphase::perShapeCellBudget(0u, false), 0u,
             "zero span means unlimited per-shape budget");
    expectEq(fuse::physics::broadphase::perShapeCellBudget(4u, true), 16u,
             "2D per-shape budget is span squared");
    expectEq(fuse::physics::broadphase::perShapeCellBudget(4u, false), 64u,
             "3D per-shape budget is span cubed");

void testPairSlotPreflightGuards() {
    buffer.setMaxCapacity(4u);

    const auto zeroSlots = fuse::physics::broadphase::preflightPairSlots(0u, buffer);
    expectTrue(zeroSlots.skipped, "zero slot preflight is skipped");

    const auto withinCapacity = fuse::physics::broadphase::preflightPairSlots(3u, buffer);
    expectTrue(!withinCapacity.skipped, "non-zero slot preflight is active");
    expectTrue(!withinCapacity.exceedsBufferCapacity, "slots within maxCapacity pass preflight");

    const auto exceedsCapacity = fuse::physics::broadphase::preflightPairSlots(8u, buffer);
    expectTrue(exceedsCapacity.exceedsBufferCapacity,
               "slot count above maxCapacity is flagged");

void testRefineBroadphasePreflightGuards() {

    const auto emptyScene =
        fuse::physics::broadphase::preflightRefineBroadphasePairs(buffer, bodies, shapes);
    expectTrue(emptyScene.skipped, "refine preflight skips empty scene");
    expectTrue(emptyScene.emptyBroadphaseInput, "refine preflight marks empty broadphase input");
    expectTrue(emptyScene.emptyBuffer, "refine preflight marks empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphasePairs(buffer, bodies, shapes),
               "canSkipRefineBroadphasePairs on empty scene");

    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    const auto populated =
    expectTrue(!populated.skipped, "refine preflight runs with valid pairs");
    expectTrue(!populated.emptyBroadphaseInput, "populated scene is not empty input");
    expectTrue(!populated.emptyBuffer, "non-empty buffer is not empty");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphasePairs(buffer, bodies, shapes),
               "canSkipRefineBroadphasePairs false with valid pairs");

void testPairBufferDedupeAndCompactGuards() {
    expectTrue(fuse::physics::broadphase::canSkipDedupeBuffer(buffer),
               "empty buffer skips dedupe");
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer skips compactAndClamp");

    expectTrue(buffer.canSkipDedupe(), "single-pair buffer skips dedupe");
    expectTrue(buffer.isSortedCanonical(), "single-pair buffer is canonically sorted");

    buffer.push(2u, 3u);
    expectTrue(!buffer.canSkipDedupe(), "multi-pair buffer does not skip dedupe");
    expectTrue(!fuse::physics::broadphase::canSkipDedupeBuffer(buffer),
               "canSkipDedupeBuffer false for multi-pair buffer");

    fuse::physics::broadphase::PairBufferSoA slotBuffer;
    slotBuffer.preparePairSlots(3u);
    slotBuffer.writeSlot(0u, 2u, 3u);
    slotBuffer.writeSlot(2u, 0u, 1u);
    expectTrue(!slotBuffer.canSkipCompaction(), "sparse slots do not skip compaction");
    expectTrue(!slotBuffer.isSortedCanonical(), "sparse out-of-order slots are not canonical");
    expectEq(slotBuffer.compactAndClamp(), 2u, "compactAndClamp gathers sparse valid slots");
    expectTrue(slotBuffer.containsCanonicalPair(0u, 1u), "compactAndClamp preserves first pair");
    expectTrue(slotBuffer.containsCanonicalPair(2u, 3u), "compactAndClamp preserves second pair");

    expectTrue(buffer.slotIsValid(0u), "in-range slot is valid");
    expectTrue(!buffer.slotIsValid(2u), "slot beyond pairSlotCount is invalid");
    expectTrue(!buffer.slotIsValid(99u), "slot beyond storage is invalid");

    expectTrue(buffer.slotIsValid(0u), "out-of-range invalidate is a no-op");
    expectTrue(!buffer.slotIsValid(0u), "in-range invalidate clears slot");
void testCellCapacityPreflightGuards() {
        fuse::physics::broadphase::preflight_cell_capacity(unitRange, 4u, 8u);
    expectTrue(withinBudget.can_insert(), "unit range within occupancy budget can insert");
    expectTrue(!withinBudget.exceedsOccupancyBudget, "unit range does not exceed budget");
    expectEq(withinBudget.occupancyCount, 8u, "preflight reports occupancy count");

        fuse::physics::broadphase::preflight_cell_capacity(unitRange, 4u, 4u);
    expectTrue(!overBudget.can_insert(), "unit range over occupancy budget cannot insert");
    expectTrue(overBudget.exceedsOccupancyBudget, "preflight flags occupancy overflow");

    fuse::physics::broadphase::CellRange3 wideRange = {{0, 0, 0}, {10, 0, 0}};
    const auto spanClamp =
        fuse::physics::broadphase::preflight_cell_capacity(wideRange, 4u, 0u);
    expectTrue(spanClamp.exceedsSpanClamp, "wide range exceeds span clamp preflight");
    expectTrue(spanClamp.can_insert(), "span clamp alone does not block insertion");

    const auto emptyRange = fuse::physics::broadphase::preflight_cell_capacity(inverted, 4u, 8u);
    expectTrue(emptyRange.skipped, "inverted range is skipped by preflight");
    expectTrue(emptyRange.emptyRange, "inverted range marked empty");

        fuse::physics::broadphase::preflight_cell_capacity(planeRange, 2u, 4u);
    expectTrue(planePreflight.exceedsOccupancyBudget, "2D preflight flags occupancy overflow");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(planeRange, 4u),
               "canSkipShapeCellInsertion matches occupancy overflow");

void testBroadphaseInputPreflightGuards() {

    const auto emptyPreflight = fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(emptyPreflight.skipped, "empty scene is skipped by input preflight");
    expectTrue(emptyPreflight.emptyBodies, "empty scene has no bodies");
    expectTrue(emptyPreflight.emptyShapes, "empty scene has no shapes");
    expectTrue(!emptyPreflight.can_run(), "empty scene cannot run broadphase");

    const auto missingShapes =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(missingShapes.skipped, "bodies without shapes are skipped");
    expectTrue(!missingShapes.emptyBodies, "bodies present when shapes missing");
    expectTrue(missingShapes.emptyShapes, "shapes still empty");

    const auto readyPreflight =
    expectTrue(!readyPreflight.skipped, "populated scene is not skipped");
    expectTrue(readyPreflight.can_run(), "populated scene can run broadphase");

void testPairBufferPreflightGuards() {
    const auto emptyPreflight = fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(emptyPreflight.empty, "empty buffer reports empty");
    expectTrue(!emptyPreflight.full, "empty buffer is not full");
    expectTrue(emptyPreflight.can_push(1u), "empty buffer can accept push preflight");

    const auto partialPreflight = fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(!partialPreflight.skipped, "non-empty buffer is not skipped");
    expectEq(partialPreflight.remaining, 1u, "partial buffer reports one remaining slot");
    expectTrue(partialPreflight.can_push(1u), "partial buffer can accept one more pair");
    expectTrue(!partialPreflight.can_push(2u), "partial buffer rejects two more pairs");

    const auto fullPreflight = fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(fullPreflight.full, "full buffer reports full");
    expectTrue(!fullPreflight.can_push(1u), "full buffer rejects another pair preflight");

    buffer.push(4u, 5u);
    const auto droppedPreflight = fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(droppedPreflight.hasDropped, "rejected push sets dropped preflight");


        fuse::physics::broadphase::preflight_broadphase_refine(buffer, bodies, shapes);
    expectTrue(emptyBufferPreflight.skipped, "empty buffer skips refine preflight");
    expectTrue(emptyBufferPreflight.emptyBuffer, "empty buffer flagged in refine preflight");
    expectTrue(!emptyBufferPreflight.can_refine(), "empty buffer cannot refine");

    const auto missingPairsPreflight =
    expectTrue(missingPairsPreflight.skipped, "buffer without pairs skips refine");
    expectTrue(!missingPairsPreflight.emptyInput, "scene input is present");

    expectTrue(!readyPreflight.skipped, "buffer with pairs can refine");
    expectTrue(readyPreflight.can_refine(), "ready refine preflight can refine");
    expectEq(readyPreflight.pairCount, 1u, "refine preflight reports pair count");
    expectTrue(buffer.canSkipRefine() == readyPreflight.emptyBuffer,
               "canSkipRefine matches refine preflight empty buffer flag");

    const auto emptyPreflight = fuse::physics::broadphase::preflight_broadphase_dedupe(buffer);
    expectTrue(emptyPreflight.skipped, "empty buffer skips dedupe preflight");
    expectTrue(!emptyPreflight.needs_dedupe(), "empty buffer does not need dedupe");

    const auto singlePreflight = fuse::physics::broadphase::preflight_broadphase_dedupe(buffer);
    expectTrue(!singlePreflight.skipped, "single-pair buffer is not skipped");
    expectTrue(singlePreflight.noOp, "single-pair dedupe is a no-op");
    expectTrue(!singlePreflight.needs_dedupe(), "single-pair buffer does not need dedupe");
    expectTrue(buffer.canSkipDedupe(), "canSkipDedupe matches dedupe preflight no-op");

    const auto multiPreflight = fuse::physics::broadphase::preflight_broadphase_dedupe(buffer);
    expectTrue(!multiPreflight.noOp, "multi-pair buffer may need dedupe");
    expectTrue(multiPreflight.needs_dedupe(), "multi-pair buffer needs dedupe preflight");

void testPairBufferInvalidSlotGuards() {
    expectTrue(!buffer.hasInvalidSlots(), "empty buffer has no invalid slots");

    buffer.writeSlot(2u, 2u, 3u);
    expectTrue(buffer.hasInvalidSlots(), "sparse prepared slots have invalid gaps");
    expectTrue(!buffer.canSkipCompaction(), "sparse slots cannot skip compaction");
    expectEq(buffer.compact(), 2u, "compact gathers valid sparse slots");
    expectTrue(!buffer.hasInvalidSlots(), "compacted buffer has no invalid slots");
    expectTrue(buffer.canSkipCompaction(), "compacted buffer can skip compaction");

void testEmptyCellBucketGuards() {
    expectTrue(fuse::physics::broadphase::isEmptyCellBucket(0u),
               "zero occupants is empty cell bucket");
    expectTrue(fuse::physics::broadphase::isEmptyCellBucket(1u),
               "single occupant is empty cell bucket");
    expectTrue(!fuse::physics::broadphase::isEmptyCellBucket(2u),
               "two occupants can emit pairs");
void testBroadphasePreflightGuards() {

    const auto emptyPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(emptyPreflight.skipped, "empty scene preflight is skipped");
    expectTrue(!emptyPreflight.can_dispatch(), "empty scene preflight cannot dispatch");
    expectEq(emptyPreflight.bodyCount, 0u, "empty scene preflight reports zero bodies");
    expectEq(emptyPreflight.shapeCount, 0u, "empty scene preflight reports zero shapes");
    expectTrue(fuse::physics::broadphase::should_skip_broadphase(bodies, shapes),
               "should_skip_broadphase on empty scene");

    const auto populatedPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(!populatedPreflight.skipped, "populated scene preflight is not skipped");
    expectTrue(populatedPreflight.can_dispatch(), "populated scene preflight can dispatch");
    expectEq(populatedPreflight.bodyCount, 1u, "populated preflight reports body count");
    expectEq(populatedPreflight.shapeCount, 1u, "populated preflight reports shape count");
    expectTrue(!fuse::physics::broadphase::should_skip_broadphase(bodies, shapes),
               "should_skip_broadphase on populated scene");

        fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 8u);
    expectTrue(!withinBudget.skipped, "small range preflight is not skipped");
    expectTrue(!withinBudget.emptyRange, "small range preflight is non-empty");
    expectTrue(withinBudget.can_iterate(), "small range preflight can iterate");
    expectEq(withinBudget.estimatedCells, 8u, "small range preflight estimates eight cells");

        fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 4u);
    expectTrue(overBudget.exceedsBudget, "over-budget preflight flags exceed");
    expectTrue(overBudget.skipped, "over-budget preflight is skipped");
    expectTrue(!overBudget.can_iterate(), "over-budget preflight cannot iterate");

        fuse::physics::broadphase::preflight_cell_occupancy(inverted, 8u);
    expectTrue(emptyPreflight.emptyRange, "inverted range preflight is empty");
    expectTrue(emptyPreflight.skipped, "inverted range preflight is skipped");
    expectTrue(!emptyPreflight.can_iterate(), "inverted range preflight cannot iterate");

        fuse::physics::broadphase::preflight_cell_occupancy(planeRange, 4u);
    expectTrue(planePreflight.exceedsBudget, "2D over-budget preflight flags exceed");
    expectTrue(!planePreflight.can_iterate(), "2D over-budget preflight cannot iterate");

    expectEq(fuse::physics::broadphase::estimateMaxCellOccupancyBudget(4u, true), 64u,
             "3D occupancy budget is span cubed");
    expectEq(fuse::physics::broadphase::estimateMaxCellOccupancyBudget(4u, false), 16u,
             "2D occupancy budget is span squared");
    expectEq(fuse::physics::broadphase::estimateMaxCellOccupancyBudget(0u, true), 0u,
             "zero span means unlimited occupancy budget");


    expectTrue(emptyPreflight.skipped, "refine preflight skips empty scene and buffer");
    expectTrue(!emptyPreflight.can_refine(), "empty refine preflight cannot refine");
               "should_skip_refine on empty buffer");


    const auto refinePreflight =
    expectTrue(!refinePreflight.skipped, "refine preflight does not skip valid pair");
    expectTrue(refinePreflight.can_refine(), "refine preflight can refine valid pair");
    expectEq(refinePreflight.pairCount, 1u, "refine preflight reports pair count");
    expectEq(refinePreflight.bodyCount, 2u, "refine preflight reports body count");
    expectEq(refinePreflight.shapeCount, 2u, "refine preflight reports shape count");
               "should_skip_refine on valid pair buffer");

void testPairBufferDedupePreflightGuards() {

    const auto emptyPreflight = fuse::physics::broadphase::preflight_pair_buffer_dedupe(buffer);
    expectTrue(fuse::physics::broadphase::should_skip_pair_buffer_dedupe(buffer),
               "should_skip_pair_buffer_dedupe on empty buffer");

    const auto singlePreflight = fuse::physics::broadphase::preflight_pair_buffer_dedupe(buffer);
    expectTrue(!singlePreflight.can_dedupe(), "single-pair buffer cannot dedupe");
               "should_skip_pair_buffer_dedupe on single-pair buffer");

    const auto multiPreflight = fuse::physics::broadphase::preflight_pair_buffer_dedupe(buffer);
    expectTrue(!multiPreflight.skipped, "dedupe preflight does not skip multi-pair buffer");
    expectTrue(multiPreflight.can_dedupe(), "multi-pair buffer can dedupe");
    expectEq(multiPreflight.activeCount, 2u, "dedupe preflight reports active count");
    expectTrue(!fuse::physics::broadphase::should_skip_pair_buffer_dedupe(buffer),
               "should_skip_pair_buffer_dedupe on multi-pair buffer");

void testPairBufferCanSkipRefineGuard() {
    expectTrue(buffer.canSkipRefine(), "empty buffer skips refine");

    expectTrue(!buffer.canSkipRefine(), "single valid pair does not skip refine");
    expectTrue(buffer.canSkipDedupe(), "single valid pair still skips dedupe");

    expectTrue(!buffer.hasValidPairs(), "prepared slots do not count as valid pairs yet");
    expectTrue(buffer.canSkipRefine(), "uncompacted prepared slots skip refine");
    expectEq(buffer.compact(), 1u, "compact activates prepared valid slot");
    expectTrue(!buffer.canSkipRefine(), "compacted valid pair does not skip refine");

    buffer.writeSlot(2u, 0u, 1u);

    expectTrue(!buffer.canSkipDedupe(), "multi-slot buffer with duplicates does not skip dedupe");
    expectTrue(!buffer.isSortedCanonical(), "unsorted slot-mode buffer reports not sorted");

    buffer.compact();
    expectEq(buffer.activeCount, 2u, "compact gathers slot-mode pairs");
    expectTrue(!buffer.canSkipDedupe(), "compacted duplicate pairs still need dedupe");

void testPairBufferCanSkipCompactAndClamp() {

    buffer.preparePairSlots(0u);
    expectTrue(buffer.canSkipCompactAndClamp(), "zero-prepared buffer skips compactAndClamp");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp on zero-prepared buffer is no-op");


    expectEq(buffer.compactAndClamp(), 1u, "compactAndClamp gathers and clamps slot-mode pairs");
    expectEq(buffer.activeCount, 1u, "compactAndClamp active count after clamp");
void testCellOccupancyRejectReasonAndPreflight() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::cellOccupancyRejectReason(validRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "valid range reports None reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellOccupancyRejectReasonName(
                               fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
                           "ExceedsBudget") == 0,
               "ExceedsBudget reject reason has stable label");

                 fuse::physics::broadphase::cellOccupancyRejectReason(inverted, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
             "inverted range reports EmptyRange reject reason");

    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectTrue(withinBudget.canIterate(), "preflight accepts range within budget");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectTrue(!overBudget.canIterate(), "preflight rejects range over budget");
    expectTrue(overBudget.exceedsBudget, "preflight marks exceedsBudget");

    const fuse::physics::broadphase::CellOccupancyPreflight planePreflight =
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 4u);
    expectTrue(!planePreflight.canIterate(), "2D preflight rejects over-budget range");
    expectEq(planePreflight.occupancyCount, 8u, "2D preflight reports occupancy count");


    const fuse::physics::broadphase::BroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphase(bodies, shapes);
    expectTrue(emptyPreflight.emptyInput, "preflight marks empty scene");
    expectTrue(!emptyPreflight.canRun(), "preflight cannot run on empty scene");

    const fuse::physics::broadphase::BroadphasePreflight populatedPreflight =
    expectTrue(!populatedPreflight.emptyInput, "preflight does not mark populated scene empty");
    expectTrue(populatedPreflight.canRun(), "preflight can run on populated scene");


    const fuse::physics::broadphase::RefineBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightRefineBroadphase(bodies, shapes, buffer);
    expectTrue(emptyPreflight.emptyBuffer, "refine preflight marks empty buffer");
    expectTrue(emptyPreflight.noValidPairs, "refine preflight marks no valid pairs");
    expectTrue(emptyPreflight.emptyInput, "refine preflight marks empty input");
    expectTrue(!emptyPreflight.canRefine(), "refine preflight cannot refine empty scene");
    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase on empty scene");


    const fuse::physics::broadphase::RefineBroadphasePreflight validPreflight =
    expectTrue(validPreflight.canRefine(), "refine preflight accepts valid scene");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase false when refine is viable");

void testDedupeBroadphasePreflightGuards() {
    const fuse::physics::broadphase::DedupeBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "dedupe preflight marks empty buffer");
    expectTrue(!emptyPreflight.canDedupe(), "dedupe preflight skips empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase false on empty buffer");

    const fuse::physics::broadphase::DedupeBroadphasePreflight singlePreflight =
    expectTrue(singlePreflight.singlePair, "dedupe preflight marks single pair");
    expectTrue(!singlePreflight.canDedupe(), "dedupe preflight skips single pair");

    const fuse::physics::broadphase::DedupeBroadphasePreflight multiPreflight =
    expectTrue(multiPreflight.canDedupe(), "dedupe preflight accepts multiple pairs");
    expectTrue(fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase true for multiple pairs");


    const fuse::physics::broadphase::PairBufferPushPreflight validPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u);
    expectTrue(validPush.canPush(), "push preflight accepts valid pair under capacity");

    const fuse::physics::broadphase::PairBufferPushPreflight invalidPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 2u);
    expectTrue(invalidPush.invalidPair, "push preflight marks self-pair invalid");
    expectTrue(!invalidPush.canPush(), "push preflight rejects self-pair");

    const fuse::physics::broadphase::PairBufferPushPreflight fullPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 3u);
    expectTrue(fullPush.atCapacity, "push preflight marks full buffer");
    expectTrue(!fullPush.canPush(), "push preflight rejects push on full buffer");

    slotBuffer.preparePairSlots(2u);
    slotBuffer.writeSlot(0u, 0u, 1u);
    const fuse::physics::broadphase::PairBufferCompactionPreflight compactionPreflight =
        fuse::physics::broadphase::preflightPairBufferCompaction(slotBuffer);
    expectTrue(compactionPreflight.needsCompaction(),
               "compaction preflight requests work when invalid slots exist");

    slotBuffer.writeSlot(1u, 2u, 3u);
    const fuse::physics::broadphase::PairBufferCompactionPreflight skipCompaction =
    expectTrue(!skipCompaction.needsCompaction(),
               "compaction preflight skips when all slots are valid");

    fuse::physics::broadphase::PairBufferSoA clampBuffer;
    clampBuffer.push(2u, 3u);
    clampBuffer.push(0u, 1u);
    clampBuffer.setMaxCapacity(1u);
    const fuse::physics::broadphase::PairBufferClampPreflight clampPreflight =
        fuse::physics::broadphase::preflightPairBufferClamp(clampBuffer);
    expectTrue(clampPreflight.needsClamp(), "clamp preflight requests overflow truncation");

    fuse::physics::broadphase::PairBufferSoA withinBuffer;
    withinBuffer.push(0u, 1u);
    withinBuffer.setMaxCapacity(2u);
    const fuse::physics::broadphase::PairBufferClampPreflight withinPreflight =
        fuse::physics::broadphase::preflightPairBufferClamp(withinBuffer);
    expectTrue(!withinPreflight.needsClamp(), "clamp preflight skips when within capacity");

void testBroadphaseBoxShapeCellRange() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({500.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {256.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 256;
    params.maxCellSpanPerAxis = 0u;
    params.maxCellOccupancyPerShape = 8u;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "occupancy budget prevents huge sphere from flooding pairs");
}

void testBroadphaseBoxShapeCellRange() {

    bodies.addBody({1.2f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Box, 0, {1.f, 1.f, 1.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Box, 1, {1.f, 1.f, 1.f});


    expectTrue(!pairs.empty(), "box shapes emit candidate pairs via AABB cell range");

void testBroadphaseSingletonEarlyOut() {



               "singleton scene is skippable for pair generation");
    expectTrue(buffer.isEmpty(), "singleton broadphase leaves empty pair buffer");

void testBroadphaseCellOccupancyBudgetIntegration() {

    bodies.addBody({500.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {256.f, 0.f, 0.f});

    params.cellSize = 1.f;
    params.tableSize = 256;
    params.maxCellSpanPerAxis = 0u;
    params.maxCellOccupancy = 8u;

    expectTrue(buffer.isEmpty(), "occupancy budget skips flooding shape insertion");

void testPairBufferReserveForUniqueBodies() {
    buffer.reserveForUniqueBodies(4u);
    expectTrue(buffer.bodyA.capacity() >= 6u, "reserveForUniqueBodies sizes for n*(n-1)/2 pairs");
    expectTrue(buffer.canSkipMaxCapacityClamp(), "fresh buffer skips max-capacity clamp");

void testPairBufferCanSkipMaxCapacityClamp() {
    expectTrue(buffer.canSkipMaxCapacityClamp(), "empty buffer skips max-capacity clamp");

    expectTrue(buffer.canSkipMaxCapacityClamp(), "under-capacity buffer skips post clamp");
    expectTrue(buffer.canSkipMaxCapacityClamp(), "at-capacity buffer skips post clamp");

    expectTrue(!overflowBuffer.canSkipMaxCapacityClamp(), "overflow buffer needs post clamp");

void testBroadphaseRejectReasonGuards() {

                 fuse::physics::broadphase::broadphaseRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::EmptyInput),
             "empty scene reports EmptyInput reject reason");
    expectTrue(fuse::physics::broadphase::broadphaseRejectsForReason(
                   bodies, shapes, fuse::physics::broadphase::BroadphaseRejectReason::EmptyInput),
               "broadphaseRejectsForReason matches empty scene");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseRejectReasonName(
                               fuse::physics::broadphase::BroadphaseRejectReason::SingletonInput),
                           "SingletonInput") == 0,
               "SingletonInput reject reason has stable label");

             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::SingletonInput),
             "singleton scene reports SingletonInput reject reason");

             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::None),
             "populated scene reports None reject reason");

    const fuse::physics::broadphase::BroadphasePreflight preflight =
    expectEq(static_cast<fuse::u32>(preflight.reason),
             "preflightBroadphase carries reject reason");
    expectTrue(preflight.canRun(), "populated preflight can run");

void testRefineBroadphaseRejectReasonGuards() {

                 fuse::physics::broadphase::refineBroadphaseRejectReason(bodies, shapes, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer refine reject reason");
    expectTrue(fuse::physics::broadphase::refineBroadphaseRejectsForReason(
                   bodies, shapes, buffer,
                   fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyBuffer),
               "refineBroadphaseRejectsForReason matches empty buffer");
    expectTrue(std::strcmp(fuse::physics::broadphase::refineBroadphaseRejectReasonName(
                               fuse::physics::broadphase::RefineBroadphaseRejectReason::NoValidPairs),
                           "NoValidPairs") == 0,
               "NoValidPairs refine reject reason has stable label");


             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::None),
             "valid refine scene reports None reject reason");

    const fuse::physics::broadphase::RefineBroadphasePreflight preflight =
    expectTrue(preflight.canRefine(), "refine preflight accepts valid scene with reason None");
             "refine preflight carries reject reason");

void testDedupeBroadphaseRejectReasonGuards() {

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::dedupeBroadphaseRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer dedupe reject reason");
    expectTrue(fuse::physics::broadphase::dedupeBroadphaseRejectsForReason(
                   buffer, fuse::physics::broadphase::DedupeBroadphaseRejectReason::EmptyBuffer),
               "dedupeBroadphaseRejectsForReason matches empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipDedupeBroadphase(buffer),
               "canSkipDedupeBroadphase on empty buffer");
               "shouldRunDedupeBroadphase false when canSkipDedupeBroadphase true");

             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::SinglePair),
             "single pair reports SinglePair dedupe reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::dedupeBroadphaseRejectReasonName(
                               fuse::physics::broadphase::DedupeBroadphaseRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair dedupe reject reason has stable label");

             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::None),
             "multiple pairs report None dedupe reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipDedupeBroadphase(buffer),
               "canSkipDedupeBroadphase false for multiple pairs");

    const fuse::physics::broadphase::DedupeBroadphasePreflight preflight =
    expectTrue(preflight.canDedupe(), "dedupe preflight accepts multiple pairs with reason None");

void testCellOccupancyRejectsForReasonGuards() {
    expectTrue(fuse::physics::broadphase::cellOccupancyRejectsForReason(
                   validRange, 8u, fuse::physics::broadphase::CellOccupancyRejectReason::None),
               "valid range rejects for None");

                   inverted, 4u, fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
               "inverted range rejects for EmptyRange");
                   validRange, 7u, fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
               "over-budget range rejects for ExceedsBudget");

                   planeRange, 4u, fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
               "2D over-budget range rejects for ExceedsBudget");

void testPairBufferDedupeAndSortPreflightGuards() {
    const fuse::physics::broadphase::PairBufferDedupePreflight emptyDedupe =
        fuse::physics::broadphase::preflightPairBufferDedupe(buffer);
    expectTrue(!emptyDedupe.canDedupe(), "empty buffer dedupe preflight cannot dedupe");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe on empty buffer");

    const fuse::physics::broadphase::PairBufferSortPreflight emptySort =
        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectTrue(!emptySort.needsSort(), "empty buffer sort preflight does not need sort");
    expectTrue(buffer.isSortedCanonical(), "empty buffer is canonically sorted");

    const fuse::physics::broadphase::PairBufferDedupePreflight singleDedupe =
    expectTrue(!singleDedupe.canDedupe(), "single-pair dedupe preflight cannot dedupe");
    expectTrue(singleDedupe.singlePair, "single-pair dedupe preflight marks single pair");

    const fuse::physics::broadphase::PairBufferSortPreflight singleSort =
    expectTrue(!singleSort.needsSort(), "single-pair sort preflight does not need sort");

    const fuse::physics::broadphase::PairBufferDedupePreflight multiDedupe =
    expectTrue(multiDedupe.canDedupe(), "multi-pair dedupe preflight can dedupe");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe false for multiple pairs");

    const fuse::physics::broadphase::PairBufferSortPreflight multiSort =
    expectTrue(multiSort.needsSort(), "multi-pair sort preflight needs sort");

    buffer.sortCanonical();
    expectTrue(buffer.isSortedCanonical(), "sortCanonical leaves canonical order via preflight gate");

void testPairBufferSoADedupePassGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(buffer.canSkipRefine(), "empty buffer skips refine");
    expectTrue(buffer.canSkipSortCanonical(), "empty buffer skips canonical sort");
    expectTrue(buffer.isDuplicateFree(), "empty buffer is duplicate-free");
    expectTrue(buffer.canSkipDedupePass(), "empty buffer skips dedupe pass");

    buffer.push(0u, 1u);
    expectTrue(!buffer.canSkipRefine(), "single valid pair may refine");
    expectTrue(buffer.canSkipSortCanonical(), "single pair skips canonical sort");
    expectTrue(buffer.isDuplicateFree(), "single pair is duplicate-free");
    expectTrue(buffer.canSkipDedupePass(), "single pair skips dedupe pass");

    buffer.push(2u, 3u);
    buffer.push(0u, 1u);
    expectTrue(!buffer.isDuplicateFree(), "duplicate pair is not duplicate-free");
    expectTrue(!buffer.canSkipDedupePass(), "duplicate pair needs dedupe pass");
    expectTrue(!buffer.canSkipSortCanonical(), "unsorted multi-pair buffer needs sort");

    buffer.sortCanonicalIfNeeded();
    expectTrue(buffer.isSortedCanonical(), "sortCanonicalIfNeeded leaves canonical order");
    expectTrue(buffer.canSkipSortCanonical(), "sorted buffer skips redundant canonical sort");
}

void testPairBufferPushRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
             "self-pair reports InvalidPair push reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPushRejectReasonName(
                               fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
                           "AtCapacity") == 0,
               "AtCapacity push reject reason has stable label");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity push reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferPushRejectsForReason(
                   buffer, 2u, 3u, fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
               "pairBufferPushRejectsForReason matches full buffer");

    const fuse::physics::broadphase::PairBufferPushPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 3u);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "push preflight carries reject reason");
    expectTrue(!preflight.canPush(), "push preflight rejects full buffer");
}

void testPairBufferCompactionRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compaction reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferCompactionRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
               "pairBufferCompactionRejectsForReason matches empty buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
             "all-valid slots report AllValid compaction reject reason");

    buffer.invalidateSlot(1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "invalid slot reports None compaction reject reason");

    const fuse::physics::broadphase::PairBufferCompactionPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferCompaction(buffer);
    expectTrue(preflight.needsCompaction(), "compaction preflight requests work when slots invalid");
}

void testPairBufferClampRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer clamp reject reason");

    buffer.push(0u, 1u);
    buffer.setMaxCapacity(2u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
             "within-capacity buffer reports WithinCapacity clamp reject reason");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(2u, 3u);
    overflowBuffer.push(4u, 5u);
    overflowBuffer.setMaxCapacity(2u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferClampRejectReason(overflowBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::None),
             "overflow buffer reports None clamp reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferClampRejectsForReason(
                   overflowBuffer, fuse::physics::broadphase::PairBufferClampRejectReason::None),
               "pairBufferClampRejectsForReason matches overflow buffer");

    const fuse::physics::broadphase::PairBufferClampPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferClamp(overflowBuffer);
    expectTrue(preflight.needsClamp(), "clamp preflight requests overflow truncation");
}

void testBroadphaseMergeRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "empty scene reports EmptyPlaneBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge false on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies merge reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseMergeRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
                           "EmptyDynamicBodies") == 0,
               "EmptyDynamicBodies merge reject reason has stable label");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "plane plus dynamic scene reports None merge reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge true when merge is viable");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge false when merge is viable");

    const fuse::physics::broadphase::BroadphaseMergePreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge preflight carries reject reason");
}

void testBroadphaseMergePreflightGuards() {

    const fuse::physics::broadphase::BroadphaseMergePreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectTrue(!emptyPreflight.canMerge(), "empty scene cannot merge plane-dynamic pairs");
    expectTrue(emptyPreflight.emptyPlaneBodies, "empty scene has no plane bodies");
    expectTrue(emptyPreflight.emptyDynamicBodies, "empty scene has no dynamic bodies");

    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight planeOnlyPreflight =
    expectTrue(!planeOnlyPreflight.canMerge(), "plane-only scene cannot merge");
    expectTrue(!planeOnlyPreflight.emptyPlaneBodies, "plane-only scene has plane bodies");
    expectTrue(planeOnlyPreflight.emptyDynamicBodies, "plane-only scene has no dynamic bodies");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight mergePreflight =
    expectTrue(mergePreflight.canMerge(), "plane plus dynamic scene can merge");
    expectTrue(!mergePreflight.emptyPlaneBodies, "merge scene has plane bodies");
    expectTrue(!mergePreflight.emptyDynamicBodies, "merge scene has dynamic bodies");

void testPairBufferPushRejectReasonGuards() {
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::None),
             "valid push reports None reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferPushRejectsForReason(
                   buffer, 0u, 1u, fuse::physics::broadphase::PairBufferPushRejectReason::None),
               "valid push rejects for None");

                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
             "self-pair reports InvalidPair reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPushRejectReasonName(
                               fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
                           "AtCapacity") == 0,
               "AtCapacity push reject reason has stable label");

                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity reject reason");

    const fuse::physics::broadphase::PairBufferPushPreflight preflight =
    expectTrue(!preflight.canPush(), "push preflight rejects at-capacity pair");
             "push preflight carries reject reason");

void testPairBufferCompactionRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compaction reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompaction(buffer),
               "canSkipPairBufferCompaction on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferCompaction(buffer),
               "shouldRunPairBufferCompaction false on empty buffer");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
             "all-valid slots report AllValid compaction reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferCompactionRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
               "all-valid slots reject for AllValid");

    buffer.invalidateSlot(1u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "invalid slots report None compaction reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompaction(buffer),
               "shouldRunPairBufferCompaction true when invalid slots exist");

void testPairBufferClampRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferClamp(buffer),
               "canSkipPairBufferClamp on empty buffer");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
             "within-capacity buffer reports WithinCapacity clamp reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferClamp(buffer),
               "shouldRunPairBufferClamp false when within capacity");

    buffer.push(4u, 5u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::None),
             "overflow buffer reports None clamp reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferClamp(buffer),
               "shouldRunPairBufferClamp true when overflow exists");

void testPairBufferDedupeRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer SoA dedupe reject reason");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
             "single pair reports SinglePair SoA dedupe reject reason");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::None),
             "multiple pairs report None SoA dedupe reject reason");
    const fuse::physics::broadphase::PairBufferDedupePreflight preflight =
    expectTrue(preflight.canDedupe(), "SoA dedupe preflight accepts multiple pairs with reason None");

void testCellOccupancyIterationSkipGuards() {
    expectTrue(fuse::physics::broadphase::shouldRunCellOccupancyIteration(validRange, 8u),
               "shouldRunCellOccupancyIteration true within budget");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 8u),
               "canSkipCellOccupancyIteration false within budget");

    const fuse::physics::broadphase::CellOccupancyPreflight preflight =
             "cell occupancy preflight carries reject reason");
    expectTrue(preflight.canIterate(), "cell occupancy preflight can iterate within budget");

    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "canSkipCellOccupancyIteration true over budget");
    expectTrue(!fuse::physics::broadphase::shouldRunCellOccupancyIteration(validRange, 7u),
               "shouldRunCellOccupancyIteration false over budget");

    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(planeRange, 4u),
               "2D canSkipCellOccupancyIteration true over budget");

void testRefineBroadphaseShouldRunGuards() {

    expectTrue(!fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase false on empty scene");
               "canSkipRefineBroadphase true when shouldRunRefineBroadphase false");


    expectTrue(fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase true for valid scene");
               "canSkipRefineBroadphase false when shouldRunRefineBroadphase true");

void testPairBufferSortRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer sort reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort false on empty buffer");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
             "single pair reports SinglePair sort reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSortRejectReasonName(
                               fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
               "SinglePair sort reject reason has stable label");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::None),
             "multiple pairs report None sort reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort true for multiple pairs");

    const fuse::physics::broadphase::PairBufferSortPreflight preflight =
             "sort preflight carries reject reason");

void testPairBufferCompactAndClampPreflightGuards() {
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compact-and-clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompactAndClamp(buffer),
               "canSkipPairBufferCompactAndClamp on empty buffer");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs via preflight on empty buffer");

                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWork),
             "synced within-capacity buffer reports NoWork compact-and-clamp reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferCompactAndClampRejectsForReason(
                   buffer,
               "synced buffer rejects for NoWork");
    expectEq(buffer.compactAndClamp(), 2u, "compactAndClamp no-op returns synced active count");

                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::None),
             "prepared slots with stale activeCount report None compact-and-clamp reject reason");

    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(buffer),
               "shouldRunPairBufferCompactAndClamp true when invalid slots exist");

    clampBuffer.preparePairSlots(2u);
    clampBuffer.writeSlot(0u, 0u, 1u);
    clampBuffer.writeSlot(1u, 2u, 3u);
    expectEq(clampBuffer.compactAndClamp(), 1u, "compactAndClamp gathers then clamps via preflight gate");
    expectTrue(clampBuffer.isSortedCanonical(), "compactAndClamp leaves canonical order");

void testShouldRunBroadphaseGuards() {

    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase false on empty scene");
               "canSkipBroadphase true when shouldRunBroadphase false");

    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");
               "canSkipBroadphase false when shouldRunBroadphase true");

void testMergePairsIntoBufferPreflightGuards() {
    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs;

                 fuse::physics::broadphase::mergePairsIntoBufferRejectReason(emptyPairs, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::EmptyPairs),
             "empty pair list reports EmptyPairs merge-into-buffer reject reason");
    expectTrue(fuse::physics::broadphase::canSkipMergePairsIntoBuffer(emptyPairs, buffer),
               "canSkipMergePairsIntoBuffer on empty pair list");
    expectTrue(std::strcmp(fuse::physics::broadphase::mergePairsIntoBufferRejectReasonName(
                               fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
                           "BufferFull") == 0,
               "BufferFull merge-into-buffer reject reason has stable label");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}, {2u, 3u}};
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight validPreflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, buffer);
    expectTrue(validPreflight.canMerge(), "merge-into-buffer preflight accepts non-empty list into empty buffer");
    expectTrue(fuse::physics::broadphase::shouldRunMergePairsIntoBuffer(pairs, buffer),
               "shouldRunMergePairsIntoBuffer true for valid merge");

                 fuse::physics::broadphase::mergePairsIntoBufferRejectReason(pairs, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
             "full buffer reports BufferFull merge-into-buffer reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunMergePairsIntoBuffer(pairs, buffer),
               "shouldRunMergePairsIntoBuffer false when buffer is full");

void testPairBufferWriteSlotRejectReasonGuards() {

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
             "valid write-slot reports None reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "shouldRunPairBufferWriteSlot true for valid slot");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 2u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferWriteSlotRejectsForReason(
                   buffer, 2u, 0u, 1u,
                   fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),
               "out-of-range slot rejects for OutOfRangeSlot");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferWriteSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
                           "InvalidPair") == 0,
               "InvalidPair write-slot reject reason has stable label");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
             "self-pair write-slot reports InvalidPair reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u),
               "canSkipPairBufferWriteSlot true for invalid pair");

    expectTrue(buffer.slotIsValid(0u), "writeSlot accepts valid pair via preflight gate");
    buffer.writeSlot(1u, 1u, 1u);
    expectTrue(!buffer.slotIsValid(1u), "writeSlot rejects self-pair via preflight gate");

void testPairBufferInvalidateSlotRejectReasonGuards() {

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None),
             "in-range slot reports None invalidate reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateSlotRejectsForReason(
                   buffer, 0u, fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None),
               "in-range slot rejects for None");

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 4u)),
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot invalidate reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReasonName(
                           "OutOfRangeSlot") == 0,
               "OutOfRangeSlot invalidate reject reason has stable label");

    const fuse::physics::broadphase::PairBufferInvalidateSlotPreflight validPreflight =
        fuse::physics::broadphase::preflightPairBufferInvalidateSlot(buffer, 1u);
    expectTrue(validPreflight.canInvalidate(), "invalidate-slot preflight accepts in-range slot");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 1u),
               "shouldRunPairBufferInvalidateSlot true for in-range slot");

    expectTrue(!buffer.slotIsValid(1u), "invalidateSlot clears valid flag via preflight gate");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 8u),
               "canSkipPairBufferInvalidateSlot true for out-of-range slot");

void testCellPairGenRejectReasonGuards() {
    const std::vector<fuse::u32> emptyOccupants;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::EmptyCell),
             "zero occupants report EmptyCell pair-gen reject reason");
    expectTrue(fuse::physics::broadphase::cellPairGenRejectsForReason(
                   0u, fuse::physics::broadphase::CellPairGenRejectReason::EmptyCell),
               "zero occupants reject for EmptyCell");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::SingletonOccupant),
             "singleton occupants report SingletonOccupant pair-gen reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellPairGenRejectReasonName(
                               fuse::physics::broadphase::CellPairGenRejectReason::SingletonOccupant),
                           "SingletonOccupant") == 0,
               "SingletonOccupant pair-gen reject reason has stable label");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::None),
             "three unique occupants report None pair-gen reject reason");
    expectEq(fuse::physics::broadphase::estimateCellPairCount(3u), 3u,
             "estimateCellPairCount returns n*(n-1)/2");

    const fuse::physics::broadphase::CellPairGenPreflight emptyPreflight =
        fuse::physics::broadphase::preflightCellPairGeneration(emptyOccupants);
    expectTrue(!emptyPreflight.canGenerate(), "empty cell pair-gen preflight cannot generate");
    expectTrue(fuse::physics::broadphase::canSkipCellPairGeneration(emptyOccupants),
               "canSkipCellPairGeneration on empty cell");

    const std::vector<fuse::u32> duplicateOccupants = {0u, 0u};
    const fuse::physics::broadphase::CellPairGenPreflight duplicatePreflight =
        fuse::physics::broadphase::preflightCellPairGeneration(duplicateOccupants);
    expectTrue(duplicatePreflight.singletonOccupant,
               "duplicate single-body occupants mark singletonOccupant");
    expectEq(duplicatePreflight.pairCount, 0u, "duplicate single-body occupants emit zero pairs");

    const std::vector<fuse::u32> multiOccupants = {0u, 1u, 2u};
    const fuse::physics::broadphase::CellPairGenPreflight multiPreflight =
        fuse::physics::broadphase::preflightCellPairGeneration(multiOccupants);
    expectTrue(multiPreflight.canGenerate(), "multi-body cell pair-gen preflight can generate");
    expectEq(multiPreflight.pairCount, 3u, "three-body cell emits three canonical pairs");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGeneration(multiOccupants),
               "shouldRunCellPairGeneration true for multi-body cell");
    expectEq(fuse::physics::broadphase::countUniqueCellOccupants(multiOccupants), 3u,
             "countUniqueCellOccupants dedupes occupant list");

void testShapeCellInsertRejectReasonGuards() {

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 99u, {1.f, 0.f, 0.f});


    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::shapeCellInsertRejectReason(
                 0u, bodies, shapes, params, false)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::OutOfRangeBody),
             "orphan shape reports OutOfRangeBody insert reject reason");
    expectTrue(fuse::physics::broadphase::shapeCellInsertRejectsForReason(
                   0u, bodies, shapes, params, false,
                   fuse::physics::broadphase::ShapeCellInsertRejectReason::OutOfRangeBody),
               "orphan shape rejects for OutOfRangeBody");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsert(0u, bodies, shapes, params, false),
               "canSkipShapeCellInsert on orphan shape");

    shapes.clear();

             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::OccupancySkipped),
             "oversized shape reports OccupancySkipped insert reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::shapeCellInsertRejectReasonName(
                               fuse::physics::broadphase::ShapeCellInsertRejectReason::OccupancySkipped),
                           "OccupancySkipped") == 0,
               "OccupancySkipped insert reject reason has stable label");

    const fuse::physics::broadphase::ShapeCellInsertPreflight skippedPreflight =
        fuse::physics::broadphase::preflightShapeCellInsert(0u, bodies, shapes, params, false);
    expectTrue(!skippedPreflight.canInsert(), "oversized shape insert preflight cannot insert");
    expectTrue(skippedPreflight.occupancySkipped, "oversized shape insert preflight marks occupancySkipped");

    params.maxCellOccupancy = 0u;

    const fuse::physics::broadphase::ShapeCellInsertPreflight validPreflight =
        fuse::physics::broadphase::preflightShapeCellInsert(1u, bodies, shapes, params, false);
    expectTrue(validPreflight.canInsert(), "normal shape insert preflight can insert");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsert(1u, bodies, shapes, params, false),
               "shouldRunShapeCellInsert true for normal shape");
    expectTrue(validPreflight.occupancyCount > 0u, "normal shape insert preflight reports occupancy count");

void testBroadphaseCellPairGenRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellPairGenRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairGenRejectReason::EmptyCells),
             "zero cell slots report EmptyCells broadphase pair-gen reject reason");
    expectTrue(fuse::physics::broadphase::broadphaseCellPairGenRejectsForReason(
                   0u, fuse::physics::broadphase::BroadphaseCellPairGenRejectReason::EmptyCells),
               "zero cell slots reject for EmptyCells");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseCellPairGenRejectReasonName(
                               fuse::physics::broadphase::BroadphaseCellPairGenRejectReason::EmptyCells),
                           "EmptyCells") == 0,
               "EmptyCells broadphase pair-gen reject reason has stable label");

    const fuse::physics::broadphase::BroadphaseCellPairGenPreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairGen(0u);
    expectTrue(!emptyPreflight.canGenerate(), "empty cell slots broadphase pair-gen preflight cannot generate");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseCellPairGen(0u),
               "canSkipBroadphaseCellPairGen on zero cell slots");

    const fuse::physics::broadphase::BroadphaseCellPairGenPreflight validPreflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairGen(4u);
    expectTrue(validPreflight.canGenerate(), "non-zero cell slots broadphase pair-gen preflight can generate");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseCellPairGen(4u),
               "shouldRunBroadphaseCellPairGen true for non-zero cell slots");

void testPairBufferToVectorRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferToVectorRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferToVectorRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer toVector reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferToVector(buffer),
               "canSkipPairBufferToVector on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferToVector(buffer),
               "shouldRunPairBufferToVector false on empty buffer");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferToVectorRejectReasonName(
                               fuse::physics::broadphase::PairBufferToVectorRejectReason::EmptyBuffer),
                           "EmptyBuffer") == 0,
               "EmptyBuffer toVector reject reason has stable label");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferToVectorRejectReason::None),
             "non-empty buffer reports None toVector reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferToVector(buffer),
               "shouldRunPairBufferToVector true for non-empty buffer");
    expectEq(buffer.toVector().size(), 1u, "toVector exports pair after preflight gate");

void testCellSpanRejectReasonAndPreflight() {
    const fuse::physics::broadphase::CellRange3 withinRange = {{0, 0, 0}, {3, 3, 3}};
    expectTrue(fuse::physics::broadphase::cellSpanWithinPerAxisLimit(withinRange, 4u),
               "4-cell span is within limit of four");
    expectTrue(!fuse::physics::broadphase::exceedsCellSpanPerAxis(withinRange, 4u),
               "4-cell span does not exceed limit of four");
                 fuse::physics::broadphase::cellSpanRejectReason(withinRange, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::None),
             "within-limit range reports None span reject reason");

    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(withinRange, 3u),
               "4-cell span exceeds limit of three");
    expectTrue(fuse::physics::broadphase::cellSpanRejectsForReason(
                   withinRange, 3u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpan),
               "over-span range rejects for ExceedsSpan");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellSpanRejectReasonName(
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpan),
                           "ExceedsSpan") == 0,
               "ExceedsSpan reject reason has stable label");

    const fuse::physics::broadphase::CellSpanPreflight spanPreflight =
        fuse::physics::broadphase::preflightCellSpan(withinRange, 3u);
    expectTrue(!spanPreflight.withinLimit(), "preflight marks over-span range");
    expectTrue(spanPreflight.exceedsSpan, "preflight marks exceedsSpan");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanClamp(withinRange, 3u),
               "shouldRunCellSpanClamp true when span exceeds budget");
    expectTrue(!fuse::physics::broadphase::canSkipCellSpanClamp(withinRange, 3u),
               "canSkipCellSpanClamp false when span exceeds budget");

                 fuse::physics::broadphase::cellSpanRejectReason(inverted, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
             "inverted range reports EmptyRange span reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(inverted, 4u),
               "canSkipCellSpanClamp true for empty range");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {5, 1}};
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(planeRange, 4u),
               "2D span guard flags over-span range");
    const fuse::physics::broadphase::CellSpanPreflight planePreflight =
        fuse::physics::broadphase::preflightCellSpan2D(planeRange, 4u);
    expectTrue(planePreflight.exceedsSpan, "2D preflight marks exceedsSpan");


             "valid slot reports None invalidate reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot true for valid slot");
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u),
               "wouldSkipPairBufferInvalidateSlot false for valid slot");

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 2u)),
                   buffer, 2u,
                               fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
                           "AlreadyInvalid") == 0,
               "AlreadyInvalid invalidate reject reason has stable label");

    fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason skipReason =
        fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 2u, &skipReason),
               "wouldSkipPairBufferInvalidateSlot true for out-of-range slot");
    expectEq(static_cast<fuse::u32>(skipReason),
             "wouldSkipPairBufferInvalidateSlot reports OutOfRangeSlot");

    buffer.invalidateSlot(0u);
             "already-invalid slot reports AlreadyInvalid invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "canSkipPairBufferInvalidateSlot true for already-invalid slot");

    buffer.invalidateSlot(2u);
    expectTrue(buffer.slotIsValid(1u), "invalidateSlot ignores out-of-range slot via preflight gate");
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears in-range slot via preflight gate");

void testPairBufferWouldSkipWriteSlotGuards() {

    fuse::physics::broadphase::PairBufferWriteSlotRejectReason reason =
        fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u, &reason),
               "wouldSkipPairBufferWriteSlot false for valid write");
    expectEq(static_cast<fuse::u32>(reason),
             "wouldSkipPairBufferWriteSlot reports None for valid write");

    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u, &reason),
               "wouldSkipPairBufferWriteSlot true for self-pair");
             "wouldSkipPairBufferWriteSlot reports InvalidPair for self-pair");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u) ==
                   fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "wouldSkipPairBufferWriteSlot agrees with canSkipPairBufferWriteSlot for valid write");

void testCellCapacityWouldSkipGuards() {
    fuse::physics::broadphase::CellOccupancyRejectReason occupancyReason =
        fuse::physics::broadphase::CellOccupancyRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 8u, &occupancyReason),
               "wouldSkipCellOccupancyIteration false within budget");
    expectEq(static_cast<fuse::u32>(occupancyReason),
             "wouldSkipCellOccupancyIteration reports None within budget");

    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u, &occupancyReason),
               "wouldSkipCellOccupancyIteration true over budget");
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "wouldSkipCellOccupancyIteration reports ExceedsBudget over budget");
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u) ==
                   fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "wouldSkipCellOccupancyIteration agrees with canSkipCellOccupancyIteration over budget");

    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(planeRange, 4u),
               "2D wouldSkipCellOccupancyIteration true over budget");

    const fuse::physics::broadphase::CellRange3 overSpanRange = {{0, 0, 0}, {3, 3, 3}};
    fuse::physics::broadphase::CellSpanRejectReason spanReason =
        fuse::physics::broadphase::CellSpanRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(overSpanRange, 3u, &spanReason),
               "wouldSkipCellSpanClamp true when span exceeds budget");
    expectEq(static_cast<fuse::u32>(spanReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpan),
             "wouldSkipCellSpanClamp reports ExceedsSpan");
    expectTrue(!fuse::physics::broadphase::wouldSkipCellSpanClamp(validRange, 0u),
               "wouldSkipCellSpanClamp false when span budget is unlimited");
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(overSpanRange, 3u) ==
                   fuse::physics::broadphase::shouldRunCellSpanClamp(overSpanRange, 3u),
               "wouldSkipCellSpanClamp agrees with shouldRunCellSpanClamp when span exceeds budget");

void testRefineDedupeMergeWouldSkipGuards() {

    fuse::physics::broadphase::RefineBroadphaseRejectReason refineReason =
        fuse::physics::broadphase::RefineBroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer, &refineReason),
               "wouldSkipRefineBroadphase true on empty scene");
    expectEq(static_cast<fuse::u32>(refineReason),
             "wouldSkipRefineBroadphase reports EmptyBuffer on empty scene");
    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer) ==
                   fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "wouldSkipRefineBroadphase agrees with canSkipRefineBroadphase on empty scene");

    expectTrue(!fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer),
               "wouldSkipRefineBroadphase false for valid refine scene");

    fuse::physics::broadphase::DedupeBroadphaseRejectReason dedupeReason =
        fuse::physics::broadphase::DedupeBroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer, &dedupeReason),
               "wouldSkipDedupeBroadphase true on single pair");
    expectEq(static_cast<fuse::u32>(dedupeReason),
             "wouldSkipDedupeBroadphase reports SinglePair");

    expectTrue(!fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer),
               "wouldSkipDedupeBroadphase false when dedupe may proceed");
    expectTrue(fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer) ==
                   fuse::physics::broadphase::canSkipDedupeBroadphase(buffer),
               "wouldSkipDedupeBroadphase agrees with canSkipDedupeBroadphase for duplicate pair");

    fuse::physics::broadphase::BroadphaseMergeRejectReason mergeReason =
        fuse::physics::broadphase::BroadphaseMergeRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphaseMerge(bodies, shapes, &mergeReason),
               "wouldSkipBroadphaseMerge true without plane bodies");
    expectEq(static_cast<fuse::u32>(mergeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "wouldSkipBroadphaseMerge reports EmptyPlaneBodies");

    expectTrue(!fuse::physics::broadphase::wouldSkipBroadphaseMerge(bodies, shapes),
               "wouldSkipBroadphaseMerge false when plane and dynamic bodies exist");

    fuse::physics::broadphase::PairBufferSoA mergeBuffer;
    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}};
    fuse::physics::broadphase::MergePairsIntoBufferRejectReason mergeIntoReason =
        fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, mergeBuffer, &mergeIntoReason),
               "wouldSkipMergePairsIntoBuffer false for valid merge");
    expectEq(static_cast<fuse::u32>(mergeIntoReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None),
             "wouldSkipMergePairsIntoBuffer reports None for valid merge");

    mergeBuffer.setMaxCapacity(1u);
    mergeBuffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, mergeBuffer, &mergeIntoReason),
               "wouldSkipMergePairsIntoBuffer true when buffer is full");
             "wouldSkipMergePairsIntoBuffer reports BufferFull");

void testRefineDedupeMergeWithPreflightGuards() {

    expectTrue(!fuse::physics::broadphase::refineBroadphasePairsParallelWithPreflight(bodies, shapes, buffer),
               "refine with preflight skips empty scene");
    expectTrue(buffer.isEmpty(), "refine with preflight leaves empty buffer unchanged");

    bodies.addBody({20.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 2, {1.f, 0.f, 0.f});

    buffer.push(0u, 2u);
    expectTrue(fuse::physics::broadphase::refineBroadphasePairsParallelWithPreflight(bodies, shapes, buffer),
               "refine with preflight runs on valid scene");
    expectEq(buffer.activeCount, 1u, "refine with preflight removes separated pair");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "refine with preflight keeps overlapping pair");

    fuse::physics::broadphase::PairBufferSoA dedupeBuffer;
    dedupeBuffer.push(0u, 1u);
    expectTrue(!fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer),
               "dedupe with preflight returns false on single pair");
    expectEq(dedupeBuffer.activeCount, 1u, "dedupe with preflight is no-op on single pair");

    dedupeBuffer.push(2u, 3u);
    expectTrue(fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer),
               "dedupe with preflight returns true for multiple pairs");
    expectEq(dedupeBuffer.activeCount, 2u, "dedupe with preflight removes duplicate pairs");

    const std::vector<fuse::physics::broadphase::CandidatePair> mergePairs = {{0u, 1u}, {2u, 3u}};
    fuse::physics::broadphase::mergePairsIntoBufferWithPreflight(mergePairs, mergeBuffer);
    expectEq(mergeBuffer.activeCount, 2u, "merge with preflight pushes valid pairs");

    mergeBuffer.setMaxCapacity(2u);
    expectTrue(mergeBuffer.isFull(), "merge buffer at capacity after setMaxCapacity");
    expectTrue(fuse::physics::broadphase::mergePairsIntoBufferRejectsForReason(
                   mergePairs, mergeBuffer,
               "full buffer rejects for BufferFull after merge");
    expectTrue(!fuse::physics::broadphase::shouldRunMergePairsIntoBuffer(mergePairs, mergeBuffer),

void testPairBufferShouldRunDedupeGuards() {
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe false on empty buffer");
               "canSkipPairBufferDedupe true when shouldRunPairBufferDedupe false");

               "shouldRunPairBufferDedupe false on single pair");

    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe true for multiple pairs");
               "canSkipPairBufferDedupe false when shouldRunPairBufferDedupe true");

void testBroadphaseMergeRejectReasonGuards() {

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::mergeBroadphaseRejectReason(bodies, shapes)),
             "empty scene reports EmptyPlaneBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::mergeBroadphaseRejectsForReason(
                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
               "mergeBroadphaseRejectsForReason matches empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge false on empty scene");

             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies merge reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::mergeBroadphaseRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
                           "EmptyDynamicBodies") == 0,
               "EmptyDynamicBodies merge reject reason has stable label");

             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "plane plus dynamic scene reports None merge reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge true for mergeable scene");

    const fuse::physics::broadphase::BroadphaseMergePreflight preflight =
             "merge preflight carries reject reason");
    expectTrue(buffer.canSkipSort(), "single-pair buffer skips canonical sort");
    expectTrue(!buffer.canSkipRefine(), "single-pair buffer still runs refine");
    expectTrue(buffer.canSkipCompact(), "dense single-pair buffer skips compact");

void testCandidateRejectReasonGuards() {
    expectTrue(
        fuse::physics::broadphase::candidatePairRejectReason(1u, 1u) ==
            fuse::physics::broadphase::CandidateRejectReason::SelfPair,
        "reject reason flags self pair");
        fuse::physics::broadphase::candidatePairRejectReason(0u, 2u, 2u) ==
            fuse::physics::broadphase::CandidateRejectReason::OutOfRangeBody,
        "reject reason flags out-of-range body");
        fuse::physics::broadphase::candidatePairRejectReason(0u, 1u, 2u) ==
            fuse::physics::broadphase::CandidateRejectReason::None,
        "in-range pair has no reject reason");


        fuse::physics::broadphase::candidatePairRejectReason({0u, 1u}, bodies, shapes) ==
            fuse::physics::broadphase::CandidateRejectReason::AabbSeparated,
        "reject reason flags AABB-separated pair");
        fuse::physics::broadphase::candidatePairRejectReason({0u, 0u}, bodies, shapes) ==
        "reject reason with bodies still flags self pair first");

    expectTrue(std::strcmp(fuse::physics::broadphase::candidateRejectReasonLabel(
                               fuse::physics::broadphase::CandidateRejectReason::BufferFull),
                           "buffer_full") == 0,
               "reject reason label maps buffer full");

void testPairBufferRejectReasonTracking() {

    expectTrue(!buffer.push(0u, 0u), "push rejects self-pair");
    expectTrue(buffer.lastRejectReason == fuse::physics::broadphase::CandidateRejectReason::SelfPair,
               "push records self-pair reject reason");

    expectTrue(buffer.push(0u, 1u), "push accepts first pair");
    expectTrue(!buffer.push(1u, 2u), "push rejects pair when buffer is full");
    expectTrue(buffer.lastRejectReason == fuse::physics::broadphase::CandidateRejectReason::BufferFull,
               "push records buffer-full reject reason");

    buffer.writeSlot(0u, 2u, 2u);
    buffer.writeSlot(1u, 0u, 3u);
    expectEq(buffer.compact(), 1u, "writeSlot rejects self-pair before compact");
    expectTrue(!buffer.hasInvalidSlots(), "compacted buffer has no invalid slots");
    expectTrue(buffer.canSkipCompact(), "compacted buffer skips subsequent compact");

void testCellSpanClampDiagnostics() {
    fuse::physics::broadphase::CellRange3 wideRange = {
        {-10, -10, -10},
        {10, 10, 10},
    };
    expectTrue(fuse::physics::broadphase::cellSpanExceedsClamp(wideRange, 8u),
               "wide range exceeds per-axis clamp threshold");
    expectTrue(!fuse::physics::broadphase::cellSpanExceedsClamp(wideRange, 0u),
               "zero clamp disables span-exceeds check");

    const fuse::physics::broadphase::CellRange3 clamped =
        fuse::physics::broadphase::clampCellRange3(wideRange, 8u);
    expectTrue(!fuse::physics::broadphase::cellSpanExceedsClamp(clamped, 8u),
               "clampCellRange3 brings span within threshold");

    expectTrue(fuse::physics::broadphase::clampCellSize(0.f) > 0.f, "clampCellSize rejects zero");
    expectTrue(fuse::physics::broadphase::clampCellSize(-1.f) > 0.f, "clampCellSize rejects negative");

    fuse::physics::broadphase::SpatialHashParams params;
    params.maxCellSpanPerAxis = 4u;

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphase2DIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "2D clamp prevents huge sphere from pairing with distant body");
}

void testEmptySetGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseInput(bodies, shapes),
               "empty bodies and shapes trigger broadphase early-out guard");
    expectTrue(fuse::physics::broadphase::isEmptyCellBucket(0u),
               "zero occupants is empty cell bucket");
    expectTrue(fuse::physics::broadphase::isEmptyCellBucket(1u),
               "single occupant is empty cell bucket");
    expectTrue(!fuse::physics::broadphase::isEmptyCellBucket(2u),
               "two occupants can produce pairs");

    const std::vector<fuse::physics::broadphase::CandidatePair> emptyList;
    expectTrue(fuse::physics::broadphase::isEmptyPairList(emptyList),
               "empty pair list guard");
}

void testCellOccupancyBudgetGuards() {
    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {1, 1, 1}};
    expectTrue(!fuse::physics::broadphase::exceedsCellOccupancyBudget(unitRange, 0u),
               "zero budget never exceeds");
    expectTrue(!fuse::physics::broadphase::exceedsCellOccupancyBudget(unitRange, 8u),
               "unit cube fits budget of eight");
    expectTrue(fuse::physics::broadphase::exceedsCellOccupancyBudget(unitRange, 4u),
               "unit cube exceeds budget of four");

    fuse::physics::broadphase::CellRange3 inverted = {{3, 3, 3}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(inverted, 64u),
               "inverted range skips insertion");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    expectTrue(fuse::physics::broadphase::exceedsCellOccupancyBudget(planeRange, 4u),
               "2D range exceeds occupancy budget");
}

void testPairListGuards() {
    std::vector<fuse::physics::broadphase::CandidatePair> pairs = {
        {0u, 1u},
        {1u, 1u},
        {0u, 3u},
    };
    expectTrue(!fuse::physics::broadphase::isEmptyPairList(pairs),
               "non-empty pair list guard");
    expectEq(fuse::physics::broadphase::countValidCandidatePairs(pairs, 2u), 1u,
             "countValidCandidatePairs filters self and out-of-range pairs");
    expectTrue(!fuse::physics::broadphase::pairListIsCanonical(pairs, 2u),
               "pairListIsCanonical rejects invalid entries");

    expectEq(fuse::physics::broadphase::pruneInvalidCandidatePairs(pairs, 2u), 1u,
             "pruneInvalidCandidatePairs keeps only valid pairs");
    expectTrue(fuse::physics::broadphase::pairListIsCanonical(pairs, 2u),
               "pruned pair list is canonical");
}

void testPairBufferInvalidPairPrune() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(3u);
    buffer.bodyA[0] = 0u;
    buffer.bodyB[0] = 1u;
    buffer.validFlags[0] = 1u;
    buffer.bodyA[1] = 2u;
    buffer.bodyB[1] = 2u;
    buffer.validFlags[1] = 1u;
    buffer.bodyA[2] = 0u;
    buffer.bodyB[2] = 5u;
    buffer.validFlags[2] = 1u;
    expectEq(buffer.countInvalidPairs(3u), 2u, "countInvalidPairs finds self and OOB pairs");

    expectEq(buffer.pruneInvalidPairs(3u), 1u, "pruneInvalidPairs compacts to valid pair");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "prune keeps valid pair");
    expectTrue(!buffer.hasDroppedPairs(), "prune does not increment dropped count");
}

void testBroadphaseOccupancyBudgetIntegration() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({500.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {256.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 256;
    params.maxCellSpanPerAxis = 0u;
    params.maxCellOccupancyCount = 8u;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "occupancy budget skips huge sphere cell insertion");
}

void testBroadphaseSkipInputGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    expectTrue(fuse::physics::broadphase::shouldSkipBroadphaseInput(0u, 0u),
               "empty bodies and shapes skip broadphase input");
    expectTrue(fuse::physics::broadphase::shouldSkipCellPairGeneration(1u),
               "single occupant skips cell pair generation");
    expectTrue(!fuse::physics::broadphase::shouldSkipCellPairGeneration(2u),
               "two occupants may generate cell pairs");
    expectTrue(fuse::physics::broadphase::shouldSkipBroadphaseRefine(0u, 0u, 1u, 1u),
               "empty buffer skips refine even with scene data");

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::SpatialHashParams params;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "skip-input guard leaves empty pair buffer");
}

void testCellOccupancyBudgetGuards() {
    fuse::physics::broadphase::CellRange3 wideRange = {
        {-10, -10, -10},
        {10, 10, 10},
    };
    expectTrue(fuse::physics::broadphase::cellSpanExceedsClamp(wideRange, 8u),
               "wide range exceeds per-axis clamp threshold");
    expectTrue(!fuse::physics::broadphase::cellSpanExceedsClamp(wideRange, 0u),
               "zero clamp disables span-exceeds check");

    const fuse::physics::broadphase::CellRange3 clamped =
        fuse::physics::broadphase::clampCellRange3(wideRange, 8u);
    expectTrue(!fuse::physics::broadphase::cellSpanExceedsClamp(clamped, 8u),
               "clampCellRange3 brings span within threshold");

    fuse::physics::broadphase::SpatialHashParams rawParams;
    rawParams.cellSize = 0.f;
    rawParams.tableSize = 0u;
    const fuse::physics::broadphase::SpatialHashParams safeParams =
        fuse::physics::broadphase::sanitizeSpatialHashParams(rawParams);
    expectEq(safeParams.cellSize, 1.f, "sanitizeSpatialHashParams clamps cell size");
    expectEq(safeParams.tableSize, 1u, "sanitizeSpatialHashParams clamps table size");

    const fuse::physics::broadphase::CellRange3 sphereRange =
        fuse::physics::broadphase::cellRangeFromSphere({0.f, 0.f, 0.f}, 512.f, 1.f, 4u);
    expectTrue(!fuse::physics::broadphase::exceedsCellOccupancyBudget(
                   sphereRange, fuse::physics::broadphase::cellOccupancyBudgetFromSpan(4u)),
               "clamped range fits occupancy budget derived from span");
    expectTrue(fuse::physics::broadphase::exceedsCellOccupancyBudget(wideRange, 64u),
               "unclamped wide range exceeds occupancy budget");
}

void testPairBufferRejectReasonTracking() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);

    expectTrue(!buffer.push(0u, 0u), "push rejects self-pair");
    expectTrue(buffer.lastRejectReason == fuse::physics::broadphase::CandidatePairRejectReason::SelfPair,
               "push records self-pair reject reason");

    expectTrue(buffer.push(0u, 1u), "push accepts first pair");
    expectTrue(!buffer.push(1u, 2u), "push rejects pair when buffer is full");
    expectTrue(buffer.lastRejectReason == fuse::physics::broadphase::CandidatePairRejectReason::BufferFull,
               "push records buffer-full reject reason");
    expectTrue(buffer.wouldRejectPush(2u, 3u), "wouldRejectPush reports full buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 2u, 2u);
    buffer.writeSlot(1u, 0u, 1u, 2u);
    expectEq(buffer.compact(), 1u, "writeSlot rejects self-pair and keeps in-range pair");
    expectTrue(!buffer.hasInvalidSlots(), "compacted buffer has no invalid slots");
    expectTrue(buffer.canSkipCompaction(), "compacted buffer skips subsequent compact");
}

void testPairBufferInvalidateInvalidPairs() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(3u);
    buffer.bodyA[0u] = 0u;
    buffer.bodyB[0u] = 1u;
    buffer.validFlags[0u] = 1u;
    buffer.bodyA[1u] = 0u;
    buffer.bodyB[1u] = 0u;
    buffer.validFlags[1u] = 1u;
    buffer.bodyA[2u] = 0u;
    buffer.bodyB[2u] = 2u;
    buffer.validFlags[2u] = 1u;
    expectEq(buffer.invalidateInvalidPairs(2u), 2u, "invalidateInvalidPairs removes self and OOB slots");
    expectEq(buffer.compact(), 1u, "compact after invalidation keeps valid pair");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "valid pair survives invalidation sweep");
}

void testCandidatePairRejectReasonWithRefine() {
    expectTrue(fuse::physics::broadphase::isOutOfRangeCandidatePair(0u, 2u, 2u),
               "isOutOfRangeCandidatePair detects OOB indices");
    expectTrue(!fuse::physics::broadphase::isOutOfRangeCandidatePair(0u, 1u, 2u),
               "isOutOfRangeCandidatePair accepts in-range pair");
    expectTrue(fuse::physics::broadphase::isRejectedCandidatePair(1u, 1u),
               "isRejectedCandidatePair flags self-pair");

    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::broadphase::candidatePairRejectReason({0u, 1u}, bodies, shapes) ==
            fuse::physics::broadphase::CandidatePairRejectReason::AabbSeparated,
        "reject reason flags AABB-separated pair");
    expectTrue(std::strcmp(fuse::physics::broadphase::candidatePairRejectReasonName(
                               fuse::physics::broadphase::CandidatePairRejectReason::BufferFull),
                           "BufferFull") == 0,
               "BufferFull reject reason has stable label");
}

void testBroadphase2DCellSpanClampIntegration() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({500.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {256.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 256;
    params.maxCellSpanPerAxis = 4u;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphase2DIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "2D clamp prevents huge sphere from pairing with distant body");
}

void testPairBufferSetMaxCapacityTrim() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    expectEq(buffer.activeCount, 2u, "buffer holds two pairs before trim");

    buffer.setMaxCapacity(1u);
    expectTrue(buffer.isFull(), "buffer reports full at max capacity");
    expectEq(buffer.activeCount, 1u, "setMaxCapacity trims excess pairs");
    expectEq(buffer.droppedCount, 1u, "setMaxCapacity records dropped pairs");
}

void testMaxCellOccupancyBudgetHelpers() {
    expectEq(fuse::physics::broadphase::maxCellOccupancyBudget3D(0u), 0u,
             "zero span clamp means unlimited 3D occupancy budget");
    expectEq(fuse::physics::broadphase::maxCellOccupancyBudget3D(4u), 64u,
             "3D occupancy budget is span cubed");
    expectEq(fuse::physics::broadphase::maxCellOccupancyBudget2D(4u), 16u,
             "2D occupancy budget is span squared");
}

void testShapeCellOccupancyPreflight() {
    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {1, 1, 1}};
    const auto withinBudget =
        fuse::physics::broadphase::preflight_shape_cell_occupancy(unitRange, 4u);
    expectTrue(withinBudget.can_insert(), "clamped unit range passes occupancy preflight");
    expectEq(withinBudget.estimatedCells, 8u, "preflight reports estimated cell count");
    expectEq(withinBudget.maxCells, 64u, "preflight reports max cell budget");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const auto skipped = fuse::physics::broadphase::preflight_shape_cell_occupancy(inverted, 4u);
    expectTrue(skipped.skipped, "empty range skips occupancy preflight");
    expectTrue(!skipped.can_insert(), "skipped occupancy preflight cannot insert");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const auto planePreflight =
        fuse::physics::broadphase::preflight_shape_cell_occupancy(planeRange, 2u);
    expectTrue(planePreflight.exceedsBudget, "2D preflight flags range above span budget");
    expectTrue(!planePreflight.can_insert(), "over-budget 2D range cannot insert");
}

void testBroadphasePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const auto emptyPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(emptyPreflight.skipped, "empty scene preflight is skipped");
    expectTrue(!emptyPreflight.can_run(), "empty scene preflight cannot run");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase matches empty preflight");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    const auto validPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(!validPreflight.skipped, "populated scene preflight is not skipped");
    expectTrue(validPreflight.can_run(), "populated scene preflight can run");
    expectEq(validPreflight.bodyCount, 1u, "preflight reports body count");
    expectEq(validPreflight.shapeCount, 1u, "preflight reports shape count");
}

void testBroadphaseRefinePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    const auto emptyPreflight =
        fuse::physics::broadphase::preflight_broadphase_refine(bodies, shapes, buffer);
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty input and buffer");
    expectTrue(!emptyPreflight.can_refine(), "empty refine preflight cannot refine");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseRefine(bodies, shapes, buffer),
               "canSkipBroadphaseRefine matches empty preflight");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);
    const auto validPreflight =
        fuse::physics::broadphase::preflight_broadphase_refine(bodies, shapes, buffer);
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid buffer");
    expectTrue(validPreflight.can_refine(), "refine preflight can refine valid buffer");
    expectEq(validPreflight.pairCount, 1u, "refine preflight reports pair count");
    expectTrue(!buffer.canSkipRefine(), "non-empty valid buffer does not skip refine");
}

void testPairBufferDedupePreflights() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    const auto emptyPreflight = buffer.preflight_dedupe();
    expectTrue(emptyPreflight.skipped, "empty buffer skips dedupe preflight");
    expectTrue(!emptyPreflight.needs_dedupe(), "empty buffer does not need dedupe");
    expectTrue(!buffer.needsDedupe(), "needsDedupe early-outs on empty buffer");
    expectTrue(!buffer.hasDuplicateCanonicalPairs(), "empty buffer has no duplicate pairs");

    buffer.push(0u, 1u);
    const auto singlePreflight = buffer.preflight_dedupe();
    expectTrue(singlePreflight.skipped, "single-pair buffer skips dedupe preflight");
    expectTrue(!buffer.needsDedupe(), "single pair does not need dedupe");

    buffer.push(0u, 1u);
    expectTrue(buffer.hasDuplicateCanonicalPairs(), "duplicate canonical pair detected");
    const auto duplicatePreflight = buffer.preflight_dedupe();
    expectTrue(!duplicatePreflight.skipped, "duplicate buffer runs dedupe preflight");
    expectEq(duplicatePreflight.duplicateCount, 1u, "preflight counts duplicate pairs");
    expectTrue(duplicatePreflight.needs_dedupe(), "duplicate buffer needs dedupe");
    expectTrue(buffer.needsDedupe(), "needsDedupe matches preflight");
}

void testBroadphaseInputPreflight() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const fuse::physics::broadphase::BroadphaseInputPreflight emptyPreflight =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(emptyPreflight.skipped, "empty input preflight is skipped");
    expectTrue(emptyPreflight.emptyBodies, "empty input preflight marks empty bodies");
    expectTrue(emptyPreflight.emptyShapes, "empty input preflight marks empty shapes");
    expectTrue(!emptyPreflight.can_run(), "empty input preflight cannot run");
    expectTrue(fuse::physics::broadphase::should_skip_broadphase(bodies, shapes),
               "should_skip_broadphase on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::physics::broadphase::BroadphaseInputPreflight missingShapes =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(missingShapes.skipped, "bodies-only preflight is skipped");
    expectTrue(!missingShapes.emptyBodies, "bodies-only preflight has bodies");
    expectTrue(missingShapes.emptyShapes, "bodies-only preflight marks empty shapes");

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphaseInputPreflight validPreflight =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(!validPreflight.skipped, "populated input preflight is not skipped");
    expectTrue(validPreflight.can_run(), "populated input preflight can run");
    expectTrue(!fuse::physics::broadphase::should_skip_broadphase(bodies, shapes),
               "should_skip_broadphase on populated scene");
}

void testCellOccupancyPreflight() {
    const fuse::physics::broadphase::CellRange3 smallRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 8u);
    expectTrue(!withinBudget.skipped, "non-empty range preflight is not skipped");
    expectTrue(!withinBudget.emptyRange, "non-empty range preflight is populated");
    expectEq(withinBudget.estimatedCells, 8u, "cell occupancy preflight estimates cells");
    expectTrue(withinBudget.can_insert(), "within-budget preflight can insert");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyInsert(smallRange, 8u),
               "within-budget range does not skip insert");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 4u);
    expectTrue(overBudget.exceedsBudget, "over-budget preflight flags exceed");
    expectTrue(!overBudget.can_insert(), "over-budget preflight cannot insert");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyInsert(smallRange, 4u),
               "over-budget range skips insert");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight emptyRange =
        fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(emptyRange.skipped, "empty range preflight is skipped");
    expectTrue(emptyRange.emptyRange, "empty range preflight marks empty range");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyInsert(inverted, 4u),
               "empty range skips insert");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight planePreflight =
        fuse::physics::broadphase::preflight_cell_occupancy(planeRange, 4u);
    expectTrue(planePreflight.exceedsBudget, "2D occupancy preflight flags exceed");
}

void testBroadphaseCellOccupancyIntegration() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({500.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {256.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 256;
    params.maxCellSpanPerAxis = 64u;
    params.maxCellOccupancy = 8u;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "occupancy-budgeted huge sphere skips distant body pair");
}

void testRefineBroadphasePreflight() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    const fuse::physics::broadphase::RefineBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflight_refine_broadphase(bodies, shapes, buffer);
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty buffer and input");
    expectTrue(emptyPreflight.emptyBuffer, "refine preflight marks empty buffer");
    expectTrue(emptyPreflight.emptyInput, "refine preflight marks empty input");
    expectTrue(!emptyPreflight.can_refine(), "empty refine preflight cannot refine");
    expectTrue(fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    const fuse::physics::broadphase::RefineBroadphasePreflight validPreflight =
        fuse::physics::broadphase::preflight_refine_broadphase(bodies, shapes, buffer);
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid pair buffer");
    expectTrue(validPreflight.can_refine(), "valid refine preflight can refine");
    expectTrue(!fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on valid pair buffer");
}

void testPairBufferDedupePreflight() {
void testBroadphasePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const auto emptyPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(emptyPreflight.emptyInput, "preflight marks empty scene");
    expectTrue(emptyPreflight.skipped, "preflight skips empty scene");
    expectTrue(!emptyPreflight.can_dispatch(), "preflight cannot dispatch empty scene");
    expectTrue(fuse::physics::broadphase::can_skip_broadphase_dispatch(bodies, shapes),
               "can_skip_broadphase_dispatch on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    const auto singletonPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(singletonPreflight.singletonInput, "preflight marks singleton scene");
    expectTrue(singletonPreflight.skipped, "preflight skips singleton scene");

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    const auto validPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(!validPreflight.skipped, "preflight does not skip populated scene");
    expectTrue(validPreflight.can_dispatch(), "preflight can dispatch populated scene");
    expectTrue(!fuse::physics::broadphase::can_skip_broadphase_dispatch(bodies, shapes),
               "populated scene does not skip dispatch");
}

void testRefineBroadphasePreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;

    const auto emptyPreflight =
        fuse::physics::broadphase::preflight_refine_broadphase(bodies, shapes, buffer);
    expectTrue(emptyPreflight.emptyBuffer, "refine preflight marks empty buffer");
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty buffer");
    expectTrue(!emptyPreflight.can_refine(), "refine preflight cannot refine empty buffer");
    expectTrue(fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on empty buffer");

    const auto singletonPreflight =
    expectTrue(singletonPreflight.skippedBroadphase, "refine preflight skips singleton broadphase");
    expectTrue(singletonPreflight.skipped, "refine preflight skips singleton scene");

    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    buffer.push(0u, 1u);
    const auto validPreflight =
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid scene");
    expectTrue(validPreflight.can_refine(), "refine preflight can refine valid buffer");
    expectTrue(!fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "valid scene does not skip refine");
    expectTrue(buffer.canSkipRefine() == false, "non-empty valid buffer does not skip refine");

void testDedupeBroadphasePreflightGuards() {
    const auto emptyPreflight = fuse::physics::broadphase::preflight_dedupe_broadphase(buffer);
    expectTrue(emptyPreflight.skipped, "dedupe preflight skips empty buffer");
    expectTrue(!emptyPreflight.can_dedupe(), "dedupe preflight cannot dedupe empty buffer");
    expectTrue(fuse::physics::broadphase::should_skip_dedupe_broadphase(buffer),
               "should_skip_dedupe_broadphase on empty buffer");
    expectTrue(buffer.canSkipDedupePass(), "empty buffer skips dedupe pass");

    const auto singlePreflight = fuse::physics::broadphase::preflight_dedupe_broadphase(buffer);
    expectTrue(singlePreflight.skipped, "dedupe preflight skips single pair");
    expectTrue(buffer.isDuplicateFree(), "single pair is duplicate-free");

    buffer.push(2u, 3u);
    buffer.sortCanonical();
    expectTrue(buffer.isSortedCanonical(), "sorted buffer is canonically sorted");
    expectTrue(buffer.isDuplicateFree(), "unique pairs are duplicate-free");
    const auto sortedPreflight = fuse::physics::broadphase::preflight_dedupe_broadphase(buffer);
    expectTrue(sortedPreflight.skipped, "dedupe preflight skips sorted unique pairs");
    expectTrue(buffer.canSkipDedupePass(), "sorted unique buffer skips dedupe pass");

    fuse::physics::broadphase::PairBufferSoA duplicateBuffer;
    duplicateBuffer.push(2u, 3u);
    duplicateBuffer.push(0u, 1u);
    expectTrue(!duplicateBuffer.isDuplicateFree(), "duplicate buffer is not duplicate-free");
    expectTrue(!duplicateBuffer.canSkipDedupePass(), "duplicate buffer needs dedupe pass");
    const auto duplicatePreflight = fuse::physics::broadphase::preflight_dedupe_broadphase(duplicateBuffer);
    expectTrue(!duplicatePreflight.skipped, "dedupe preflight does not skip duplicate pairs");
    expectTrue(duplicatePreflight.can_dedupe(), "dedupe preflight can dedupe duplicate pairs");

void testCellOccupancyPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 smallRange = {{0, 0, 0}, {1, 1, 1}};
    const auto withinBudget =
        fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 8u);
    expectTrue(!withinBudget.skipped, "within-budget range does not skip insertion");
    expectTrue(withinBudget.can_insert(), "within-budget range can insert");
    expectEq(withinBudget.occupancyCount, 8u, "preflight reports occupancy count");
    expectEq(withinBudget.budgetRemaining, 0u, "at-budget range leaves zero headroom");
    expectTrue(!fuse::physics::broadphase::should_skip_shape_cell_insertion(smallRange, 8u),
               "at-budget range does not skip insertion");

    const auto overBudget =
        fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 7u);
    expectTrue(overBudget.exceedsBudget, "over-budget range exceeds budget");
    expectTrue(overBudget.skipped, "over-budget range skips insertion");
    expectTrue(!overBudget.can_insert(), "over-budget range cannot insert");
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insertion(smallRange, 7u),
               "over-budget range skips insertion");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const auto emptyPreflight = fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(emptyPreflight.emptyRange, "preflight marks empty range");
    expectTrue(emptyPreflight.skipped, "preflight skips empty range");
    expectEq(emptyPreflight.budgetRemaining, 4u, "empty range leaves full budget");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const auto planePreflight = fuse::physics::broadphase::preflight_cell_occupancy_2d(planeRange, 4u);
    expectTrue(planePreflight.exceedsBudget, "2D preflight flags over-budget range");
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insertion_2d(planeRange, 4u),
               "2D over-budget range skips insertion");

void testPairBufferSortCanonicalIfNeeded() {
    buffer.sortCanonicalIfNeeded();
    expectTrue(buffer.canSkipSortCanonical(), "empty buffer skips sort");

    expectTrue(!buffer.isSortedCanonical(), "unsorted pairs are not canonically sorted");
    expectTrue(!buffer.canSkipSortCanonical(), "unsorted pairs need canonical sort");

    expectTrue(buffer.isSortedCanonical(), "sortCanonicalIfNeeded sorts pairs");
    expectTrue(buffer.canSkipSortCanonical(), "sorted pairs skip repeat sort");

    expectTrue(buffer.containsCanonicalPair(0u, 1u), "repeat sort preserves pairs");

void testPairBufferCanSkipMaxCapacityClamp() {
    fuse::physics::broadphase::PairBufferSoA buffer;

    const fuse::physics::broadphase::PairBufferDedupePreflight emptyPreflight =
        fuse::physics::broadphase::preflight_dedupe_pair_buffer(buffer);
    expectTrue(emptyPreflight.skipped, "dedupe preflight skips empty buffer");
    expectTrue(!emptyPreflight.needs_dedupe(), "empty buffer does not need dedupe");
    expectTrue(fuse::physics::broadphase::should_skip_dedupe_pair_buffer(buffer),
               "should_skip_dedupe_pair_buffer on empty buffer");

    buffer.push(0u, 1u);
    const fuse::physics::broadphase::PairBufferDedupePreflight singlePreflight =
        fuse::physics::broadphase::preflight_dedupe_pair_buffer(buffer);
    expectTrue(singlePreflight.skipped, "dedupe preflight skips single pair");
    expectTrue(!singlePreflight.needs_dedupe(), "single pair does not need dedupe");

    buffer.push(2u, 3u);
    const fuse::physics::broadphase::PairBufferDedupePreflight multiPreflight =
        fuse::physics::broadphase::preflight_dedupe_pair_buffer(buffer);
    expectTrue(!multiPreflight.skipped, "multi-pair dedupe preflight is not skipped");
    expectTrue(multiPreflight.needs_dedupe(), "multi-pair buffer needs dedupe");
    expectTrue(multiPreflight.can_dedupe(), "multi-pair buffer can dedupe");
    expectTrue(!fuse::physics::broadphase::should_skip_dedupe_pair_buffer(buffer),
               "should_skip_dedupe_pair_buffer on multi-pair buffer");
}

void testPairBufferClampPreflight() {
    fuse::physics::broadphase::PairBufferSoA buffer;

    const fuse::physics::broadphase::PairBufferClampPreflight emptyPreflight =
        fuse::physics::broadphase::preflight_pair_buffer_clamp(buffer);
    expectTrue(emptyPreflight.skipped, "clamp preflight skips empty buffer");
    expectTrue(!emptyPreflight.needs_clamp(), "empty buffer does not need clamp");

    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    buffer.push(4u, 5u);
    buffer.setMaxCapacity(2u);

    const fuse::physics::broadphase::PairBufferClampPreflight overflowPreflight =
        fuse::physics::broadphase::preflight_pair_buffer_clamp(buffer);
    expectTrue(!overflowPreflight.skipped, "overflow clamp preflight is not skipped");
    expectTrue(overflowPreflight.needs_clamp(), "overflow buffer needs clamp");
    expectTrue(overflowPreflight.can_clamp(), "overflow buffer can clamp");
    expectEq(overflowPreflight.excessCount, 1u, "clamp preflight counts excess pairs");
}

void testPairBufferDedupeAndClampGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(buffer.canSkipDedupeAndClamp(), "empty buffer skips dedupe and clamp");

    buffer.push(0u, 1u);
    expectTrue(buffer.canSkipDedupeAndClamp(), "single pair skips dedupe and clamp");

    buffer.push(2u, 3u);
    expectTrue(!buffer.canSkipDedupeAndClamp(), "multi pair does not skip dedupe");

    buffer.setMaxCapacity(1u);
    expectTrue(!buffer.canSkipDedupeAndClamp(), "overflow buffer does not skip clamp");
}

void testEmptyBroadphaseOutputGuard() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseOutput(buffer),
               "empty buffer is empty broadphase output");

    buffer.preparePairSlots(0u);
    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseOutput(buffer),
               "zero-slot buffer is empty broadphase output");

    buffer.push(0u, 1u);
    expectTrue(!fuse::physics::broadphase::isEmptyBroadphaseOutput(buffer),
               "active pair buffer is not empty output");
}

void testBroadphaseInputPreflight() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const fuse::physics::broadphase::BroadphaseInputPreflight emptyPreflight =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(emptyPreflight.skipped, "empty input preflight is skipped");
    expectTrue(emptyPreflight.emptyBodies, "empty input preflight marks empty bodies");
    expectTrue(emptyPreflight.emptyShapes, "empty input preflight marks empty shapes");
    expectTrue(!emptyPreflight.can_run(), "empty input preflight cannot run");
    expectTrue(fuse::physics::broadphase::should_skip_broadphase(bodies, shapes),
               "should_skip_broadphase on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::physics::broadphase::BroadphaseInputPreflight missingShapes =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(missingShapes.skipped, "bodies-only preflight is skipped");
    expectTrue(!missingShapes.emptyBodies, "bodies-only preflight has bodies");
    expectTrue(missingShapes.emptyShapes, "bodies-only preflight marks empty shapes");

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphaseInputPreflight validPreflight =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(!validPreflight.skipped, "populated input preflight is not skipped");
    expectTrue(validPreflight.can_run(), "populated input preflight can run");
    expectTrue(!fuse::physics::broadphase::should_skip_broadphase(bodies, shapes),
               "should_skip_broadphase on populated scene");
}

void testCellOccupancyPreflight() {
    const fuse::physics::broadphase::CellRange3 smallRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 8u);
    expectTrue(!withinBudget.skipped, "non-empty range preflight is not skipped");
    expectTrue(!withinBudget.emptyRange, "non-empty range preflight is populated");
    expectEq(withinBudget.estimatedCells, 8u, "cell occupancy preflight estimates cells");
    expectTrue(withinBudget.can_insert(), "within-budget preflight can insert");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyInsert(smallRange, 8u),
               "within-budget range does not skip insert");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 4u);
    expectTrue(overBudget.exceedsBudget, "over-budget preflight flags exceed");
    expectTrue(!overBudget.can_insert(), "over-budget preflight cannot insert");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyInsert(smallRange, 4u),
               "over-budget range skips insert");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight emptyRange =
        fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(emptyRange.skipped, "empty range preflight is skipped");
    expectTrue(emptyRange.emptyRange, "empty range preflight marks empty range");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyInsert(inverted, 4u),
               "empty range skips insert");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight planePreflight =
        fuse::physics::broadphase::preflight_cell_occupancy(planeRange, 4u);
    expectTrue(planePreflight.exceedsBudget, "2D occupancy preflight flags exceed");
}

void testBroadphaseCellOccupancyIntegration() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({500.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {256.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 1.f;
    params.tableSize = 256;
    params.maxCellSpanPerAxis = 64u;
    params.maxCellOccupancy = 8u;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "occupancy-budgeted huge sphere skips distant body pair");
}

void testRefineBroadphasePreflight() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    const fuse::physics::broadphase::RefineBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflight_refine_broadphase(bodies, shapes, buffer);
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty buffer and input");
    expectTrue(emptyPreflight.emptyBuffer, "refine preflight marks empty buffer");
    expectTrue(emptyPreflight.emptyInput, "refine preflight marks empty input");
    expectTrue(!emptyPreflight.can_refine(), "empty refine preflight cannot refine");
    expectTrue(fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    const fuse::physics::broadphase::RefineBroadphasePreflight validPreflight =
        fuse::physics::broadphase::preflight_refine_broadphase(bodies, shapes, buffer);
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid pair buffer");
    expectTrue(validPreflight.can_refine(), "valid refine preflight can refine");
    expectTrue(!fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on valid pair buffer");
}

void testPairBufferDedupePreflight() {
    fuse::physics::broadphase::PairBufferSoA buffer;

    const fuse::physics::broadphase::PairBufferDedupePreflight emptyPreflight =
        fuse::physics::broadphase::preflight_dedupe_pair_buffer(buffer);
    expectTrue(emptyPreflight.skipped, "dedupe preflight skips empty buffer");
    expectTrue(!emptyPreflight.needs_dedupe(), "empty buffer does not need dedupe");
    expectTrue(fuse::physics::broadphase::should_skip_dedupe_pair_buffer(buffer),
               "should_skip_dedupe_pair_buffer on empty buffer");

    buffer.push(0u, 1u);
    const fuse::physics::broadphase::PairBufferDedupePreflight singlePreflight =
        fuse::physics::broadphase::preflight_dedupe_pair_buffer(buffer);
    expectTrue(singlePreflight.skipped, "dedupe preflight skips single pair");
    expectTrue(!singlePreflight.needs_dedupe(), "single pair does not need dedupe");

    buffer.push(2u, 3u);
    const fuse::physics::broadphase::PairBufferDedupePreflight multiPreflight =
        fuse::physics::broadphase::preflight_dedupe_pair_buffer(buffer);
    expectTrue(!multiPreflight.skipped, "multi-pair dedupe preflight is not skipped");
    expectTrue(multiPreflight.needs_dedupe(), "multi-pair buffer needs dedupe");
    expectTrue(multiPreflight.can_dedupe(), "multi-pair buffer can dedupe");
    expectTrue(!fuse::physics::broadphase::should_skip_dedupe_pair_buffer(buffer),
               "should_skip_dedupe_pair_buffer on multi-pair buffer");
}

void testPairBufferClampPreflight() {
    fuse::physics::broadphase::PairBufferSoA buffer;

    const fuse::physics::broadphase::PairBufferClampPreflight emptyPreflight =
        fuse::physics::broadphase::preflight_pair_buffer_clamp(buffer);
    expectTrue(emptyPreflight.skipped, "clamp preflight skips empty buffer");
    expectTrue(!emptyPreflight.needs_clamp(), "empty buffer does not need clamp");

    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    buffer.push(4u, 5u);
    buffer.setMaxCapacity(2u);

    const fuse::physics::broadphase::PairBufferClampPreflight overflowPreflight =
        fuse::physics::broadphase::preflight_pair_buffer_clamp(buffer);
    expectTrue(!overflowPreflight.skipped, "overflow clamp preflight is not skipped");
    expectTrue(overflowPreflight.needs_clamp(), "overflow buffer needs clamp");
    expectTrue(overflowPreflight.can_clamp(), "overflow buffer can clamp");
    expectEq(overflowPreflight.excessCount, 1u, "clamp preflight counts excess pairs");
}

void testPairBufferDedupeAndClampGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(buffer.canSkipDedupeAndClamp(), "empty buffer skips dedupe and clamp");

    buffer.push(0u, 1u);
    expectTrue(buffer.canSkipDedupeAndClamp(), "single pair skips dedupe and clamp");

    buffer.push(2u, 3u);
    expectTrue(!buffer.canSkipDedupeAndClamp(), "multi pair does not skip dedupe");

    buffer.setMaxCapacity(1u);
    expectTrue(!buffer.canSkipDedupeAndClamp(), "overflow buffer does not skip clamp");
}

void testEmptyBroadphaseOutputGuard() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseOutput(buffer),
               "empty buffer is empty broadphase output");

    buffer.preparePairSlots(0u);
    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseOutput(buffer),
               "zero-slot buffer is empty broadphase output");

    buffer.push(0u, 1u);
    expectTrue(!fuse::physics::broadphase::isEmptyBroadphaseOutput(buffer),
               "active pair buffer is not empty output");
}

void testCellOccupancyPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {1, 1, 1}};
    const auto withinBudget =
        fuse::physics::broadphase::preflight_cell_occupancy(unitRange, 8u);
    expectTrue(withinBudget.can_insert_cells(), "preflight allows occupancy within budget");
    expectTrue(!withinBudget.skipped, "within-budget preflight is not skipped");
    expectEq(withinBudget.estimatedCells, 8u, "preflight reports estimated cell count");

    const auto overBudget =
        fuse::physics::broadphase::preflight_cell_occupancy(unitRange, 7u);
    expectTrue(!overBudget.can_insert_cells(), "preflight rejects occupancy over budget");
    expectTrue(overBudget.exceedsBudget, "preflight flags budget overflow");
    expectTrue(overBudget.skipped, "over-budget preflight is skipped");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const auto emptyRange = fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(emptyRange.emptyRange, "preflight marks inverted range empty");
    expectTrue(!emptyRange.can_insert_cells(), "empty range cannot insert cells");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const auto planePreflight =
        fuse::physics::broadphase::preflight_cell_occupancy(planeRange, 4u);
    expectTrue(planePreflight.exceedsBudget, "2D preflight flags budget overflow");
}

void testBroadphaseDispatchPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const auto emptyPreflight =
        fuse::physics::broadphase::preflight_broadphase_dispatch(bodies, shapes);
    expectTrue(emptyPreflight.emptyInput, "dispatch preflight marks empty scene");
    expectTrue(!emptyPreflight.can_dispatch(), "empty scene cannot dispatch broadphase");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    const auto populatedPreflight =
        fuse::physics::broadphase::preflight_broadphase_dispatch(bodies, shapes);
    expectTrue(!populatedPreflight.emptyInput, "populated scene is not empty input");
    expectTrue(populatedPreflight.can_dispatch(), "populated scene can dispatch broadphase");
}

void testBroadphaseRefinePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    const auto emptyPreflight =
        fuse::physics::broadphase::preflight_broadphase_refine(bodies, shapes, buffer);
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty scene and buffer");
    expectTrue(!emptyPreflight.can_refine(), "empty refine preflight cannot refine");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    const auto validPreflight =
        fuse::physics::broadphase::preflight_broadphase_refine(bodies, shapes, buffer);
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid scene");
    expectTrue(validPreflight.can_refine(), "valid refine preflight can refine");
    expectEq(validPreflight.validPairCount, 1u, "refine preflight counts valid pairs");
}

void testCanSkipRefineBroadphaseIntegration() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase on empty buffer");

    fuse::physics::broadphase::refineBroadphasePairsParallel(bodies, shapes, buffer);
    expectTrue(buffer.isEmpty(), "refine on skipped preflight leaves buffer empty");
}

void testPairBufferCanSkipCompactAndClamp() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer skips compactAndClamp");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs when empty");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.invalidateSlot(0u);
    expectTrue(buffer.canSkipCompactAndClamp(), "all-invalid slots skip compactAndClamp");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp clears all-invalid slot storage");

    buffer.clear();
    buffer.push(2u, 3u);
    buffer.push(0u, 1u);
    buffer.setMaxCapacity(1u);
    expectTrue(!buffer.canSkipCompactAndClamp(), "overflow buffer does not skip compactAndClamp");
    expectEq(buffer.compactAndClamp(), 1u, "compactAndClamp gathers and clamps overflow");
}

void testPairBufferSlotValidityBounds() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    expectTrue(buffer.slotIsValid(0u), "in-range slot is valid");
    expectTrue(!buffer.slotIsValid(2u), "out-of-range slot is invalid");
    buffer.invalidateSlot(2u);
    expectTrue(buffer.slotIsValid(0u), "out-of-range invalidate is a no-op");
}

void testPairBufferSparseCanonicalSort() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, 2u, 3u);
    buffer.writeSlot(2u, 0u, 1u);
    expectTrue(!buffer.isSortedCanonical(), "sparse unsorted slots are not canonical");
    expectTrue(!buffer.canSkipDedupe(), "multi-slot buffer does not skip dedupe");
    expectEq(buffer.compact(), 2u, "compact gathers sparse valid slots");
    expectTrue(!buffer.isSortedCanonical(), "compact alone does not canonicalize order");
    buffer.sortCanonical();
    expectTrue(buffer.isSortedCanonical(), "sortCanonical orders compacted pairs");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "sortCanonical preserves first pair");
    expectTrue(buffer.containsCanonicalPair(2u, 3u), "sortCanonical preserves second pair");
}

void testBroadphaseInputPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const fuse::physics::broadphase::BroadphaseInputPreflight emptyPreflight =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(emptyPreflight.skipped, "empty scene preflight is skipped");
    expectTrue(emptyPreflight.emptyBodies, "empty scene reports empty bodies");
    expectTrue(emptyPreflight.emptyShapes, "empty scene reports empty shapes");
    expectTrue(!emptyPreflight.can_build(), "empty scene cannot build broadphase");
    expectTrue(fuse::physics::broadphase::should_skip_broadphase_build(bodies, shapes),
               "should_skip_broadphase_build on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::physics::broadphase::BroadphaseInputPreflight missingShapes =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(missingShapes.skipped, "bodies without shapes preflight is skipped");
    expectTrue(!missingShapes.emptyBodies, "bodies without shapes still has bodies");
    expectTrue(missingShapes.emptyShapes, "bodies without shapes reports empty shapes");

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphaseInputPreflight validPreflight =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(!validPreflight.skipped, "populated scene preflight is not skipped");
    expectTrue(validPreflight.can_build(), "populated scene can build broadphase");
    expectTrue(!fuse::physics::broadphase::should_skip_broadphase_build(bodies, shapes),
               "should_skip_broadphase_build on populated scene");
}

void testCellOccupancyPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 smallRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 8u);
    expectTrue(!withinBudget.skipped, "non-empty range preflight is not skipped");
    expectTrue(withinBudget.within_budget(), "occupancy within budget");
    expectTrue(withinBudget.can_insert(), "occupancy can insert within budget");
    expectEq(withinBudget.occupancyCount, 8u, "preflight reports occupancy count");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 4u);
    expectTrue(overBudget.exceedsBudget, "preflight flags over-budget occupancy");
    expectTrue(!overBudget.within_budget(), "over-budget preflight is not within budget");
    expectTrue(!overBudget.can_insert(), "over-budget preflight cannot insert");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight emptyRange =
        fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(emptyRange.skipped, "empty range preflight is skipped");
    expectTrue(emptyRange.emptyRange, "empty range preflight reports empty range");
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insert(inverted),
               "should_skip_shape_cell_insert on empty range");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight planePreflight =
        fuse::physics::broadphase::preflight_cell_occupancy(planeRange, 4u);
    expectEq(planePreflight.occupancyCount, 8u, "2D preflight reports occupancy count");
    expectTrue(planePreflight.exceedsBudget, "2D preflight flags over-budget occupancy");
}

void testPairBufferPreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    const fuse::physics::broadphase::PairBufferPreflight emptyPreflight =
        fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(emptyPreflight.skipped, "empty buffer preflight is skipped");
    expectTrue(emptyPreflight.skipDedupe, "empty buffer skips dedupe");
    expectTrue(emptyPreflight.skipCompaction, "empty buffer skips compaction");
    expectTrue(fuse::physics::broadphase::should_skip_pair_buffer_dedupe(buffer),
               "should_skip_pair_buffer_dedupe on empty buffer");
    expectTrue(fuse::physics::broadphase::should_skip_pair_buffer_compaction(buffer),
               "should_skip_pair_buffer_compaction on empty buffer");

    buffer.setMaxCapacity(2u);
    buffer.push(0u, 1u);
    const fuse::physics::broadphase::PairBufferPreflight partialPreflight =
        fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(!partialPreflight.skipped, "partial buffer preflight is not skipped");
    expectTrue(partialPreflight.can_push(1u), "partial buffer can push one more pair");
    expectTrue(!partialPreflight.can_push(2u), "partial buffer rejects two more pairs");
    expectTrue(!partialPreflight.full, "partial buffer is not full");
    expectEq(partialPreflight.remainingCapacity, 1u, "preflight reports remaining capacity");

    buffer.push(2u, 3u);
    const fuse::physics::broadphase::PairBufferPreflight fullPreflight =
        fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(fullPreflight.full, "full buffer preflight reports full");
    expectTrue(!fullPreflight.can_push(1u), "full buffer preflight rejects push");
    expectTrue(!fullPreflight.skipDedupe, "two-pair buffer does not skip dedupe");

    fuse::physics::broadphase::PairBufferSoA singlePairBuffer;
    singlePairBuffer.push(0u, 1u);
    const fuse::physics::broadphase::PairBufferPreflight singlePreflight =
        fuse::physics::broadphase::preflight_pair_buffer(singlePairBuffer);
    expectTrue(singlePreflight.skipDedupe, "single canonical pair skips dedupe");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(2u, 3u);
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(4u, 5u);
    overflowBuffer.setMaxCapacity(2u);
    const fuse::physics::broadphase::PairBufferPreflight clampPreflight =
        fuse::physics::broadphase::preflight_pair_buffer(overflowBuffer);
    expectTrue(clampPreflight.needsClamp, "overflow buffer preflight needs clamp");
    expectTrue(overflowBuffer.needsMaxCapacityClamp(), "needsMaxCapacityClamp mirrors preflight");
}

void testRefineBroadphasePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    const fuse::physics::broadphase::RefineBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflight_refine_broadphase(bodies, shapes, buffer);
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty input and buffer");
    expectTrue(emptyPreflight.emptyInput, "refine preflight reports empty input");
    expectTrue(emptyPreflight.emptyBuffer, "refine preflight reports empty buffer");
    expectTrue(!emptyPreflight.can_refine(), "empty refine preflight cannot refine");
    expectTrue(fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    const fuse::physics::broadphase::RefineBroadphasePreflight validPreflight =
        fuse::physics::broadphase::preflight_refine_broadphase(bodies, shapes, buffer);
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid scene");
    expectTrue(!validPreflight.emptyInput, "refine preflight has populated input");
    expectTrue(!validPreflight.emptyBuffer, "refine preflight has valid buffer");
    expectTrue(validPreflight.can_refine(), "valid refine preflight can refine");
    expectEq(validPreflight.validPairCount, 1u, "refine preflight counts valid pairs");
    expectTrue(!fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on valid scene");
}

void testPairBufferCompactionPreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);

    const fuse::physics::broadphase::PairBufferPreflight compactPreflight =
        fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(compactPreflight.skipCompaction, "all-valid slots skip compaction in preflight");
    expectTrue(fuse::physics::broadphase::should_skip_pair_buffer_compaction(buffer),
               "should_skip_pair_buffer_compaction on all-valid slots");

    buffer.invalidateSlot(0u);
    const fuse::physics::broadphase::PairBufferPreflight needsCompactPreflight =
        fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(!needsCompactPreflight.skipCompaction, "invalid slot needs compaction");
    expectTrue(!fuse::physics::broadphase::should_skip_pair_buffer_compaction(buffer),
               "should_skip_pair_buffer_compaction false when invalid slots exist");
}

void testCellOccupancyPreflightReasonGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectEq(static_cast<fuse::u32>(withinBudget.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "within-budget preflight carries None reason");
    expectTrue(withinBudget.canIterate(), "within-budget preflight can iterate");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 8u),
               "canSkipCellOccupancyIteration false within budget");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectEq(static_cast<fuse::u32>(overBudget.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "over-budget preflight carries ExceedsBudget reason");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "canSkipCellOccupancyIteration true over budget");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight emptyRange =
        fuse::physics::broadphase::preflightCellOccupancy(inverted, 4u);
    expectEq(static_cast<fuse::u32>(emptyRange.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
             "inverted range preflight carries EmptyRange reason");
}

void testBroadphaseMergeRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "empty scene reports EmptyPlaneBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge false on empty scene");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseMergeRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
                           "EmptyDynamicBodies") == 0,
               "EmptyDynamicBodies merge reject reason has stable label");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::broadphaseMergeRejectsForReason(
                   bodies, shapes,
                   fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
               "broadphaseMergeRejectsForReason matches plane-only scene");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge-ready scene reports None merge reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge true when merge is viable");
}

void testPairBufferPushRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
             "self-pair reports InvalidPair push reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferPushRejectsForReason(
                   buffer, 1u, 1u, fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
               "pairBufferPushRejectsForReason matches self-pair");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPushRejectReasonName(
                               fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
                           "AtCapacity") == 0,
               "AtCapacity push reject reason has stable label");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity push reject reason");

    const fuse::physics::broadphase::PairBufferPushPreflight validPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u);
    expectEq(static_cast<fuse::u32>(validPush.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "push preflight carries AtCapacity reason on full buffer");
}

void testPairBufferCompactionRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compaction reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferCompactionRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
               "pairBufferCompactionRejectsForReason matches empty buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
             "all-valid slots report AllValid compaction reject reason");

    fuse::physics::broadphase::PairBufferSoA sparseBuffer;
    sparseBuffer.preparePairSlots(2u);
    sparseBuffer.writeSlot(0u, 0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(sparseBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "sparse slots report None compaction reject reason");
    expectTrue(fuse::physics::broadphase::preflightPairBufferCompaction(sparseBuffer).needsCompaction(),
               "sparse slots compaction preflight needs work");
}

void testPairBufferClampRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer clamp reject reason");

    buffer.push(0u, 1u);
    buffer.setMaxCapacity(2u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
             "within-capacity buffer reports WithinCapacity clamp reject reason");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(2u, 3u);
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(4u, 5u);
    overflowBuffer.setMaxCapacity(2u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(overflowBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::None),
             "overflow buffer reports None clamp reject reason");
    expectTrue(fuse::physics::broadphase::preflightPairBufferClamp(overflowBuffer).needsClamp(),
               "overflow clamp preflight needs truncation");
}

void testPairBufferDedupeSortRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer dedupe reject reason");
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer sort reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe false on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort false on empty buffer");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
             "single pair reports SinglePair dedupe reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSortRejectReasonName(
                               fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair sort reject reason has stable label");

    buffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::None),
             "multiple pairs report None dedupe reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe true for multiple pairs");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort true for multiple pairs");
}

void testShouldRunBroadphaseAndRefineGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase true on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");

    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase true with valid pair buffer");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase false when refine is viable");
}

void testBroadphaseMergeRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "empty scene reports EmptyPlaneBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::broadphaseMergeRejectsForReason(
                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
               "broadphaseMergeRejectsForReason matches empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseMergeRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
                           "EmptyDynamicBodies") == 0,
               "EmptyDynamicBodies merge reject reason has stable label");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies merge reject reason");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge-ready scene reports None merge reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge false when merge is viable");

    const fuse::physics::broadphase::BroadphaseMergePreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge preflight carries reject reason");
}

void testCellOccupancyPreflightReasonField() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectEq(static_cast<fuse::u32>(withinBudget.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "within-budget preflight carries None reason");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight emptyRange =
        fuse::physics::broadphase::preflightCellOccupancy(inverted, 4u);
    expectEq(static_cast<fuse::u32>(emptyRange.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
             "empty-range preflight carries EmptyRange reason");
    expectTrue(!emptyRange.canIterate(), "empty-range preflight cannot iterate");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectEq(static_cast<fuse::u32>(overBudget.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "over-budget preflight carries ExceedsBudget reason");
}

void testPairBufferPushRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
             "self-pair reports InvalidPair push reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferPushRejectsForReason(
                   buffer, 2u, 2u, fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
               "pairBufferPushRejectsForReason matches self-pair");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPushRejectReasonName(
                               fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
                           "AtCapacity") == 0,
               "AtCapacity push reject reason has stable label");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity push reject reason");

    const fuse::physics::broadphase::PairBufferPushPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "push preflight carries reject reason on full buffer");
}

void testPairBufferCompactionRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compaction reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferCompactionRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
               "pairBufferCompactionRejectsForReason matches empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompaction(buffer),
               "canSkipPairBufferCompaction on empty buffer");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactionRejectReasonName(
                               fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
                           "AllValid") == 0,
               "AllValid compaction reject reason has stable label");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
             "all-valid slots report AllValid compaction reject reason");

    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(2u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "invalid slots report None compaction reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferCompaction(buffer),
               "canSkipPairBufferCompaction false when compaction is needed");

    const fuse::physics::broadphase::PairBufferCompactionPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferCompaction(buffer);
    expectTrue(preflight.needsCompaction(), "compaction preflight requests work when reason is None");
}

void testPairBufferClampRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer clamp reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferClampRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferClampRejectReason::EmptyBuffer),
               "pairBufferClampRejectsForReason matches empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferClamp(buffer),
               "canSkipPairBufferClamp on empty buffer");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferClampRejectReasonName(
                               fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
                           "WithinCapacity") == 0,
               "WithinCapacity clamp reject reason has stable label");

    buffer.push(0u, 1u);
    buffer.setMaxCapacity(2u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
             "within-capacity buffer reports WithinCapacity clamp reject reason");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(2u, 3u);
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(4u, 5u);
    overflowBuffer.setMaxCapacity(2u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferClampRejectReason(overflowBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::None),
             "overflow buffer reports None clamp reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferClamp(overflowBuffer),
               "canSkipPairBufferClamp false when clamp is needed");

    const fuse::physics::broadphase::PairBufferClampPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferClamp(overflowBuffer);
    expectTrue(preflight.needsClamp(), "clamp preflight requests work when reason is None");
}

void testShouldRunBroadphaseGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase false on empty scene");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase mirrors shouldRunBroadphase inversion");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration true on populated scene");
}

void testCellOccupancyIterationSkipGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::shouldIterateCellOccupancy(validRange, 8u),
               "shouldIterateCellOccupancy true within budget");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 8u),
               "canSkipCellOccupancyIteration false within budget");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(inverted, 4u),
               "canSkipCellOccupancyIteration true on empty range");
    expectTrue(!fuse::physics::broadphase::shouldIterateCellOccupancy(inverted, 4u),
               "shouldIterateCellOccupancy false on empty range");

    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "canSkipCellOccupancyIteration true over budget");

    const fuse::physics::broadphase::CellOccupancyPreflight preflight =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "cell occupancy preflight carries reject reason");
}

void testShouldRunRefineBroadphaseGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectTrue(!fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase false on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    expectTrue(fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase true with valid pairs");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase mirrors shouldRunRefineBroadphase inversion");
}

void testBroadphaseMergeRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "empty scene reports EmptyPlaneBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge false on empty scene");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseMergeRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
                           "EmptyDynamicBodies") == 0,
               "EmptyDynamicBodies merge reject reason has stable label");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectTrue(fuse::physics::broadphase::broadphaseMergeRejectsForReason(
                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
               "plane-only scene rejects for EmptyDynamicBodies");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge-ready scene reports None merge reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge true on merge-ready scene");

    const fuse::physics::broadphase::BroadphaseMergePreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge preflight carries reject reason");
}

void testPairBufferRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
             "self-pair reports InvalidPair push reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPushRejectReasonName(
                               fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
                           "AtCapacity") == 0,
               "AtCapacity push reject reason has stable label");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity push reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferPushRejectsForReason(
                   buffer, 2u, 3u, fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
               "pairBufferPushRejectsForReason matches AtCapacity");

    const fuse::physics::broadphase::PairBufferPushPreflight pushPreflight =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 3u);
    expectEq(static_cast<fuse::u32>(pushPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "push preflight carries reject reason");

    fuse::physics::broadphase::PairBufferSoA slotBuffer;
    slotBuffer.preparePairSlots(2u);
    slotBuffer.writeSlot(0u, 0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(slotBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "partial slot buffer reports None compaction reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompaction(slotBuffer),
               "shouldRunPairBufferCompaction true when invalid slots exist");

    slotBuffer.writeSlot(1u, 2u, 3u);
    expectTrue(fuse::physics::broadphase::pairBufferCompactionRejectsForReason(
                   slotBuffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
               "all-valid slots reject for AllValid compaction reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompaction(slotBuffer),
               "canSkipPairBufferCompaction on all-valid slots");

    fuse::physics::broadphase::PairBufferSoA clampBuffer;
    clampBuffer.push(2u, 3u);
    clampBuffer.push(0u, 1u);
    clampBuffer.setMaxCapacity(1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(clampBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::None),
             "overflow buffer reports None clamp reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferClamp(clampBuffer),
               "shouldRunPairBufferClamp true on overflow buffer");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferClamp(clampBuffer),
               "canSkipPairBufferClamp false on overflow buffer");

    fuse::physics::broadphase::PairBufferSoA dedupeBuffer;
    expectTrue(fuse::physics::broadphase::pairBufferDedupeRejectsForReason(
                   dedupeBuffer, fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
               "empty buffer rejects for EmptyBuffer dedupe reason");
    dedupeBuffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(dedupeBuffer),
               "canSkipPairBufferDedupe on single pair");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(dedupeBuffer),
               "shouldRunPairBufferDedupe false on single pair");

    dedupeBuffer.push(2u, 3u);
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(dedupeBuffer),
               "shouldRunPairBufferDedupe true for multiple pairs");

    fuse::physics::broadphase::PairBufferSoA sortBuffer;
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(sortBuffer),
               "canSkipPairBufferSort on empty buffer");
    sortBuffer.push(2u, 3u);
    sortBuffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(sortBuffer),
               "shouldRunPairBufferSort true for multiple pairs");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSortRejectReasonName(
                               fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair sort reject reason has stable label");
}

void testBroadphaseMergeRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "empty scene reports EmptyPlaneBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::broadphaseMergeRejectsForReason(
                   bodies, shapes,
                   fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
               "broadphaseMergeRejectsForReason matches empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseMergeRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
                           "EmptyDynamicBodies") == 0,
               "EmptyDynamicBodies merge reject reason has stable label");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies merge reject reason");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge-ready scene reports None merge reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge false when merge is viable");

    const fuse::physics::broadphase::BroadphaseMergePreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "preflightBroadphaseMerge carries merge reject reason");
}

void testCellOccupancyCanSkipIterationGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 8u),
               "valid range does not skip occupancy iteration");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(inverted, 4u),
               "empty range skips occupancy iteration");
    expectTrue(fuse::physics::broadphase::cellOccupancyRejectsForReason(
                   inverted, 4u, fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
               "empty range rejects for EmptyRange");

    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "over-budget range skips occupancy iteration");

    const fuse::physics::broadphase::CellOccupancyPreflight preflight =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "preflightCellOccupancy carries occupancy reject reason");
}

void testPairBufferRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
             "self-pair reports InvalidPair push reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferPushRejectsForReason(
                   buffer, 2u, 2u, fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
               "pairBufferPushRejectsForReason matches self-pair");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity push reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPushRejectReasonName(
                               fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
                           "AtCapacity") == 0,
               "AtCapacity push reject reason has stable label");

    const fuse::physics::broadphase::PairBufferPushPreflight pushPreflight =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 3u);
    expectTrue(!pushPreflight.canPush(), "push preflight rejects at-capacity pair");
    expectEq(static_cast<fuse::u32>(pushPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "push preflight carries reject reason");

    fuse::physics::broadphase::PairBufferSoA slotBuffer;
    slotBuffer.preparePairSlots(2u);
    slotBuffer.writeSlot(0u, 0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(slotBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "invalid slot reports None compaction reject reason");
    expectTrue(fuse::physics::broadphase::preflightPairBufferCompaction(slotBuffer).needsCompaction(),
               "invalid slot compaction preflight needs work");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferCompaction(slotBuffer),
               "canSkipPairBufferCompaction false when compaction is needed");

    slotBuffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(slotBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
             "all-valid slots report AllValid compaction reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompaction(slotBuffer),
               "canSkipPairBufferCompaction on all-valid slots");

    fuse::physics::broadphase::PairBufferSoA clampBuffer;
    clampBuffer.push(2u, 3u);
    clampBuffer.push(0u, 1u);
    clampBuffer.setMaxCapacity(1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferClampRejectReason(clampBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::None),
             "overflow buffer reports None clamp reject reason");
    expectTrue(fuse::physics::broadphase::preflightPairBufferClamp(clampBuffer).needsClamp(),
               "overflow clamp preflight needs work");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferClamp(clampBuffer),
               "canSkipPairBufferClamp false when clamp is needed");

    clampBuffer.applyMaxCapacityClamp();
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferClampRejectReason(clampBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
             "within-capacity buffer reports WithinCapacity clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferClamp(clampBuffer),
               "canSkipPairBufferClamp on within-capacity buffer");

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
             "single pair reports SinglePair dedupe reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferDedupeRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
               "pairBufferDedupeRejectsForReason matches single pair");

    fuse::physics::broadphase::PairBufferSoA dedupeBuffer;
    dedupeBuffer.push(0u, 1u);
    dedupeBuffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferDedupeRejectReason(dedupeBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::None),
             "multiple pairs report None dedupe reject reason");
}

void testPairBufferCompactAndClampPreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight emptyPreflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(buffer);
    expectTrue(emptyPreflight.canSkipAll(), "empty buffer skips compact-and-clamp");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs on empty buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight compactionOnly =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(buffer);
    expectTrue(!compactionOnly.canSkipAll(), "invalid slot buffer needs compact-and-clamp work");
    expectTrue(compactionOnly.compactionNeeded, "invalid slot buffer needs compaction");
    expectTrue(!compactionOnly.clampNeeded, "invalid slot buffer does not need clamp");

    buffer.setMaxCapacity(1u);
    buffer.writeSlot(1u, 2u, 3u);
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight bothNeeded =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(buffer);
    expectTrue(bothNeeded.clampNeeded, "two-slot buffer over max capacity needs clamp after compact");
    expectEq(buffer.compactAndClamp(), 1u, "compactAndClamp compacts then clamps overflow slots");
    expectEq(buffer.activeCount, 1u, "compactAndClamp active count after clamp");
}

void testCellOccupancyPreflightReasonGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectEq(static_cast<fuse::u32>(withinBudget.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "within-budget preflight carries None reason");
    expectTrue(withinBudget.canIterate(), "within-budget preflight can iterate");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 8u),
               "canSkipCellOccupancyIteration false within budget");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectEq(static_cast<fuse::u32>(overBudget.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "over-budget preflight carries ExceedsBudget reason");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "canSkipCellOccupancyIteration true over budget");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight emptyRange =
        fuse::physics::broadphase::preflightCellOccupancy(inverted, 4u);
    expectEq(static_cast<fuse::u32>(emptyRange.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
             "inverted range preflight carries EmptyRange reason");
}

void testBroadphaseMergeRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "empty scene reports EmptyPlaneBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge false on empty scene");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseMergeRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
                           "EmptyDynamicBodies") == 0,
               "EmptyDynamicBodies merge reject reason has stable label");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::broadphaseMergeRejectsForReason(
                   bodies, shapes,
                   fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
               "broadphaseMergeRejectsForReason matches plane-only scene");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge-ready scene reports None merge reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge true when merge is viable");
}

void testPairBufferPushRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
             "self-pair reports InvalidPair push reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferPushRejectsForReason(
                   buffer, 1u, 1u, fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
               "pairBufferPushRejectsForReason matches self-pair");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPushRejectReasonName(
                               fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
                           "AtCapacity") == 0,
               "AtCapacity push reject reason has stable label");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity push reject reason");

    const fuse::physics::broadphase::PairBufferPushPreflight validPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u);
    expectEq(static_cast<fuse::u32>(validPush.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "push preflight carries AtCapacity reason on full buffer");
}

void testPairBufferCompactionRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compaction reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferCompactionRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
               "pairBufferCompactionRejectsForReason matches empty buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
             "all-valid slots report AllValid compaction reject reason");

    fuse::physics::broadphase::PairBufferSoA sparseBuffer;
    sparseBuffer.preparePairSlots(2u);
    sparseBuffer.writeSlot(0u, 0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(sparseBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "sparse slots report None compaction reject reason");
    expectTrue(fuse::physics::broadphase::preflightPairBufferCompaction(sparseBuffer).needsCompaction(),
               "sparse slots compaction preflight needs work");
}

void testPairBufferClampRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer clamp reject reason");

    buffer.push(0u, 1u);
    buffer.setMaxCapacity(2u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
             "within-capacity buffer reports WithinCapacity clamp reject reason");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(2u, 3u);
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(4u, 5u);
    overflowBuffer.setMaxCapacity(2u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(overflowBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::None),
             "overflow buffer reports None clamp reject reason");
    expectTrue(fuse::physics::broadphase::preflightPairBufferClamp(overflowBuffer).needsClamp(),
               "overflow clamp preflight needs truncation");
}

void testPairBufferDedupeSortRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer dedupe reject reason");
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer sort reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe false on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort false on empty buffer");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
             "single pair reports SinglePair dedupe reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSortRejectReasonName(
                               fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair sort reject reason has stable label");

    buffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::None),
             "multiple pairs report None dedupe reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe true for multiple pairs");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort true for multiple pairs");
}

void testShouldRunBroadphaseAndRefineGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase true on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");

    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase true with valid pair buffer");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase false when refine is viable");
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
    testEmptyPairGuards();
    testCellClampHelpers();
    testBroadphaseCellSpanClampIntegration();
    testPairBufferSoAIterationEarlyOuts();
    testCandidatePairRejectReasonGuards();
    testCandidatePairRejectReasonName();
    testRejectedCandidatePairGuards();
    testCanSkipBroadphaseEmptySetGuard();
    testCellSpanExceedsClampGuards();
    testCellOccupancyBudgetGuards();
    testPairBufferLastRejectReasonGuards();
    testPairBufferRefineAndInvalidSlotGuards();
    testCandidatePairAabbRejectReason();
    testCellRangeFromAabbHelpers();
    testPairBufferCapacityGuards();
    testPairBufferPreparePairSlotsZeroGuard();
    testNormalizeSpatialHashParamsAndOccupancy();
    testBroadphaseNormalizedParamsGuard();
    testPairBufferCompactionEarlyOuts();
    testEmptyBroadphaseInputGuards();
    testCandidatePairRejectsForReasonGuards();
    testCellOccupancyBudgetGuards();
    testEstimatePairCountForUniqueBodies();
    testPairBufferCanAcceptPairsGuard();
    testPairBufferSlotValidityBounds();
    testPairBufferSlotModeDedupeGuard();
    testPairBufferCompactAndClampZeroGuard();
    testCellOccupancyPreflightGuards();
    testShouldSkipShapeCellInsertionGuards();
    testBroadphaseRefinePreflightGuards();
    testBroadphaseCanSkipIntegration();
    testCellOccupancyRejectReasonAndPreflight();
    testBroadphasePreflightGuards();
    testRefineBroadphasePreflightGuards();
    testDedupeBroadphasePreflightGuards();
    testPairBufferPreflightGuards();
    testPairBufferInvalidateInvalidPairs();
    testPairBufferWriteSlotBodyCountGuard();
    testCanSkipBroadphaseGuards();
    testPruneInvalidCandidatePairs();
    testPairBufferWouldRejectPush();
    testBroadphaseCellOccupancyBudgetIntegration();
    testPairBufferSlotValidityBounds();
    testPairBufferSlotModeDedupeGuard();
    testPairBufferWouldRejectAdditionalPairs();
    testCanSkipBroadphaseRefineGuard();
    testMaxCellSpanAxisGuards();
    testCellOccupancyPreflightGuards();
    testPerShapeCellBudgetGuard();
    testPairSlotPreflightGuards();
    testPairBufferDedupeAndCompactGuards();
    testCellCapacityPreflightGuards();
    testBroadphaseInputPreflightGuards();
    testBroadphaseRefinePreflightGuards();
    testBroadphaseDedupePreflightGuards();
    testPairBufferInvalidSlotGuards();
    testEmptyCellBucketGuards();
    testPairBufferDedupePreflightGuards();
    testPairBufferCanSkipRefineGuard();
    testPairBufferCanSkipCompactAndClamp();
    testBroadphaseBoxShapeCellRange();
    testBroadphaseSingletonEarlyOut();
    testBroadphaseCellOccupancyBudgetIntegration();
    testPairBufferReserveForUniqueBodies();
    testPairBufferCanSkipMaxCapacityClamp();
    testBroadphaseRejectReasonGuards();
    testRefineBroadphaseRejectReasonGuards();
    testDedupeBroadphaseRejectReasonGuards();
    testCellOccupancyRejectsForReasonGuards();
    testPairBufferDedupeAndSortPreflightGuards();
    testPairBufferSoADedupePassGuards();
    testPairBufferPushRejectReasonGuards();
    testPairBufferCompactionRejectReasonGuards();
    testPairBufferClampRejectReasonGuards();
    testBroadphaseMergeRejectReasonGuards();
    testBroadphaseMergePreflightGuards();
    testPairBufferPushRejectReasonGuards();
    testPairBufferCompactionRejectReasonGuards();
    testPairBufferClampRejectReasonGuards();
    testPairBufferDedupeRejectReasonGuards();
    testCellOccupancyIterationSkipGuards();
    testRefineBroadphaseShouldRunGuards();
    testBroadphaseMergeRejectReasonGuards();
    testPairBufferSortRejectReasonGuards();
    testPairBufferCompactAndClampPreflightGuards();
    testShouldRunBroadphaseGuards();
    testMergePairsIntoBufferPreflightGuards();
    testPairBufferShouldRunDedupeGuards();
    testPairBufferWriteSlotRejectReasonGuards();
    testPairBufferInvalidateSlotRejectReasonGuards();
    testCellPairGenRejectReasonGuards();
    testShapeCellInsertRejectReasonGuards();
    testBroadphaseCellPairGenRejectReasonGuards();
    testPairBufferToVectorRejectReasonGuards();
    testCellSpanRejectReasonAndPreflight();
    testRefineDedupeMergeWithPreflightGuards();
    testPairBufferWouldSkipWriteSlotGuards();
    testCellCapacityWouldSkipGuards();
    testRefineDedupeMergeWouldSkipGuards();
    testCandidateRejectReasonGuards();
    testPairBufferRejectReasonTracking();
    testCellSpanClampDiagnostics();
    testCellRangeVolumeAndClampCellSize();
    testPairBufferIsFullAndSetMaxCapacityTrim();
    testPairBufferCompactAllInvalidEarlyOut();
    testPairBufferCompactAlreadyPacked();
    testPairBufferSortCanonicalEarlyOut();
    testBroadphase2DCellSpanClampIntegration();
    testEmptySetGuards();
    testCellOccupancyBudgetGuards();
    testPairListGuards();
    testPairBufferInvalidPairPrune();
    testBroadphaseOccupancyBudgetIntegration();
    testBroadphaseSkipInputGuards();
    testPairBufferInvalidateInvalidPairs();
    testCandidatePairRejectReasonWithRefine();
    testPairBufferSetMaxCapacityTrim();
    testMaxCellOccupancyBudgetHelpers();
    testShapeCellOccupancyPreflight();
    testBroadphasePreflightGuards();
    testBroadphaseRefinePreflightGuards();
    testPairBufferDedupePreflights();
    testBroadphaseInputPreflight();
    testCellOccupancyPreflight();
    testBroadphaseCellOccupancyIntegration();
    testRefineBroadphasePreflight();
    testPairBufferDedupePreflight();
    testPairBufferClampPreflight();
    testPairBufferDedupeAndClampGuards();
    testEmptyBroadphaseOutputGuard();
    testCellOccupancyPreflightGuards();
    testBroadphaseDispatchPreflightGuards();
    testCanSkipRefineBroadphaseIntegration();
    testPairBufferCanSkipCompactAndClamp();
    testPairBufferSlotValidityBounds();
    testPairBufferSparseCanonicalSort();
    testBroadphaseInputPreflightGuards();
    testPairBufferPreflightGuards();
    testRefineBroadphasePreflightGuards();
    testPairBufferCompactionPreflightGuards();
    testDedupeBroadphasePreflightGuards();
    testPairBufferSortCanonicalIfNeeded();
    testCellOccupancyPreflightReasonGuards();
    testPairBufferDedupeSortRejectReasonGuards();
    testShouldRunBroadphaseAndRefineGuards();
    testCellOccupancyPreflightReasonField();
    testShouldRunRefineBroadphaseGuards();
    testPairBufferRejectReasonGuards();
    testCellOccupancyCanSkipIterationGuards();

    if (g_failures == 0) {
        std::printf("fuse_physics_broadphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_broadphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
