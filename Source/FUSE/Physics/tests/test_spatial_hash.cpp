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
    buffer.push(2u, 3u);
    const fuse::physics::broadphase::DedupeBroadphasePreflight uniquePreflight =
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectTrue(uniquePreflight.alreadyUnique, "dedupe preflight marks already-unique pairs");
    expectTrue(!uniquePreflight.canDedupe(), "dedupe preflight skips already-unique pairs");

    buffer.push(0u, 1u);
    expectTrue(multiPreflight.canDedupe(), "dedupe preflight accepts duplicate pairs");
               "shouldRunDedupeBroadphase true for duplicate pairs");
}

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
    buffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::dedupeBroadphaseRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::AlreadyUnique),
             "unique multiple pairs report AlreadyUnique dedupe reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase false for already-unique pairs");

    buffer.push(0u, 1u);
             "duplicate pairs report None dedupe reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase true for duplicate pairs");
    expectTrue(!fuse::physics::broadphase::canSkipDedupeBroadphase(buffer),
               "canSkipDedupeBroadphase false for duplicate pairs");

    const fuse::physics::broadphase::DedupeBroadphasePreflight preflight =
    expectTrue(preflight.canDedupe(), "dedupe preflight accepts multiple pairs with reason None");
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectTrue(preflight.canDedupe(), "dedupe preflight accepts duplicate pairs with reason None");
}

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
    buffer.push(2u, 3u);
    const fuse::physics::broadphase::PairBufferDedupePreflight uniqueDedupe =
        fuse::physics::broadphase::preflightPairBufferDedupe(buffer);
    expectTrue(uniqueDedupe.alreadyUnique, "multi-pair unique dedupe preflight marks already unique");
    expectTrue(!uniqueDedupe.canDedupe(), "multi-pair unique dedupe preflight cannot dedupe");

    buffer.push(0u, 1u);
    expectTrue(multiDedupe.canDedupe(), "multi-pair duplicate dedupe preflight can dedupe");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe false for duplicate pairs");

    fuse::physics::broadphase::PairBufferSoA unsortedBuffer;
    unsortedBuffer.push(2u, 3u);
    unsortedBuffer.push(0u, 1u);
    const fuse::physics::broadphase::PairBufferSortPreflight multiSort =
    expectTrue(multiSort.needsSort(), "multi-pair sort preflight needs sort");

    buffer.sortCanonical();
    expectTrue(buffer.isSortedCanonical(), "sortCanonical leaves canonical order via preflight gate");
        fuse::physics::broadphase::preflightPairBufferSort(unsortedBuffer);

    unsortedBuffer.sortCanonical();
    expectTrue(unsortedBuffer.isSortedCanonical(), "sortCanonical leaves canonical order via preflight gate");
}

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
             "plane plus dynamic scene reports None merge reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge false when merge is viable");

    const fuse::physics::broadphase::BroadphaseMergePreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "preflightBroadphaseMerge carries reject reason");
    expectTrue(preflight.canMerge(), "merge preflight accepts plane plus dynamic scene");
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

    const fuse::physics::broadphase::PairBufferPushPreflight validPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u);
    expectEq(static_cast<fuse::u32>(validPush.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::None),
             "valid push preflight carries None reject reason");
    expectTrue(validPush.canPush(), "valid push preflight accepts pair under capacity");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity push reject reason");
}

void testPairBufferCompactionClampRejectReasonGuards() {
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

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
             "all-valid slots report AllValid compaction reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactionRejectReasonName(
                               fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
                           "AllValid") == 0,
               "AllValid compaction reject reason has stable label");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    const fuse::physics::broadphase::PairBufferCompactionPreflight compactionPreflight =
        fuse::physics::broadphase::preflightPairBufferCompaction(buffer);
    expectTrue(compactionPreflight.needsCompaction(), "compaction preflight requests invalid slot work");
    expectEq(static_cast<fuse::u32>(compactionPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "compaction preflight carries None when work is needed");

    fuse::physics::broadphase::PairBufferSoA clampBuffer;
    clampBuffer.push(2u, 3u);
    clampBuffer.push(0u, 1u);
    clampBuffer.setMaxCapacity(1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferClampRejectReason(clampBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::None),
             "overflow buffer reports None clamp reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferClamp(clampBuffer),
               "canSkipPairBufferClamp false when clamp is needed");

    clampBuffer.setMaxCapacity(4u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferClampRejectReason(clampBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
             "within-capacity buffer reports WithinCapacity clamp reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferClampRejectsForReason(
                   clampBuffer, fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
               "pairBufferClampRejectsForReason matches within-capacity buffer");
}

void testPairBufferSkipPredicateGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort on empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe on empty buffer");

    buffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort on single pair");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe on single pair");

    buffer.push(2u, 1u);
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort false for multiple pairs");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe false for multiple pairs");
}

void testCellOccupancyPreflightReasonField() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight validPreflight =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectEq(static_cast<fuse::u32>(validPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "valid range preflight carries None reject reason");
    expectTrue(validPreflight.canIterate(), "valid range preflight can iterate");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validPreflight),
               "canSkipCellOccupancyIteration false for valid range");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight emptyPreflight =
        fuse::physics::broadphase::preflightCellOccupancy(inverted, 4u);
    expectEq(static_cast<fuse::u32>(emptyPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
             "inverted range preflight carries EmptyRange reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(emptyPreflight),
               "canSkipCellOccupancyIteration on empty range");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudgetPreflight =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectEq(static_cast<fuse::u32>(overBudgetPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "over-budget range preflight carries ExceedsBudget reject reason");
    expectTrue(overBudgetPreflight.exceedsBudget, "over-budget preflight marks exceedsBudget");
}

void testShouldRunRefineBroadphaseGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectTrue(!fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase mirrors shouldRunRefineBroadphase inversion");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    expectTrue(fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase true when refine is viable");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase false when shouldRunRefineBroadphase true");
}

void testCellSpanClampPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 wideRange = {
        {-100, -100, -100},
        {100, 100, 100},
    };
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(wideRange, 8u),
               "wide range exceeds per-axis span clamp");
    expectTrue(!fuse::physics::broadphase::cellSpanWithinClamp(wideRange, 8u),
               "wide range is outside span clamp");
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::cellSpanRejectReason(wideRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanClamp),
             "wide range reports ExceedsSpanClamp reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellSpanRejectReasonName(
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanClamp),
                           "ExceedsSpanClamp") == 0,
               "ExceedsSpanClamp span reject reason has stable label");

    const fuse::physics::broadphase::CellRange3 smallRange = {{0, 0, 0}, {3, 3, 3}};
    expectTrue(fuse::physics::broadphase::cellSpanWithinClamp(smallRange, 8u),
               "small range is within span budget");
    const fuse::physics::broadphase::CellSpanPreflight spanPreflight =
        fuse::physics::broadphase::preflightCellSpanClamp(smallRange, 8u);
    expectTrue(spanPreflight.canIterate(), "small span preflight accepts range");
    expectTrue(!spanPreflight.exceedsSpanClamp, "small span preflight does not exceed clamp");
    expectEq(spanPreflight.spanPerAxis.x, 4, "span preflight reports per-axis span");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::cellSpanRejectsForReason(
                   inverted, 4u, fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
               "inverted range rejects for EmptyRange span reason");
}

void testPairBufferWriteSlotAndPreparePreflights() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight validWrite =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 0u, 1u);
    expectTrue(validWrite.canWrite(), "write-slot preflight accepts valid slot");
    buffer.writeSlot(0u, 0u, 1u);

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight invalidPair =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 1u, 2u, 2u);
    expectTrue(invalidPair.invalidPair, "write-slot preflight marks self-pair");
    expectTrue(!invalidPair.canWrite(), "write-slot preflight rejects self-pair");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight outOfRange =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 4u, 0u, 2u);
    expectTrue(outOfRange.outOfRangeSlot, "write-slot preflight marks out-of-range slot");

    const fuse::physics::broadphase::PairBufferPrepareSlotsPreflight zeroSlots =
        fuse::physics::broadphase::preflightPairBufferPrepareSlots(0u);
    expectTrue(!zeroSlots.canPrepare(), "prepare-slots preflight rejects zero slots");

    fuse::physics::broadphase::PairBufferSoA zeroBuffer;
    zeroBuffer.preparePairSlots(0u);
    expectTrue(zeroBuffer.canSkipSoAIteration(), "preparePairSlots(0) via preflight leaves empty buffer");
}

void testPairBufferMergeAndDuplicateGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);
    buffer.push(0u, 1u);

    const fuse::physics::broadphase::PairBufferMergePreflight atCapacity =
        fuse::physics::broadphase::preflightPairBufferMerge(buffer, 2u);
    expectTrue(atCapacity.atCapacity, "merge preflight marks full buffer");
    expectTrue(!atCapacity.canMergeAny(), "merge preflight rejects when buffer is full");
    expectEq(atCapacity.rejectedCount, 2u, "merge preflight rejects all incoming pairs at capacity");

    fuse::physics::broadphase::PairBufferSoA openBuffer;
    const fuse::physics::broadphase::PairBufferMergePreflight partialAccept =
        fuse::physics::broadphase::preflightPairBufferMerge(openBuffer, 3u);
    expectEq(partialAccept.acceptedCount, 3u, "open buffer merge preflight accepts all incoming pairs");

    openBuffer.push(0u, 1u);
    openBuffer.push(0u, 1u);
    expectTrue(openBuffer.hasDuplicateCanonicalPairs(), "buffer detects duplicate canonical pairs");
    expectTrue(fuse::physics::broadphase::bufferHasDuplicateCanonicalPairs(openBuffer),
               "bufferHasDuplicateCanonicalPairs matches SoA helper");
    expectTrue(fuse::physics::broadphase::dedupeBroadphaseWouldReduceCount(openBuffer),
               "dedupeBroadphaseWouldReduceCount true when duplicates exist");
    expectTrue(!openBuffer.canSkipPairBufferSort(), "multi-pair buffer needs canonical sort");

    fuse::physics::broadphase::PairBufferSoA singleBuffer;
    singleBuffer.push(0u, 1u);
    expectTrue(singleBuffer.canSkipPairBufferSort(), "single-pair buffer skips canonical sort");
}

void testRefineDedupeBroadphasePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightRefineDedupeBroadphase(bodies, shapes, buffer);
    expectTrue(!emptyPreflight.canRefine(), "combined preflight cannot refine empty scene");
    expectTrue(!emptyPreflight.needsDedupe(), "combined preflight does not need dedupe when empty");
    expectTrue(fuse::physics::broadphase::canSkipRefineDedupeBroadphase(bodies, shapes, buffer),
               "canSkipRefineDedupeBroadphase on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);
    buffer.push(0u, 1u);

    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight duplicatePreflight =
        fuse::physics::broadphase::preflightRefineDedupeBroadphase(bodies, shapes, buffer);
    expectTrue(duplicatePreflight.canRefine(), "combined preflight can refine valid scene");
    expectTrue(duplicatePreflight.hasDuplicatePairs, "combined preflight detects duplicate pairs");
    expectTrue(duplicatePreflight.needsDedupe(), "combined preflight needs dedupe when duplicates exist");
    expectTrue(!fuse::physics::broadphase::canSkipRefineDedupeBroadphase(bodies, shapes, buffer),
               "canSkipRefineDedupeBroadphase false when refine or dedupe is needed");
}

void testBroadphaseMergeRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::NoPlaneBodies),
             "empty scene reports NoPlaneBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseMergeRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::NoDynamicBodies),
                           "NoDynamicBodies") == 0,
               "NoDynamicBodies merge reject reason has stable label");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectTrue(fuse::physics::broadphase::broadphaseMergeRejectsForReason(
                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::NoDynamicBodies),
               "plane-only scene rejects for NoDynamicBodies");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight mergePreflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectTrue(mergePreflight.canMerge(), "merge preflight accepts plane plus dynamic scene");
    expectEq(mergePreflight.estimatedMergePairs, 1u, "merge preflight estimates dynamic-plane pair count");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge false when merge is viable");

    fuse::physics::broadphase::PairBufferSoA buffer;
    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight mergeIntoBuffer =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer, 1u);
    expectTrue(mergeIntoBuffer.canMergeAny(), "merge-into-buffer preflight accepts open buffer");
    expectEq(mergeIntoBuffer.incomingPairCount, 1u, "merge-into-buffer preflight carries incoming count");
}

void testCellSpanClampPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 wideRange = {
        {-100, -100, -100},
        {100, 100, 100},
    };
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(wideRange, 8u),
               "wide range exceeds per-axis span clamp");
    expectTrue(!fuse::physics::broadphase::cellSpanWithinClamp(wideRange, 8u),
               "wide range is outside span clamp");
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::cellSpanRejectReason(wideRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanClamp),
             "wide range reports ExceedsSpanClamp reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellSpanRejectReasonName(
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanClamp),
                           "ExceedsSpanClamp") == 0,
               "ExceedsSpanClamp span reject reason has stable label");

    const fuse::physics::broadphase::CellRange3 smallRange = {{0, 0, 0}, {3, 3, 3}};
    expectTrue(fuse::physics::broadphase::cellSpanWithinClamp(smallRange, 8u),
               "small range is within span budget");
    const fuse::physics::broadphase::CellSpanPreflight spanPreflight =
        fuse::physics::broadphase::preflightCellSpanClamp(smallRange, 8u);
    expectTrue(spanPreflight.canIterate(), "small span preflight accepts range");
    expectTrue(!spanPreflight.exceedsSpanClamp, "small span preflight does not exceed clamp");
    expectEq(spanPreflight.spanPerAxis.x, 4, "span preflight reports per-axis span");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::cellSpanRejectsForReason(
                   inverted, 4u, fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
               "inverted range rejects for EmptyRange span reason");
}

void testPairBufferWriteSlotAndPreparePreflights() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight validWrite =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 0u, 1u);
    expectTrue(validWrite.canWrite(), "write-slot preflight accepts valid slot");
    buffer.writeSlot(0u, 0u, 1u);

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight invalidPair =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 1u, 2u, 2u);
    expectTrue(invalidPair.invalidPair, "write-slot preflight marks self-pair");
    expectTrue(!invalidPair.canWrite(), "write-slot preflight rejects self-pair");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight outOfRange =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 4u, 0u, 2u);
    expectTrue(outOfRange.outOfRangeSlot, "write-slot preflight marks out-of-range slot");

    const fuse::physics::broadphase::PairBufferPrepareSlotsPreflight zeroSlots =
        fuse::physics::broadphase::preflightPairBufferPrepareSlots(0u);
    expectTrue(!zeroSlots.canPrepare(), "prepare-slots preflight rejects zero slots");

    fuse::physics::broadphase::PairBufferSoA zeroBuffer;
    zeroBuffer.preparePairSlots(0u);
    expectTrue(zeroBuffer.canSkipSoAIteration(), "preparePairSlots(0) via preflight leaves empty buffer");
}

void testPairBufferMergeAndDuplicateGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);
    buffer.push(0u, 1u);

    const fuse::physics::broadphase::PairBufferMergePreflight atCapacity =
        fuse::physics::broadphase::preflightPairBufferMerge(buffer, 2u);
    expectTrue(atCapacity.atCapacity, "merge preflight marks full buffer");
    expectTrue(!atCapacity.canMergeAny(), "merge preflight rejects when buffer is full");
    expectEq(atCapacity.rejectedCount, 2u, "merge preflight rejects all incoming pairs at capacity");

    fuse::physics::broadphase::PairBufferSoA openBuffer;
    const fuse::physics::broadphase::PairBufferMergePreflight partialAccept =
        fuse::physics::broadphase::preflightPairBufferMerge(openBuffer, 3u);
    expectEq(partialAccept.acceptedCount, 3u, "open buffer merge preflight accepts all incoming pairs");

    openBuffer.push(0u, 1u);
    openBuffer.push(0u, 1u);
    expectTrue(openBuffer.hasDuplicateCanonicalPairs(), "buffer detects duplicate canonical pairs");
    expectTrue(fuse::physics::broadphase::bufferHasDuplicateCanonicalPairs(openBuffer),
               "bufferHasDuplicateCanonicalPairs matches SoA helper");
    expectTrue(fuse::physics::broadphase::dedupeBroadphaseWouldReduceCount(openBuffer),
               "dedupeBroadphaseWouldReduceCount true when duplicates exist");
    expectTrue(!openBuffer.canSkipPairBufferSort(), "multi-pair buffer needs canonical sort");

    fuse::physics::broadphase::PairBufferSoA singleBuffer;
    singleBuffer.push(0u, 1u);
    expectTrue(singleBuffer.canSkipPairBufferSort(), "single-pair buffer skips canonical sort");
}

void testRefineDedupeBroadphasePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightRefineDedupeBroadphase(bodies, shapes, buffer);
    expectTrue(!emptyPreflight.canRefine(), "combined preflight cannot refine empty scene");
    expectTrue(!emptyPreflight.needsDedupe(), "combined preflight does not need dedupe when empty");
    expectTrue(fuse::physics::broadphase::canSkipRefineDedupeBroadphase(bodies, shapes, buffer),
               "canSkipRefineDedupeBroadphase on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);
    buffer.push(0u, 1u);

    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight duplicatePreflight =
        fuse::physics::broadphase::preflightRefineDedupeBroadphase(bodies, shapes, buffer);
    expectTrue(duplicatePreflight.canRefine(), "combined preflight can refine valid scene");
    expectTrue(duplicatePreflight.hasDuplicatePairs, "combined preflight detects duplicate pairs");
    expectTrue(duplicatePreflight.needsDedupe(), "combined preflight needs dedupe when duplicates exist");
    expectTrue(!fuse::physics::broadphase::canSkipRefineDedupeBroadphase(bodies, shapes, buffer),
               "canSkipRefineDedupeBroadphase false when refine or dedupe is needed");
}

void testBroadphaseMergeRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::NoPlaneBodies),
             "empty scene reports NoPlaneBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseMergeRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::NoDynamicBodies),
                           "NoDynamicBodies") == 0,
               "NoDynamicBodies merge reject reason has stable label");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectTrue(fuse::physics::broadphase::broadphaseMergeRejectsForReason(
                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::NoDynamicBodies),
               "plane-only scene rejects for NoDynamicBodies");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight mergePreflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectTrue(mergePreflight.canMerge(), "merge preflight accepts plane plus dynamic scene");
    expectEq(mergePreflight.estimatedMergePairs, 1u, "merge preflight estimates dynamic-plane pair count");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge false when merge is viable");

    fuse::physics::broadphase::PairBufferSoA buffer;
    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight mergeIntoBuffer =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer, 1u);
    expectTrue(mergeIntoBuffer.canMergeAny(), "merge-into-buffer preflight accepts open buffer");
    expectEq(mergeIntoBuffer.incomingPairCount, 1u, "merge-into-buffer preflight carries incoming count");
}

void testCellOccupancyPreflightReasonField() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectEq(static_cast<fuse::u32>(withinBudget.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "cell occupancy preflight carries None reason within budget");
    expectTrue(withinBudget.canIterate(), "cell occupancy preflight can iterate within budget");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 8u),
               "canSkipCellOccupancyIteration false within budget");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectEq(static_cast<fuse::u32>(overBudget.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "cell occupancy preflight carries ExceedsBudget reason");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "canSkipCellOccupancyIteration true over budget");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(inverted, 4u),
               "canSkipCellOccupancyIteration true for empty range");
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
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseMergeRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
                           "EmptyDynamicBodies") == 0,
               "EmptyDynamicBodies merge reject reason has stable label");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");

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
             "plane plus dynamic scene reports None merge reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge false when merge is viable");

    const fuse::physics::broadphase::BroadphaseMergePreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge preflight carries reject reason");
    expectTrue(preflight.canMerge(), "merge preflight can merge on viable scene");
}

void testRefineBroadphaseShouldRunGuard() {
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
               "shouldRunRefineBroadphase true when refine is viable");
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
    expectTrue(fuse::physics::broadphase::canSkipPairBufferPush(buffer, 2u, 2u),
               "canSkipPairBufferPush on self-pair");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity push reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferPush(buffer, 2u, 3u),
               "canSkipPairBufferPush on full buffer");

    const fuse::physics::broadphase::PairBufferPushPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 3u);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "push preflight carries reject reason");
}

void testPairBufferCompactionRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compaction reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompaction(buffer),
               "canSkipPairBufferCompaction on empty buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "sparse slots report None compaction reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferCompaction(buffer),
               "canSkipPairBufferCompaction false when compaction needed");

    buffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
             "all-valid slots report AllValid compaction reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactionRejectReasonName(
                               fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
                           "AllValid") == 0,
               "AllValid compaction reject reason has stable label");
}

void testPairBufferClampRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferClamp(buffer),
               "canSkipPairBufferClamp on empty buffer");

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
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferClamp(overflowBuffer),
               "canSkipPairBufferClamp false when clamp needed");
    expectTrue(fuse::physics::broadphase::pairBufferClampRejectsForReason(
                   overflowBuffer, fuse::physics::broadphase::PairBufferClampRejectReason::None),
               "pairBufferClampRejectsForReason matches overflow buffer");
}

void testPairBufferDedupeRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer SoA dedupe reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferDedupeRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
               "pairBufferDedupeRejectsForReason matches empty buffer");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
             "single pair reports SinglePair SoA dedupe reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferDedupeRejectReasonName(
                               fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair SoA dedupe reject reason has stable label");

    buffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::AlreadyUnique),
             "unique multiple pairs report AlreadyUnique SoA dedupe reject reason");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::None),
             "duplicate pairs report None SoA dedupe reject reason");
    const fuse::physics::broadphase::PairBufferDedupePreflight preflight =
        fuse::physics::broadphase::preflightPairBufferDedupe(buffer);
    expectTrue(preflight.canDedupe(), "SoA dedupe preflight accepts duplicate pairs with reason None");
}

void testPairBufferSortRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer sort reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort on empty buffer");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
             "single pair reports SinglePair sort reject reason");

    buffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::None),
             "multiple pairs report None sort reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort false when sort needed");
    expectTrue(fuse::physics::broadphase::pairBufferSortRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::None),
               "pairBufferSortRejectsForReason matches multi-pair buffer");
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
             "merge preflight carries reject reason");
}

void testPairBufferCompactAndClampPreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight emptyPreflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "empty buffer compact+clamp preflight marks empty");
    expectTrue(!emptyPreflight.needsWork(), "empty buffer compact+clamp preflight needs no work");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompactAndClamp(buffer),
               "canSkipPairBufferCompactAndClamp on empty buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight compactionPreflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(buffer);
    expectTrue(compactionPreflight.needsCompaction, "invalid slot requests compaction work");
    expectTrue(compactionPreflight.needsWork(), "invalid slot compact+clamp preflight needs work");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferCompactAndClamp(buffer),
               "non-empty buffer cannot skip compact+clamp entirely");

    buffer.writeSlot(1u, 2u, 3u);
    expectEq(buffer.compact(), 2u, "slot compaction gathers valid pairs before clamp preflight");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(2u, 3u);
    overflowBuffer.setMaxCapacity(1u);
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight clampPreflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(overflowBuffer);
    expectTrue(clampPreflight.needsClamp, "overflow buffer compact+clamp preflight needs clamp");
    expectEq(overflowBuffer.compactAndClamp(), 1u, "compactAndClamp honors composite preflight gates");
}

void testPairBufferSkipHelperGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompaction(buffer),
               "canSkipPairBufferCompaction on empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferClamp(buffer),
               "canSkipPairBufferClamp on empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort on empty buffer");

    fuse::physics::broadphase::PairBufferSoA slotBuffer;
    slotBuffer.preparePairSlots(2u);
    slotBuffer.writeSlot(0u, 0u, 1u);
    slotBuffer.writeSlot(1u, 2u, 3u);
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompaction(slotBuffer),
               "all-valid slots skip compaction helper");

    fuse::physics::broadphase::PairBufferSoA denseBuffer;
    denseBuffer.push(0u, 1u);
    denseBuffer.push(2u, 3u);
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompaction(denseBuffer),
               "dense all-valid buffer skips compaction helper");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferClamp(denseBuffer),
               "under-capacity buffer skips clamp helper");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(2u, 3u);
    overflowBuffer.push(4u, 5u);
    overflowBuffer.setMaxCapacity(2u);
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferClamp(overflowBuffer),
               "overflow buffer does not skip clamp helper");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferSort(overflowBuffer),
               "multi-pair buffer does not skip sort helper");
}

void testShouldRunRefineBroadphaseGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectTrue(!fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase inverse of shouldRun");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    expectTrue(fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase true when refine is viable");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase false when shouldRun is true");
}

void testCellOccupancyPreflightReasonGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight validPreflight =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectEq(static_cast<fuse::u32>(validPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "valid range preflight carries None reason");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 8u),
               "valid range does not skip occupancy iteration");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight emptyPreflight =
        fuse::physics::broadphase::preflightCellOccupancy(inverted, 4u);
    expectEq(static_cast<fuse::u32>(emptyPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
             "inverted range preflight carries EmptyRange reason");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(inverted, 4u),
               "inverted range skips occupancy iteration");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight overBudgetPreflight =
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 4u);
    expectEq(static_cast<fuse::u32>(overBudgetPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "over-budget 2D range preflight carries ExceedsBudget reason");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(planeRange, 4u),
               "over-budget range skips occupancy iteration");
}

void testBroadphaseMergePreflightGuards() {

    const fuse::physics::broadphase::BroadphaseMergePreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectTrue(!emptyPreflight.canMerge(), "empty scene cannot merge plane-dynamic pairs");
    expectTrue(emptyPreflight.emptyPlaneBodies, "empty scene has no plane bodies");
    expectTrue(emptyPreflight.emptyDynamicBodies, "empty scene has no dynamic bodies");
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "empty scene merge preflight carries EmptyPlaneBodies reason");

    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight planeOnlyPreflight =
    expectTrue(!planeOnlyPreflight.canMerge(), "plane-only scene cannot merge");
    expectTrue(!planeOnlyPreflight.emptyPlaneBodies, "plane-only scene has plane bodies");
    expectTrue(planeOnlyPreflight.emptyDynamicBodies, "plane-only scene has no dynamic bodies");
    expectEq(static_cast<fuse::u32>(planeOnlyPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only merge preflight carries EmptyDynamicBodies reason");

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
               "canSkipCellOccupancyIteration false within budget");

    const fuse::physics::broadphase::CellOccupancyPreflight preflight =
             "cell occupancy preflight carries reject reason");
    expectTrue(preflight.canIterate(), "cell occupancy preflight can iterate within budget");

    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "canSkipCellOccupancyIteration true over budget");
    expectTrue(!fuse::physics::broadphase::shouldRunCellOccupancyIteration(validRange, 7u),
               "shouldRunCellOccupancyIteration false over budget");

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
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(fuse::physics::broadphase::pairBufferSortRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
               "sort rejects for EmptyBuffer on empty buffer");
void testPairBufferCompactAndClampPreflightGuards() {
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer skips compactAndClamp");
void testPairBufferWriteSlotRejectReasonGuards() {
    buffer.preparePairSlots(2u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
             "in-range valid write reports None reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferWriteSlotRejectsForReason(
                   buffer, 0u, 0u, 1u, fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
               "valid write rejects for None");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
             "self-pair write reports InvalidPair reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferWriteSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),
                           "OutOfRangeSlot") == 0,
               "OutOfRangeSlot write reject reason has stable label");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 4u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot reject reason");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 1u, 2u, 2u);
    expectTrue(!preflight.canWrite(), "write-slot preflight rejects self-pair");
    expectTrue(preflight.invalidPair, "write-slot preflight marks invalid pair");




    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort false on empty buffer");

    buffer.push(0u, 1u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
             "single pair reports SinglePair sort reject reason");
void testShouldRunBroadphaseGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase true when shouldRunBroadphase false");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
               "shouldRunBroadphase false on singleton scene");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration false on empty scene");

               "shouldRunBroadphasePairGeneration false on singleton scene");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase false when shouldRunBroadphase true");


               "empty buffer rejects for EmptyBuffer sort reason");
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
    expectTrue(fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration true on populated scene");
}

void testPerShapeCellBudgetGuards() {
    expectEq(fuse::physics::broadphase::perShapeCellBudget(0u, false), 0u,
             "zero span is unlimited per-shape cell budget");
    expectEq(fuse::physics::broadphase::perShapeCellBudget(4u, false), 64u,
             "3D per-shape cell budget is span cubed");
    expectEq(fuse::physics::broadphase::perShapeCellBudget(4u, true), 16u,
             "2D per-shape cell budget is span squared");

    const fuse::physics::broadphase::CellRange3 smallRange = {{0, 0, 0}, {1, 1, 1}};
    expectTrue(!fuse::physics::broadphase::exceedsPerShapeCellBudget(smallRange, 4u),
               "eight-cell range is within 4^3 per-shape budget");
    expectTrue(!fuse::physics::broadphase::exceedsPerShapeCellBudget(smallRange, 2u),
               "eight-cell range is within 2^3 per-shape budget at limit");
    expectTrue(fuse::physics::broadphase::exceedsPerShapeCellBudget(smallRange, 1u),
               "eight-cell range exceeds 1^3 per-shape budget");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    expectTrue(fuse::physics::broadphase::exceedsPerShapeCellBudget(planeRange, 2u),
               "2D range exceeds 2^2 per-shape budget");

void testPairBufferSortRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;



             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer sort reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferSortRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),

void testPairBufferWriteSlotRejectReasonGuards() {
    buffer.preparePairSlots(2u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
             "valid writeSlot reports None reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferWriteSlotRejectsForReason(
                   buffer, 0u, 0u, 1u, fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
               "valid writeSlot rejects for None");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 2u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferWriteSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
                           "InvalidPair") == 0,
               "InvalidPair write-slot reject reason has stable label");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
             "self-pair writeSlot reports InvalidPair reject reason");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 0u, 1u);
    expectTrue(preflight.canWrite(), "write-slot preflight accepts valid slot");
    expectEq(static_cast<fuse::u32>(preflight.reason),
             "write-slot preflight carries reject reason");

    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort false on empty buffer");

    buffer.push(0u, 1u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
             "single pair reports SinglePair sort reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSortRejectReasonName(
                               fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair sort reject reason has stable label");

    buffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::None),
             "multiple pairs report None sort reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort true for multiple pairs");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort false for multiple pairs");
    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
             "single pair reports SinglePair sort reject reason");

    buffer.push(2u, 3u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::None),
             "multiple pairs report None sort reject reason");

    const fuse::physics::broadphase::PairBufferSortPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectTrue(preflight.needsSort(), "sort preflight accepts multiple pairs with reason None");
    expectEq(static_cast<fuse::u32>(preflight.reason),
             "sort preflight carries reject reason");

void testPairBufferCompactAndClampRejectReasonGuards() {
}

void testShouldRunPairBufferDedupeAndSortGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe false on empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe true when shouldRunPairBufferDedupe false");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort true on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort false on empty buffer");

               "shouldRunPairBufferDedupe false on single pair");
               "shouldRunPairBufferSort false on single pair");

    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe true for multiple pairs");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort true for multiple pairs");
               "canSkipPairBufferSort false when shouldRunPairBufferSort true");

void testPairSlotPreflightGuards() {


    buffer.setMaxCapacity(2u);

    const fuse::physics::broadphase::PairSlotPreflight zeroSlots =
        fuse::physics::broadphase::preflightPairSlots(0u, buffer);
    expectTrue(zeroSlots.skipped, "zero slot count is skipped");
    expectTrue(!zeroSlots.canPrepare(), "zero slot preflight cannot prepare");

    const fuse::physics::broadphase::PairSlotPreflight withinCapacity =
        fuse::physics::broadphase::preflightPairSlots(2u, buffer);
    expectTrue(withinCapacity.canPrepare(), "slot count within max capacity can prepare");
    expectTrue(!withinCapacity.exceedsBufferCapacity,
               "slot count at max capacity does not exceed buffer capacity");

    const fuse::physics::broadphase::PairSlotPreflight exceedsCapacity =
        fuse::physics::broadphase::preflightPairSlots(4u, buffer);
    expectTrue(exceedsCapacity.canPrepare(), "exceeding slot count may still prepare slots");
    expectTrue(exceedsCapacity.exceedsBufferCapacity,
               "slot count above max capacity flags exceedsBufferCapacity");

void testPairBufferCompactAndClampPreflightGuards() {

    buffer.preparePairSlots(0u);
    expectTrue(buffer.canSkipSoAIteration(), "preparePairSlots(0) clears slot storage via preflight");




    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compact+clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompactAndClamp(buffer),
               "canSkipPairBufferCompactAndClamp on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(buffer),
               "shouldRunPairBufferCompactAndClamp false on empty buffer");

    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    expectTrue(buffer.canSkipCompactAndClamp(), "dense valid buffer within capacity skips compactAndClamp");
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWorkNeeded),
             "dense valid buffer reports NoWorkNeeded compact+clamp reject reason");
    expectEq(buffer.compactAndClamp(), 2u, "compactAndClamp early-outs without changing dense valid buffer");
             "all-valid within-capacity buffer reports NoWorkNeeded");
    expectTrue(buffer.canSkipCompactAndClamp(), "all-valid buffer skips compact+clamp");
    expectEq(buffer.compactAndClamp(), 2u, "compactAndClamp no-op preserves active count");

    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(2u, 2u, 3u);
    expectTrue(!buffer.canSkipCompactAndClamp(), "sparse slots need compactAndClamp work");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(buffer),
               "shouldRunPairBufferCompactAndClamp true when invalid slots exist");
    expectEq(buffer.compactAndClamp(), 2u, "compactAndClamp gathers sparse valid slots");

void testPairBufferSortAndDedupeShouldRunGuards() {
    expectTrue(buffer.canSkipCompactAndClamp(), "SoA canSkipCompactAndClamp on empty buffer");

    buffer.setMaxCapacity(4u);
             "finalized within-capacity buffer reports NoWorkNeeded");
    expectTrue(fuse::physics::broadphase::pairBufferCompactAndClampRejectsForReason(
                   buffer,
               "finalized buffer rejects for NoWorkNeeded");
    expectTrue(std::strcmp(
                   fuse::physics::broadphase::pairBufferCompactAndClampRejectReasonName(
                   "NoWorkNeeded") == 0,
               "NoWorkNeeded compact+clamp reject reason has stable label");

    fuse::physics::broadphase::PairBufferSoA slotBuffer;
    slotBuffer.setMaxCapacity(1u);
    slotBuffer.preparePairSlots(2u);
    slotBuffer.writeSlot(0u, 0u, 1u);
    slotBuffer.writeSlot(1u, 2u, 3u);
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(slotBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::None),
             "uncompacted slot buffer reports None compact+clamp reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(slotBuffer),
               "shouldRunPairBufferCompactAndClamp true for uncompacted slots");
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::None),

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compact-and-clamp reject reason");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs via preflight on empty buffer");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWorkNeeded),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactAndClampRejectReasonName(
               "NoWorkNeeded compact-and-clamp reject reason has stable label");
               "shouldRunPairBufferCompactAndClamp false when no work needed");

    buffer.preparePairSlots(2u);
    buffer.invalidateSlot(1u);
             "invalid slots report None compact-and-clamp reject reason");
               "shouldRunPairBufferCompactAndClamp true when compaction needed");

    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(buffer);
    expectTrue(preflight.needsCompactAndClamp(), "compact-and-clamp preflight requests work");
    expectEq(buffer.compactAndClamp(), 1u, "compactAndClamp compacts via preflight gate");
}

void testShouldRunPairBufferDedupeGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe false on empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe mirrors shouldRun inverse on empty buffer");

               "shouldRunPairBufferDedupe false for single pair");

    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe true for multiple pairs");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe false when shouldRunPairBufferDedupe true");

void testShouldRunBroadphaseGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase true when shouldRunBroadphase false");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration false on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
               "shouldRunBroadphase false on singleton scene");
               "shouldRunBroadphasePairGeneration false on singleton scene");


               "canSkipBroadphase mirrors shouldRun inverse on empty scene");


    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration true on populated scene");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase false when shouldRunBroadphase true");


               "canSkipPairBufferDedupe true when shouldRunPairBufferDedupe false");

               "shouldRunPairBufferDedupe false on single pair");
    expectTrue(fuse::physics::broadphase::pairBufferDedupeRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
               "single pair rejects for SinglePair at SoA layer");


void testPairBufferSortRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer sort reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferSortRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
               "empty buffer rejects for EmptyBuffer sort reason");

    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort false on empty buffer");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSortRejectReasonName(
                               fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair sort reject reason has stable label");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
             "single pair reports SinglePair sort reject reason");

                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
               "pairBufferSortRejectsForReason matches single pair");

               "single pair rejects for SinglePair");

               "canSkipPairBufferSort on single pair");

    const fuse::physics::broadphase::PairBufferSortPreflight singleSort =
        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectEq(static_cast<fuse::u32>(singleSort.reason),
             "sort preflight carries reject reason for single pair");
    expectTrue(!singleSort.needsSort(), "single-pair sort preflight does not need sort");


             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::None),
             "multiple pairs report None sort reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort true for multiple pairs");

    const fuse::physics::broadphase::PairBufferSortPreflight preflight =
             "sort preflight carries reject reason");

void testPairBufferCompactAndClampPreflightGuards() {
    expectEq(static_cast<fuse::u32>(preflight.reason),

void testPairBufferWriteSlotPreflightGuards() {
    buffer.preparePairSlots(2u);

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight validWrite =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 0u, 1u);
    expectTrue(validWrite.canWrite(), "write-slot preflight accepts valid pair");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight invalidPair =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 1u, 1u);
    expectTrue(invalidPair.invalidPair, "write-slot preflight marks self-pair invalid");
    expectTrue(!invalidPair.canWrite(), "write-slot preflight rejects self-pair");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight outOfRange =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 4u, 0u, 1u);
    expectTrue(outOfRange.outOfRangeSlot, "write-slot preflight marks out-of-range slot");
    expectTrue(fuse::physics::broadphase::pairBufferWriteSlotRejectsForReason(
                   buffer, 4u, 0u, 1u, fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),
               "write-slot rejects for OutOfRangeSlot");

    buffer.writeSlot(1u, 1u, 1u);
    expectEq(buffer.compact(), 1u, "writeSlot rejects invalid pair via preflight gate");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compact-and-clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompactAndClamp(buffer),
               "canSkipPairBufferCompactAndClamp on empty buffer");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs via preflight on empty buffer");

                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWork),
             "synced within-capacity buffer reports NoWork compact-and-clamp reject reason");
               "synced buffer rejects for NoWork");
    expectEq(buffer.compactAndClamp(), 2u, "compactAndClamp no-op returns synced active count");

                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::None),
             "prepared slots with stale activeCount report None compact-and-clamp reject reason");


    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
             static_cast<fuse::u32>(
    expectTrue(fuse::physics::broadphase::pairBufferCompactAndClampRejectsForReason(
                   buffer,

    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);

    buffer.invalidateSlot(1u);
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(buffer),
               "shouldRunPairBufferCompactAndClamp true when invalid slots exist");

    fuse::physics::broadphase::PairBufferSoA clampBuffer;
    clampBuffer.setMaxCapacity(1u);
    clampBuffer.preparePairSlots(2u);
    clampBuffer.writeSlot(0u, 0u, 1u);
    clampBuffer.writeSlot(1u, 2u, 3u);
    expectEq(clampBuffer.compactAndClamp(), 1u, "compactAndClamp gathers then clamps via preflight gate");
    expectTrue(clampBuffer.isSortedCanonical(), "compactAndClamp leaves canonical order");




void testMergePairsIntoBufferPreflightGuards() {
    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs;

}



    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);

    fuse::physics::broadphase::PairBufferSoA buffer;

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

    buffer.setMaxCapacity(1u);
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

void testPairBufferInvalidateSlotRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "canSkipPairBufferInvalidateSlot on empty buffer");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
                           "AlreadyInvalid") == 0,
               "AlreadyInvalid invalidate reject reason has stable label");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None),
             "valid slot reports None invalidate reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot true for valid slot");

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 2u)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot invalidate reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateSlotRejectsForReason(
                   buffer, 2u,
                   fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
               "out-of-range slot rejects for OutOfRangeSlot");

    buffer.invalidateSlot(1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 1u)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
             "already-invalid slot reports AlreadyInvalid invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 1u),
               "canSkipPairBufferInvalidateSlot true for already-invalid slot");

    expectTrue(fuse::physics::broadphase::invalidateSlotWithPreflight(buffer, 0u),
               "invalidateSlotWithPreflight succeeds on valid slot");
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlotWithPreflight clears slot validity");
    expectTrue(!fuse::physics::broadphase::invalidateSlotWithPreflight(buffer, 0u),
               "invalidateSlotWithPreflight skips already-invalid slot");
}

void testShapeCellCapacityRejectReasonAndPreflight() {
    const fuse::physics::broadphase::CellRange3 withinRange = {{0, 0, 0}, {1, 1, 1}};
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::shapeCellCapacityRejectReason(withinRange, 4u, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellCapacityRejectReason::None),
             "within-limit range reports None shape cell-capacity reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellCapacityIteration(withinRange, 4u, 8u),
               "shouldRunShapeCellCapacityIteration true within span and budget");
    expectTrue(std::strcmp(fuse::physics::broadphase::shapeCellCapacityRejectReasonName(
                               fuse::physics::broadphase::ShapeCellCapacityRejectReason::ExceedsBudget),
                           "ExceedsBudget") == 0,
               "ExceedsBudget shape cell-capacity reject reason has stable label");

    expectTrue(fuse::physics::broadphase::shapeCellCapacityRejectsForReason(
                   withinRange, 1u, 8u,
                   fuse::physics::broadphase::ShapeCellCapacityRejectReason::ExceedsSpan),
               "over-span range rejects for ExceedsSpan");
                   withinRange, 4u, 7u,
               "over-budget range rejects for ExceedsBudget");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
                 fuse::physics::broadphase::shapeCellCapacityRejectReason(inverted, 4u, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellCapacityRejectReason::EmptyRange),
             "inverted range reports EmptyRange shape cell-capacity reject reason");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellCapacityIteration(inverted, 4u, 8u),
               "canSkipShapeCellCapacityIteration true for empty range");

    const fuse::physics::broadphase::ShapeCellCapacityPreflight preflight =
        fuse::physics::broadphase::preflightShapeCellCapacity(withinRange, 4u, 8u);
    expectTrue(preflight.canIterate(), "shape cell-capacity preflight accepts valid range");
    expectEq(preflight.occupancyCount, 8u, "shape cell-capacity preflight reports occupancy count");
    expectEq(static_cast<fuse::u32>(preflight.reason),
             "shape cell-capacity preflight carries reject reason");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::ShapeCellCapacityPreflight planePreflight =
        fuse::physics::broadphase::preflightShapeCellCapacity2D(planeRange, 4u, 4u);
    expectTrue(!planePreflight.canIterate(), "2D shape cell-capacity preflight rejects over-budget range");
    expectTrue(planePreflight.exceedsBudget, "2D shape cell-capacity preflight marks exceedsBudget");
}

void testBroadphaseMergePairsIntoBufferPreflightGuards() {
void testPairBufferInvalidateSlotRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None),
             "valid slot reports None invalidate reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot true for valid slot");

    buffer.invalidateSlot(0u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
             "cleared slot reports AlreadyInvalid invalidate reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateSlotRejectsForReason(
                   buffer, 0u,
                   fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
               "cleared slot rejects for AlreadyInvalid");
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u),
               "wouldSkipPairBufferInvalidateSlot false for valid slot");

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             "invalidated slot reports AlreadyInvalid reject reason");
               "invalidated slot rejects for AlreadyInvalid");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "canSkipPairBufferInvalidateSlot true for already-invalid slot");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
                           "OutOfRangeSlot") == 0,
               "OutOfRangeSlot invalidate reject reason has stable label");

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 2u),
               "canSkipPairBufferInvalidateSlot true for out-of-range slot");

    fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason invalidateReason =
        fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 2u, &invalidateReason),
               "wouldSkipPairBufferInvalidateSlot true for out-of-range slot");
    expectEq(static_cast<fuse::u32>(invalidateReason),
             static_cast<fuse::u32>(
             "wouldSkipPairBufferInvalidateSlot records OutOfRangeSlot reason");

    buffer.writeSlot(1u, 2u, 3u);
    fuse::physics::broadphase::PairBufferWriteSlotRejectReason writeReason =
        fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 1u, 2u, 3u, &writeReason),
               "wouldSkipPairBufferWriteSlot false for valid write");
    expectEq(static_cast<fuse::u32>(writeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
             "wouldSkipPairBufferWriteSlot records None for valid write");

    buffer.invalidateSlot(1u);
    expectTrue(!buffer.slotIsValid(1u), "invalidateSlot clears slot via preflight gate");
    expectTrue(!buffer.slotIsValid(1u), "invalidateSlot is no-op on already-invalid slot");

void testCellCapacityPreflightAndWouldSkipGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellCapacityPreflight withinCapacity =
        fuse::physics::broadphase::preflightCellCapacity(validRange, 8u, 4u);
    expectTrue(withinCapacity.canInsert(), "cell-capacity preflight accepts range within budget");
    expectEq(withinCapacity.occupancyCount, 8u, "cell-capacity preflight reports occupancy count");
    expectTrue(!withinCapacity.exceedsSpan, "cell-capacity preflight does not mark within-span range");

    expectTrue(fuse::physics::broadphase::shouldRunCellCapacityInsertion(validRange, 8u, 4u),
               "shouldRunCellCapacityInsertion true for valid range");
    expectTrue(!fuse::physics::broadphase::canSkipCellCapacityInsertion(validRange, 8u, 4u),
               "canSkipCellCapacityInsertion false for valid range");

    fuse::physics::broadphase::CellCapacityRejectReason capacityReason =
        fuse::physics::broadphase::CellCapacityRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipCellCapacityInsertion(validRange, 8u, 4u, &capacityReason),
               "wouldSkipCellCapacityInsertion false for valid range");
    expectEq(static_cast<fuse::u32>(capacityReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityRejectReason::None),
             "wouldSkipCellCapacityInsertion records None for valid range");

    expectTrue(fuse::physics::broadphase::cellCapacityRejectsForReason(
                   validRange, 7u, 4u,
                   fuse::physics::broadphase::CellCapacityRejectReason::ExceedsOccupancyBudget),
               "over-budget range rejects for ExceedsOccupancyBudget");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellCapacityRejectReasonName(
                           "ExceedsOccupancyBudget") == 0,
               "ExceedsOccupancyBudget cell-capacity reject reason has stable label");

    const fuse::physics::broadphase::CellCapacityPreflight overBudget =
        fuse::physics::broadphase::preflightCellCapacity(validRange, 7u, 4u);
    expectTrue(!overBudget.canInsert(), "cell-capacity preflight rejects over-budget range");
    expectTrue(overBudget.exceedsOccupancyBudget, "cell-capacity preflight marks exceedsOccupancyBudget");
    expectTrue(fuse::physics::broadphase::wouldSkipCellCapacityInsertion(validRange, 7u, 4u, &capacityReason),
               "wouldSkipCellCapacityInsertion true for over-budget range");

    const fuse::physics::broadphase::CellRange3 overSpan = {{0, 0, 0}, {5, 1, 1}};
    const fuse::physics::broadphase::CellCapacityPreflight spanPreflight =
        fuse::physics::broadphase::preflightCellCapacity(overSpan, 0u, 4u);
    expectTrue(spanPreflight.canInsert(), "span excess does not block insertion when occupancy is unlimited");
    expectTrue(spanPreflight.exceedsSpan, "cell-capacity preflight marks exceedsSpan");

    fuse::physics::broadphase::CellOccupancyRejectReason occupancyReason =
        fuse::physics::broadphase::CellOccupancyRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u, &occupancyReason),
               "wouldSkipCellOccupancyIteration true for over-budget range");
    expectEq(static_cast<fuse::u32>(occupancyReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "wouldSkipCellOccupancyIteration records ExceedsBudget reason");

    const fuse::physics::broadphase::CellCapacityPreflight planePreflight =
        fuse::physics::broadphase::preflightCellCapacity2D(planeRange, 4u, 4u);
    expectTrue(!planePreflight.canInsert(), "2D cell-capacity preflight rejects over-budget range");
    expectEq(planePreflight.occupancyCount, 8u, "2D cell-capacity preflight reports occupancy count");

void testRefineDedupeMergeWouldSkipGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    fuse::physics::broadphase::RefineBroadphaseRejectReason refineReason =
        fuse::physics::broadphase::RefineBroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer, &refineReason),
               "wouldSkipRefineBroadphase true on empty scene");
    expectEq(static_cast<fuse::u32>(refineReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyBuffer),
             "wouldSkipRefineBroadphase records EmptyBuffer reason");
    expectEq(static_cast<fuse::u32>(
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 2u),

    buffer.invalidateSlot(2u);
    expectTrue(buffer.slotIsValid(1u), "out-of-range invalidate leaves other slots untouched");
}

void testPairBufferWouldSkipWriteSlotGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(1u);

    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u),
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u),
               "wouldSkipPairBufferWriteSlot true for self-pair");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u) ==
                   fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u),
               "wouldSkipPairBufferWriteSlot agrees with canSkipPairBufferWriteSlot");

void testWouldSkipCellCapacityGuards() {
    expectTrue(!fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 8u),
               "wouldSkipCellOccupancyIteration false within budget");
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u),
               "wouldSkipCellOccupancyIteration true over budget");
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u) ==
                   fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "wouldSkipCellOccupancyIteration agrees with canSkipCellOccupancyIteration");

    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(validRange, 4u),
               "wouldSkipCellSpanClamp true within span limit");
    expectTrue(!fuse::physics::broadphase::wouldSkipCellSpanClamp(validRange, 1u),
               "wouldSkipCellSpanClamp false when span exceeds budget");
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(validRange, 3u) ==
                   fuse::physics::broadphase::canSkipCellSpanClamp(validRange, 3u),
               "wouldSkipCellSpanClamp agrees with canSkipCellSpanClamp");

    const fuse::physics::broadphase::ShapeCellInsertionPreflight insertionPreflight =
        fuse::physics::broadphase::preflightShapeCellInsertion(validRange, 8u);
    expectTrue(insertionPreflight.canInsert(), "shape cell insertion preflight accepts within budget");
    expectTrue(!fuse::physics::broadphase::wouldSkipShapeCellInsertion(validRange, 8u),
               "wouldSkipShapeCellInsertion false within budget");
    expectTrue(fuse::physics::broadphase::wouldSkipShapeCellInsertion(validRange, 7u),
               "wouldSkipShapeCellInsertion true over budget");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    expectTrue(fuse::physics::broadphase::wouldSkipShapeCellInsertion2D(planeRange, 4u),
               "2D wouldSkipShapeCellInsertion true over budget");

void testWouldSkipRefineDedupeMergeGuards() {

    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer),
    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer) ==
                   fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "wouldSkipRefineBroadphase agrees with canSkipRefineBroadphase");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);
    expectTrue(!fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer, &refineReason),
               "wouldSkipRefineBroadphase false on valid refine scene");
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::None),
             "wouldSkipRefineBroadphase records None on valid refine scene");

    fuse::physics::broadphase::DedupeBroadphaseRejectReason dedupeReason =
        fuse::physics::broadphase::DedupeBroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer, &dedupeReason),
               "wouldSkipDedupeBroadphase true on single pair");
    expectEq(static_cast<fuse::u32>(dedupeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::SinglePair),
             "wouldSkipDedupeBroadphase records SinglePair reason");
    expectTrue(fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer),
               "wouldSkipDedupeBroadphase true on empty buffer");
    expectTrue(fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer) ==
                   fuse::physics::broadphase::canSkipDedupeBroadphase(buffer),
               "wouldSkipDedupeBroadphase agrees with canSkipDedupeBroadphase");

    buffer.push(2u, 3u);
    expectTrue(!fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer, &dedupeReason),
               "wouldSkipDedupeBroadphase false for multiple pairs");

    fuse::physics::broadphase::BroadphaseMergeRejectReason mergeReason =
        fuse::physics::broadphase::BroadphaseMergeRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphaseMerge(bodies, shapes, &mergeReason),
               "wouldSkipBroadphaseMerge true without plane bodies");
    expectEq(static_cast<fuse::u32>(mergeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "wouldSkipBroadphaseMerge records EmptyPlaneBodies reason");

    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectTrue(!fuse::physics::broadphase::wouldSkipBroadphaseMerge(bodies, shapes, &mergeReason),
               "wouldSkipBroadphaseMerge false for plane plus dynamic scene");
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphaseMerge(bodies, shapes),
               "wouldSkipBroadphaseMerge true on empty scene");
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphaseMerge(bodies, shapes) ==
                   fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "wouldSkipBroadphaseMerge agrees with canSkipBroadphaseMerge");

    fuse::physics::broadphase::PairBufferSoA mergeBuffer;
    const std::vector<fuse::physics::broadphase::CandidatePair> mergePairs = {{0u, 1u}};
    fuse::physics::broadphase::MergePairsIntoBufferRejectReason mergeIntoReason =
        fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(mergePairs, mergeBuffer, &mergeIntoReason),
               "wouldSkipMergePairsIntoBuffer false for valid merge");
    expectEq(static_cast<fuse::u32>(mergeIntoReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None),
             "wouldSkipMergePairsIntoBuffer records None for valid merge");

    mergeBuffer.setMaxCapacity(1u);
    mergeBuffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(mergePairs, mergeBuffer, &mergeIntoReason),
               "wouldSkipMergePairsIntoBuffer true when buffer is full");
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
             "wouldSkipMergePairsIntoBuffer records BufferFull reason");
    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}};
    expectTrue(!fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, buffer),
               "wouldSkipMergePairsIntoBuffer false for valid merge into empty buffer");
    expectTrue(fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, buffer) ==
                   fuse::physics::broadphase::canSkipMergePairsIntoBuffer(pairs, buffer),
               "wouldSkipMergePairsIntoBuffer agrees with canSkipMergePairsIntoBuffer");

    buffer.setMaxCapacity(1u);
    buffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, buffer),
}

void testRefineDedupeMergeWithPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;
    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}, {2u, 3u}};

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergePairsIntoBufferRejectReason(
                     bodies, shapes, pairs, buffer)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::BroadphaseMergePairsIntoBufferRejectReason::EmptyPlaneBodies),
             "empty scene reports EmptyPlaneBodies combined merge reject reason");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMergePairsIntoBuffer(bodies, shapes, pairs, buffer),
               "canSkipBroadphaseMergePairsIntoBuffer on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergePairsIntoBufferRejectReason(
                     bodies, shapes, pairs, buffer)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::BroadphaseMergePairsIntoBufferRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies combined merge reject reason");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMergePairsIntoBuffer(bodies, shapes, pairs, buffer),
               "shouldRunBroadphaseMergePairsIntoBuffer true for mergeable scene with pairs");

    const fuse::physics::broadphase::BroadphaseMergePairsIntoBufferPreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMergePairsIntoBuffer(bodies, shapes, pairs, buffer);
    expectTrue(preflight.canMerge(), "combined merge preflight accepts valid scene and pairs");
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::BroadphaseMergePairsIntoBufferRejectReason::None),
             "combined merge preflight carries reject reason");

    expectTrue(fuse::physics::broadphase::mergeBroadphasePairsIntoBufferWithPreflight(
                   bodies, shapes, pairs, buffer),
               "mergeBroadphasePairsIntoBufferWithPreflight succeeds on valid scene");
    expectEq(buffer.activeCount, 2u, "combined merge preflight wrapper pushes valid pairs");

    buffer.setMaxCapacity(2u);
    expectTrue(fuse::physics::broadphase::broadphaseMergePairsIntoBufferRejectsForReason(
                   bodies, shapes, pairs, buffer,
                   fuse::physics::broadphase::BroadphaseMergePairsIntoBufferRejectReason::BufferFull),
               "full buffer rejects for BufferFull in combined merge preflight");
    expectTrue(!fuse::physics::broadphase::mergeBroadphasePairsIntoBufferWithPreflight(
                   bodies, shapes, pairs, buffer),
               "mergeBroadphasePairsIntoBufferWithPreflight skips when buffer is full");
}

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
    expectTrue(!fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer),
               "dedupe with preflight returns false on empty buffer");
    dedupeBuffer.push(0u, 1u);
    expectTrue(!fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer),
               "dedupe with preflight returns false on single pair");
               "dedupe with preflight skips single pair");
    expectEq(dedupeBuffer.activeCount, 1u, "dedupe with preflight is no-op on single pair");

    dedupeBuffer.push(2u, 3u);
    expectTrue(fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer),
               "dedupe with preflight returns true for multiple pairs");
    dedupeBuffer.push(0u, 1u);
               "dedupe with preflight returns true when dedupe runs");
    expectTrue(fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer),
               "dedupe with preflight runs on multiple pairs");
               "dedupe with preflight returns true for multiple pairs");
    expectEq(dedupeBuffer.activeCount, 2u, "dedupe with preflight removes duplicate pairs");

    const std::vector<fuse::physics::broadphase::CandidatePair> mergePairs = {{0u, 1u}, {2u, 3u}};
    expectTrue(fuse::physics::broadphase::mergePairsIntoBufferWithPreflight(mergePairs, mergeBuffer),
               "merge with preflight returns true when merge runs");
               "merge with preflight runs on valid pair list");
               "merge with preflight returns true for valid merge");
    expectEq(mergeBuffer.activeCount, 2u, "merge with preflight pushes valid pairs");

    mergeBuffer.setMaxCapacity(2u);
    expectTrue(mergeBuffer.isFull(), "merge buffer at capacity after setMaxCapacity");
    expectTrue(fuse::physics::broadphase::mergePairsIntoBufferRejectsForReason(
                   mergePairs, mergeBuffer,
               "full buffer rejects for BufferFull after merge");
    expectTrue(!fuse::physics::broadphase::shouldRunMergePairsIntoBuffer(mergePairs, mergeBuffer),

void testPairBufferShouldRunDedupeGuards() {



             "empty buffer reports EmptyBuffer compact+clamp reject reason");
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer skips compact+clamp member guard");

    buffer.setMaxCapacity(2u);
             static_cast<fuse::u32>(fuse::physics::broadphase::
                                       PairBufferCompactAndClampRejectReason::AlreadyCompactAndWithinCapacity),
             "within-capacity buffer reports AlreadyCompactAndWithinCapacity reject reason");
                   fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::
                       AlreadyCompactAndWithinCapacity),
               "compact+clamp rejects for AlreadyCompactAndWithinCapacity");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(buffer),
               "shouldRunPairBufferCompactAndClamp false when already compact");

void testPairBufferCompactAndClampRejectReasonGuards() {
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer canSkipCompactAndClamp");

             "valid compact buffer reports NoWorkNeeded compact+clamp reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactAndClampRejectReasonName(

             "invalid slots with overflow report None compact+clamp reject reason");
               "shouldRunPairBufferCompactAndClamp true when compaction or clamp needed");
    expectEq(slotBuffer.compactAndClamp(), 1u, "compactAndClamp gathers and clamps via preflight gate");

void testPairBufferDedupeShouldRunGuards() {
                   buffer, fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
               "SoA dedupe rejects for EmptyBuffer");


             "invalid slots report None compact+clamp reject reason");
               "shouldRunPairBufferCompactAndClamp true when compaction needed");

    expectTrue(!fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort false for multiple pairs");


               "canSkipPairBufferDedupe true on empty buffer");


               "canSkipPairBufferDedupe false for multiple pairs");

void testBroadphaseShouldRunGuards() {




                 fuse::physics::broadphase::MergePairsIntoBufferRejectReason::EmptyPairs),
             "empty pair list reports EmptyPairs merge reject reason");

                 fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferAtCapacity),
             "full buffer reports BufferAtCapacity merge reject reason");
                           "BufferAtCapacity") == 0,
               "BufferAtCapacity merge reject reason has stable label");

    const fuse::physics::broadphase::MergePairsIntoBufferPreflight preflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, mergeBuffer);
    expectTrue(preflight.canMerge(), "merge preflight accepts non-empty pair list into empty buffer");
    expectTrue(fuse::physics::broadphase::shouldRunMergePairsIntoBuffer(pairs, mergeBuffer),
               "shouldRunMergePairsIntoBuffer true for mergeable input");

void testRefinableBroadphasePairCountGuards() {

    expectEq(fuse::physics::broadphase::countRefinableBroadphasePairs(bodies, shapes, buffer), 0u,
             "empty scene has zero refinable pairs");
    expectTrue(!fuse::physics::broadphase::hasRefinableBroadphasePair(bodies, shapes, buffer),
               "hasRefinableBroadphasePair false on empty scene");


    expectEq(fuse::physics::broadphase::countRefinableBroadphasePairs(bodies, shapes, buffer), 1u,
             "countRefinableBroadphasePairs excludes separated pair");
    expectTrue(fuse::physics::broadphase::hasRefinableBroadphasePair(bodies, shapes, buffer),
               "hasRefinableBroadphasePair true when overlapping pair exists");

void testShouldRunBroadphaseGuard() {

               "canSkipBroadphase mirrors shouldRunBroadphase inverse");



    const fuse::physics::broadphase::MergePairsIntoBufferPreflight emptyPreflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(emptyPairs, buffer);
    expectTrue(!emptyPreflight.canMerge(), "empty pairs cannot merge into buffer");
    expectTrue(emptyPreflight.emptyPairs, "merge preflight marks empty pairs");
               "canSkipMergePairsIntoBuffer on empty pairs");

    expectTrue(validPreflight.canMerge(), "valid pairs can merge into empty buffer");
               "shouldRunMergePairsIntoBuffer true for valid pairs");

    const fuse::physics::broadphase::MergePairsIntoBufferPreflight fullPreflight =
    expectTrue(!fullPreflight.canMerge(), "merge preflight rejects full buffer");
    expectTrue(fullPreflight.bufferFull, "merge preflight marks buffer full");

void testMergePairsIntoBufferRejectReasonGuards() {

             "empty pairs report EmptyPairs merge reject reason");
                   emptyPairs, buffer,
               "mergePairsIntoBufferRejectsForReason matches empty pairs");
               "BufferFull merge reject reason has stable label");

    buffer.push(4u, 5u);
             "full buffer reports BufferFull merge reject reason");

    fuse::physics::broadphase::mergePairsIntoBuffer(pairs, mergeBuffer);
    expectEq(mergeBuffer.activeCount, 2u, "mergePairsIntoBuffer appends valid pairs");



void testCellSpanPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {7, 7, 7}};
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(validRange, 4u),
               "wide range exceeds per-axis span limit");
    expectTrue(fuse::physics::broadphase::cellSpanWithinLimit(validRange, 8u),
               "wide range within larger per-axis span limit");
    expectTrue(fuse::physics::broadphase::isUnboundedCellSpanPerAxis(0u),
               "zero max span is unbounded");

                 fuse::physics::broadphase::cellSpanRejectReason(validRange, 4u)),
             "oversized range reports None span reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanClamp(validRange, 4u),
               "shouldRunCellSpanClamp true when span exceeds limit");
    expectTrue(!fuse::physics::broadphase::canSkipCellSpanClamp(validRange, 4u),
               "canSkipCellSpanClamp false when clamp is needed");

    const fuse::physics::broadphase::CellRange3 compactRange = {{0, 0, 0}, {2, 2, 2}};
                 fuse::physics::broadphase::cellSpanRejectReason(compactRange, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::WithinSpanLimit),
             "compact range reports WithinSpanLimit span reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(compactRange, 4u),
               "canSkipCellSpanClamp true when span is within limit");

                 fuse::physics::broadphase::cellSpanRejectReason(compactRange, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::Unbounded),
             "unbounded span reports Unbounded reject reason");
                               fuse::physics::broadphase::CellSpanRejectReason::WithinSpanLimit),
                           "WithinSpanLimit") == 0,
               "WithinSpanLimit span reject reason has stable label");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
                   inverted, 4u, fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
               "inverted range rejects for EmptyRange");

    const fuse::physics::broadphase::CellSpanPreflight preflight =
        fuse::physics::broadphase::preflightCellSpan(validRange, 4u);
    expectTrue(preflight.needsClamp(), "span preflight requests clamp when span exceeds limit");
             "span preflight carries reject reason");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {7, 1}};
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanClamp(planeRange, 4u),
               "2D shouldRunCellSpanClamp true when span exceeds limit");

void testBroadphaseMergeBufferPreflightGuards() {

    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer);
    expectTrue(!emptyPreflight.canMergeIntoBuffer(), "empty scene cannot merge into buffer");
    expectTrue(emptyPreflight.sceneNotMergeable, "empty scene merge buffer preflight marks scene not mergeable");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMergeIntoBuffer(bodies, shapes, buffer),
               "canSkipBroadphaseMergeIntoBuffer on empty scene");

    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});

    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight mergePreflight =
    expectTrue(mergePreflight.canMergeIntoBuffer(), "mergeable scene with room can merge into buffer");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMergeIntoBuffer(bodies, shapes, buffer),
               "shouldRunBroadphaseMergeIntoBuffer true for mergeable scene");

    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight fullPreflight =
    expectTrue(!fullPreflight.canMergeIntoBuffer(), "full buffer cannot accept merge pairs");
    expectTrue(fullPreflight.bufferAtCapacity, "full buffer merge preflight marks at capacity");
    expectEq(static_cast<fuse::u32>(fullPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::BufferAtCapacity),
    expectTrue(fuse::physics::broadphase::mergeBroadphaseBufferRejectsForReason(
                   bodies, shapes, buffer,
                   fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::BufferAtCapacity),
               "mergeBroadphaseBufferRejectsForReason matches full buffer");
    expectTrue(std::strcmp(fuse::physics::broadphase::mergeBroadphaseBufferRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::SceneNotMergeable),
                           "SceneNotMergeable") == 0,
               "SceneNotMergeable merge-buffer reject reason has stable label");


    expectTrue(preflight.needsSort(), "sort preflight needs sort for multiple pairs");




void testBroadphaseMergePreflightHasBodiesFields() {

    const fuse::physics::broadphase::BroadphaseMergePreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectTrue(!emptyPreflight.hasPlaneBodies, "empty scene has no plane bodies");
    expectTrue(!emptyPreflight.hasDynamicBodies, "empty scene has no dynamic bodies");

    const fuse::physics::broadphase::BroadphaseMergePreflight planeOnlyPreflight =
    expectTrue(planeOnlyPreflight.hasPlaneBodies, "plane-only scene marks hasPlaneBodies");
    expectTrue(!planeOnlyPreflight.hasDynamicBodies, "plane-only scene has no dynamic bodies");

    const fuse::physics::broadphase::BroadphaseMergePreflight mergePreflight =
    expectTrue(mergePreflight.hasPlaneBodies, "merge scene marks hasPlaneBodies");
    expectTrue(mergePreflight.hasDynamicBodies, "merge scene marks hasDynamicBodies");
    expectTrue(mergePreflight.canMerge(), "merge scene can merge with positive body flags");

    buffer.sortCanonical();
    expectTrue(buffer.isSortedCanonical(), "sortCanonical leaves canonical order via shouldRun gate");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWork),
             "within-capacity buffer reports NoWork compact-and-clamp reject reason");

    fuse::physics::broadphase::PairBufferSoA workBuffer;
    workBuffer.setMaxCapacity(1u);
    workBuffer.preparePairSlots(3u);
    workBuffer.writeSlot(0u, 0u, 1u);
    workBuffer.writeSlot(2u, 2u, 3u);
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(workBuffer);
    expectTrue(preflight.needsWork(), "sparse slots with clamp need compact-and-clamp work");
    expectTrue(preflight.needsCompaction, "sparse slots need compaction");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(workBuffer),
               "shouldRunPairBufferCompactAndClamp true when work is needed");
    expectEq(workBuffer.compactAndClamp(), 1u, "compactAndClamp uses preflight gates");

void testBroadphaseCellPairPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellPairRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroSlots),
             "zero slots reports ZeroSlots cell-pair reject reason");
    expectTrue(fuse::physics::broadphase::broadphaseCellPairRejectsForReason(
                   0u, fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroSlots),
               "cell-pair rejects for ZeroSlots");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseCellPairGeneration(0u),
               "canSkipBroadphaseCellPairGeneration on zero slots");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseCellPairGeneration(0u),
               "shouldRunBroadphaseCellPairGeneration false on zero slots");

    const fuse::physics::broadphase::BroadphaseCellPairPreflight zeroPreflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairs(0u);
    expectTrue(zeroPreflight.zeroSlots, "cell-pair preflight marks zero slots");
    expectTrue(!zeroPreflight.canGenerate(), "cell-pair preflight cannot generate zero slots");

    const fuse::physics::broadphase::BroadphaseCellPairPreflight validPreflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairs(4u);
    expectTrue(validPreflight.canGenerate(), "cell-pair preflight accepts non-zero slots");
    expectEq(validPreflight.totalCellSlots, 4u, "cell-pair preflight reports slot count");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseCellPairGeneration(4u),
               "shouldRunBroadphaseCellPairGeneration true for non-zero slots");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseCellPairRejectReasonName(
                               fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroSlots),
                           "ZeroSlots") == 0,
               "ZeroSlots cell-pair reject reason has stable label");












               "sparse slots need compact+clamp work");
                               fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWorkNeeded),



    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe false on empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),


               "shouldRunPairBufferDedupe false on single pair");

    buffer.push(2u, 3u);
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe true for multiple pairs");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe false when shouldRunPairBufferDedupe true");
}

void testPairBufferSortRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer sort reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort false on empty buffer");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSortRejectReasonName(
                               fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair sort reject reason has stable label");

    buffer.push(0u, 1u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
             "single pair reports SinglePair sort reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferSortRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
               "single pair rejects for SinglePair");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::None),
             "multiple pairs report None sort reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort true for multiple pairs");

    const fuse::physics::broadphase::PairBufferSortPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             "sort preflight carries reject reason");

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::EmptyBuffer),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompactAndClamp(buffer),
               "canSkipPairBufferCompactAndClamp on empty buffer");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs on empty buffer");

             "within-capacity all-valid buffer reports NoWorkNeeded");
                           "NoWorkNeeded") == 0,
               "NoWorkNeeded compact+clamp reject reason has stable label");

    fuse::physics::broadphase::PairBufferSoA slotBuffer;
    slotBuffer.setMaxCapacity(1u);
    slotBuffer.preparePairSlots(2u);
    slotBuffer.writeSlot(0u, 0u, 1u);
    slotBuffer.writeSlot(1u, 2u, 3u);
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(slotBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::None),
             "overflow slots report None compact+clamp reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(slotBuffer),
    expectEq(slotBuffer.compactAndClamp(), 1u, "compactAndClamp gathers then clamps overflow slots");

void testShouldRunBroadphaseGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase false on empty scene");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration false on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
               "shouldRunBroadphase false on singleton scene");

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration true on populated scene");
               "shouldRunMergePairsIntoBuffer false when buffer is full");
    expectTrue(!fuse::physics::broadphase::mergePairsIntoBufferWithPreflight(mergePairs, mergeBuffer),
               "merge with preflight skips when buffer is full");

void testPairBufferInvalidateSlotRejectReasonGuards() {
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "empty buffer reports OutOfRangeSlot invalidate reject reason");
               "merge with preflight returns false when buffer is full");

    expectTrue(!fuse::physics::broadphase::mergeBroadphasePlaneDynamicWithPreflight(bodies, shapes, mergeBuffer),
               "plane-dynamic merge with preflight skips when no plane shapes");
}

    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "canSkipPairBufferInvalidateSlot on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot false on empty buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None),
             "valid slot reports None invalidate reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot true for valid slot");

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 2u)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot invalidate reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateSlotRejectsForReason(
                   buffer, 2u,
               "out-of-range slot rejects for OutOfRangeSlot");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
                           "AlreadyInvalid") == 0,
               "AlreadyInvalid invalidate reject reason has stable label");

    buffer.invalidateSlot(0u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
             "already-invalid slot reports AlreadyInvalid invalidate reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateSlotRejectsForReason(
                   buffer, 0u, fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
               "already-invalid slot rejects for AlreadyInvalid");
               "canSkipPairBufferInvalidateSlot true for already-invalid slot");

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 2u)),
             "out-of-range slot reports OutOfRangeSlot invalidate reject reason");

    const fuse::physics::broadphase::PairBufferInvalidateSlotPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferInvalidateSlot(buffer, 1u);
    expectTrue(preflight.canInvalidate(), "invalidate preflight accepts valid slot");

void testShapeCellHashInsertPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::ShapeCellHashInsertPreflight withinBudget =
        fuse::physics::broadphase::preflightShapeCellHashInsert(validRange, 8u);
    expectTrue(withinBudget.canInsert(), "shape hash-insert preflight accepts range within budget");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellHashInsert(validRange, 8u),
               "shouldRunShapeCellHashInsert true within budget");
    expectTrue(!fuse::physics::broadphase::canSkipShapeCellHashInsert(validRange, 8u),
               "canSkipShapeCellHashInsert false within budget");

    expectTrue(fuse::physics::broadphase::canSkipShapeCellHashInsert(validRange, 7u),
               "canSkipShapeCellHashInsert true over budget");
    expectTrue(!fuse::physics::broadphase::shouldRunShapeCellHashInsert(validRange, 7u),
               "shouldRunShapeCellHashInsert false over budget");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::ShapeCellHashInsertPreflight planePreflight =
        fuse::physics::broadphase::preflightShapeCellHashInsert2D(planeRange, 4u);
    expectTrue(!planePreflight.canInsert(), "2D shape hash-insert preflight rejects over-budget range");
    expectEq(planePreflight.occupancy.occupancyCount, 8u, "2D shape hash-insert preflight reports occupancy count");
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears slot via preflight gate");
}

void testCellCapacityRejectReasonAndPreflight() {
                 fuse::physics::broadphase::cellCapacityRejectReason(validRange, 8u, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityRejectReason::None),
             "valid range reports None combined cell-capacity reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellCapacityCheck(validRange, 8u, 4u),
               "shouldRunCellCapacityCheck true within combined budget");
    expectTrue(!fuse::physics::broadphase::canSkipCellCapacityCheck(validRange, 8u, 4u),
               "canSkipCellCapacityCheck false within combined budget");

    const fuse::physics::broadphase::CellCapacityPreflight withinBudget =
        fuse::physics::broadphase::preflightCellCapacity(validRange, 8u, 4u);
    expectTrue(withinBudget.canIterate(), "combined preflight accepts range within budget");
    expectEq(withinBudget.occupancyCount, 8u, "combined preflight reports occupancy count");

    expectTrue(fuse::physics::broadphase::cellCapacityRejectsForReason(
                   validRange, 7u, 4u,
                   fuse::physics::broadphase::CellCapacityRejectReason::ExceedsBudget),
               "over-budget range rejects for ExceedsBudget");

    const fuse::physics::broadphase::CellRange3 wideRange = {{0, 0, 0}, {3, 1, 1}};
                   wideRange, 64u, 3u,
                   fuse::physics::broadphase::CellCapacityRejectReason::ExceedsSpan),
               "over-span range rejects for ExceedsSpan");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellCapacityRejectReasonName(
                           "ExceedsSpan") == 0,
               "ExceedsSpan combined reject reason has stable label");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
                 fuse::physics::broadphase::cellCapacityRejectReason(inverted, 4u, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityRejectReason::EmptyRange),
             "inverted range reports EmptyRange combined reject reason");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {5, 1}};
    const fuse::physics::broadphase::CellCapacityPreflight planePreflight =
        fuse::physics::broadphase::preflightCellCapacity(planeRange, 8u, 4u);
    expectTrue(!planePreflight.canIterate(), "2D combined preflight rejects over-span range");
    expectTrue(planePreflight.exceedsSpan, "2D combined preflight marks exceedsSpan");

void testBroadphaseMergePlaneDynamicWithPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectTrue(!fuse::physics::broadphase::mergeBroadphasePlaneDynamicWithPreflight(bodies, shapes, buffer),
               "plane-dynamic merge with preflight skips empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
               "plane-dynamic merge with preflight skips plane-only scene");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::mergeBroadphasePlaneDynamicWithPreflight(bodies, shapes, buffer),
               "plane-dynamic merge with preflight runs on mergeable scene");
    expectTrue(buffer.activeCount >= 1u, "plane-dynamic merge with preflight emits plane-dynamic pair");
}

void testPairBufferShouldRunDedupeGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe false on empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe true when shouldRunPairBufferDedupe false");

    buffer.push(0u, 1u);
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe false on single pair");

    buffer.push(2u, 3u);
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe true for multiple pairs");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe false when shouldRunPairBufferDedupe true");
}

void testBroadphaseCountValidPairsGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(fuse::physics::broadphase::countValidBroadphasePairs(buffer), 0u,
             "countValidBroadphasePairs returns zero for empty buffer");
    expectTrue(!fuse::physics::broadphase::hasMultipleBroadphasePairs(buffer),
               "hasMultipleBroadphasePairs false for empty buffer");

    buffer.push(0u, 1u);
    expectEq(fuse::physics::broadphase::countValidBroadphasePairs(buffer), 1u,
             "countValidBroadphasePairs counts pushed pair");
               "hasMultipleBroadphasePairs false for single pair");

    buffer.push(2u, 3u);
    expectEq(fuse::physics::broadphase::countValidBroadphasePairs(buffer), 2u,
             "countValidBroadphasePairs counts multiple pairs");
    expectTrue(fuse::physics::broadphase::hasMultipleBroadphasePairs(buffer),
               "hasMultipleBroadphasePairs true for multiple pairs");
    expectTrue(fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase aligns with hasMultipleBroadphasePairs");
}

void testCellOccupancyPreflightCanSkipGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectTrue(!withinBudget.canSkip(), "within-budget preflight does not skip");
    expectTrue(withinBudget.canIterate(), "within-budget preflight can iterate");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectTrue(overBudget.canSkip(), "over-budget preflight can skip");
    expectTrue(!overBudget.canIterate(), "over-budget preflight cannot iterate");

void testBroadphaseMergePreflightCounts() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight planeOnly =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectEq(planeOnly.planeBodyCount, 1u, "plane-only scene reports one plane body");
    expectEq(planeOnly.dynamicBodyCount, 0u, "plane-only scene reports zero dynamic bodies");
    expectTrue(planeOnly.canSkip(), "plane-only merge preflight can skip");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight mergeable =
    expectEq(mergeable.planeBodyCount, 1u, "mergeable scene reports plane body count");
    expectEq(mergeable.dynamicBodyCount, 1u, "mergeable scene reports dynamic body count");
    expectTrue(!mergeable.canSkip(), "mergeable merge preflight does not skip");

void testPairBufferSortRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer sort reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort false on empty buffer");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSortRejectReasonName(
                               fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair sort reject reason has stable label");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
             "single pair reports SinglePair sort reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferSortRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
               "single pair rejects for SinglePair");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::None),
             "multiple pairs report None sort reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort true for multiple pairs");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort false for multiple pairs");

    const fuse::physics::broadphase::PairBufferSortPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectTrue(preflight.needsSort(), "sort preflight accepts multiple pairs with reason None");
    expectEq(static_cast<fuse::u32>(preflight.reason),
             "sort preflight carries reject reason");

void testPairBufferPushSkipGuards() {
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferPush(buffer, 0u, 1u),
               "shouldRunPairBufferPush accepts valid pair");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferPush(buffer, 0u, 1u),
               "canSkipPairBufferPush false for valid pair");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferPush(buffer, 1u, 1u),
               "canSkipPairBufferPush true for self-pair");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferPush(buffer, 1u, 1u),
               "shouldRunPairBufferPush false for self-pair");

    buffer.setMaxCapacity(1u);

    expectTrue(fuse::physics::broadphase::canSkipPairBufferPush(buffer, 2u, 3u),
               "canSkipPairBufferPush true when buffer is full");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferPush(buffer, 2u, 3u),
               "shouldRunPairBufferPush false when buffer is full");

void testPairBufferDedupeShouldRunGuards() {
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe false on empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe on empty buffer");

               "shouldRunPairBufferDedupe false for single pair");

    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe true for multiple pairs");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe false for multiple pairs");

void testShouldRunBroadphaseGuards() {

    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase true when shouldRunBroadphase false");

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase false when shouldRunBroadphase true");

void testRefineDedupeBroadphasePreflightGuards() {

    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightRefineDedupeBroadphase(bodies, shapes, buffer);
    expectTrue(!emptyPreflight.canRefine(), "combined preflight cannot refine empty scene");
    expectTrue(!emptyPreflight.canDedupe(), "combined preflight cannot dedupe empty buffer");
    expectTrue(!emptyPreflight.canRefineDedupe(), "combined preflight cannot refine+dedupe empty scene");
    expectTrue(fuse::physics::broadphase::canSkipRefineDedupeBroadphase(bodies, shapes, buffer),
               "canSkipRefineDedupeBroadphase on empty scene");
    expectTrue(!fuse::physics::broadphase::shouldRunRefineDedupeBroadphase(bodies, shapes, buffer),
               "shouldRunRefineDedupeBroadphase false on empty scene");

    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);

    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight singlePreflight =
    expectTrue(singlePreflight.canRefine(), "combined preflight can refine valid scene");
    expectTrue(!singlePreflight.canDedupe(), "combined preflight cannot dedupe single pair");
    expectTrue(!singlePreflight.canRefineDedupe(), "combined preflight cannot refine+dedupe single pair");

    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight multiPreflight =
    expectTrue(multiPreflight.canRefineDedupe(), "combined preflight can refine+dedupe multiple pairs");
    expectTrue(fuse::physics::broadphase::shouldRunRefineDedupeBroadphase(bodies, shapes, buffer),
               "shouldRunRefineDedupeBroadphase true for multiple pairs");

void testBroadphaseMergeLaunchPreflightGuards() {

    const fuse::physics::broadphase::BroadphaseMergeLaunchPreflight emptyLaunch =
        fuse::physics::broadphase::preflightBroadphaseMergeLaunch(bodies, shapes);
    expectTrue(!emptyLaunch.canRunBroadphase(), "merge launch preflight marks empty broadphase");
    expectTrue(!emptyLaunch.canMerge(), "merge launch preflight marks empty merge");
    expectTrue(!emptyLaunch.canLaunchMerge(), "merge launch preflight cannot launch on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMergeLaunch(bodies, shapes),
               "canSkipBroadphaseMergeLaunch on empty scene");

    const fuse::physics::broadphase::BroadphaseMergeLaunchPreflight planeOnlyLaunch =
    expectTrue(!planeOnlyLaunch.canRunBroadphase(), "singleton scene cannot run broadphase");
    expectTrue(!planeOnlyLaunch.canLaunchMerge(), "singleton scene cannot launch merge");

    const fuse::physics::broadphase::BroadphaseMergeLaunchPreflight launchPreflight =
    expectTrue(launchPreflight.canRunBroadphase(), "populated scene can run broadphase");
    expectTrue(launchPreflight.canMerge(), "plane plus dynamic scene can merge");
    expectTrue(launchPreflight.canLaunchMerge(), "merge launch preflight can launch mergeable scene");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMergeLaunch(bodies, shapes),
               "shouldRunBroadphaseMergeLaunch true for mergeable scene");


    expectTrue(!fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration false on empty scene");

    expectTrue(fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration true on populated scene");

void testCellSpanClampPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {3, 3, 3}};
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::cellSpanClampRejectReason(validRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::None),
             "valid range reports None span-clamp reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanClamp(validRange, 8u),
               "shouldRunCellSpanClamp true for clampable range");
    expectTrue(!fuse::physics::broadphase::canSkipCellSpanClamp(validRange, 8u),
               "canSkipCellSpanClamp false for clampable range");

                 fuse::physics::broadphase::cellSpanClampRejectReason(validRange, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::UnboundedSpan),
             "zero max span reports UnboundedSpan reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(validRange, 0u),
               "canSkipCellSpanClamp true for unbounded span");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellSpanClampRejectReasonName(
                               fuse::physics::broadphase::CellSpanClampRejectReason::EmptyRange),
                           "EmptyRange") == 0,
               "EmptyRange span-clamp reject reason has stable label");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::cellSpanRejectsForReason(
                   inverted, 4u, fuse::physics::broadphase::CellSpanClampRejectReason::EmptyRange),
               "inverted range rejects for EmptyRange span clamp");

    const fuse::physics::broadphase::CellSpanClampPreflight preflight =
        fuse::physics::broadphase::preflightCellSpanClamp(validRange, 8u);
    expectTrue(preflight.canClamp(), "span-clamp preflight accepts clampable range");
             "span-clamp preflight carries reject reason");





void testPairBufferSlotWriteRejectReasonGuards() {
    buffer.preparePairSlots(2u);

                 fuse::physics::broadphase::pairBufferSlotWriteRejectReason(buffer, 0u, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSlotWriteRejectReason::InvalidPair),
             "self-pair reports InvalidPair slot-write reject reason");
                 fuse::physics::broadphase::pairBufferSlotWriteRejectReason(buffer, 4u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSlotWriteRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSlotWriteRejectReasonName(
                               fuse::physics::broadphase::PairBufferSlotWriteRejectReason::OutOfRangeSlot),
                           "OutOfRangeSlot") == 0,
               "OutOfRangeSlot slot-write reject reason has stable label");

    const fuse::physics::broadphase::PairBufferSlotWritePreflight validPreflight =
        fuse::physics::broadphase::preflightPairBufferSlotWrite(buffer, 0u, 0u, 1u);
    expectTrue(validPreflight.canWrite(), "slot-write preflight accepts valid pair");
    buffer.writeSlot(0u, 0u, 1u);
    expectTrue(buffer.slotIsValid(0u), "writeSlot succeeds through preflight gate");

void testPairBufferMergeIntoRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferMergeIntoRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferMergeIntoRejectReason::EmptyInput),
             "zero pair count reports EmptyInput merge-into reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferMergeInto(buffer, 0u),
               "canSkipPairBufferMergeInto on empty input");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferMergeIntoRejectReason(buffer, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferMergeIntoRejectReason::BufferFull),
             "full buffer reports BufferFull merge-into reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferMergeInto(buffer, 1u),
               "shouldRunPairBufferMergeInto false when buffer full");

    fuse::physics::broadphase::PairBufferSoA openBuffer;
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferMergeInto(openBuffer, 2u),
               "shouldRunPairBufferMergeInto true when buffer accepts pairs");
    const fuse::physics::broadphase::PairBufferMergeIntoPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferMergeInto(openBuffer, 2u);
    expectTrue(preflight.canMerge(), "merge-into preflight accepts non-empty input into open buffer");

void testShouldRunPairBufferDedupeGuards() {
               "canSkipPairBufferDedupe mirrors shouldRunPairBufferDedupe on empty buffer");


               "canSkipPairBufferDedupe false when shouldRunPairBufferDedupe true");

void testBroadphaseMergeIntoBufferPreflightGuards() {

                 fuse::physics::broadphase::mergeIntoBufferBroadphaseRejectReason(bodies, shapes, buffer)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::BroadphaseMergeIntoBufferRejectReason::SceneNotMergeable),
             "empty scene reports SceneNotMergeable merge-into-buffer reject reason");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMergeIntoBuffer(bodies, shapes, buffer),
               "canSkipBroadphaseMergeIntoBuffer on empty scene");


             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeIntoBufferRejectReason::BufferFull),
             "full buffer reports BufferFull merge-into-buffer reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::mergeIntoBufferBroadphaseRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeIntoBufferRejectReason::BufferFull),
                           "BufferFull") == 0,
               "BufferFull merge-into-buffer reject reason has stable label");

    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, openBuffer);
    expectTrue(preflight.canMerge(), "merge-into-buffer preflight accepts mergeable scene with open buffer");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMergeIntoBuffer(bodies, shapes, openBuffer),
               "shouldRunBroadphaseMergeIntoBuffer true for mergeable scene with capacity");



             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::AlreadySorted),
             "canonical-order multiple pairs report AlreadySorted sort reject reason");

    fuse::physics::broadphase::PairBufferSoA unsortedBuffer;
    unsortedBuffer.push(2u, 3u);
    unsortedBuffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(unsortedBuffer)),
             "unsorted multiple pairs report None sort reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(unsortedBuffer),
               "shouldRunPairBufferSort true for unsorted multiple pairs");

        fuse::physics::broadphase::preflightPairBufferSort(unsortedBuffer);


               "canSkipPairBufferDedupe true for single pair");


void testPairBufferCompactClampRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compact-clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompactClamp(buffer),
               "canSkipPairBufferCompactClamp on empty buffer");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactClampRejectReason::NoWork),
             "all-valid within-capacity buffer reports NoWork compact-clamp reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactClampRejectReasonName(
                               fuse::physics::broadphase::PairBufferCompactClampRejectReason::NoWork),
                           "NoWork") == 0,
               "NoWork compact-clamp reject reason has stable label");

    fuse::physics::broadphase::PairBufferSoA workBuffer;
    workBuffer.setMaxCapacity(1u);
    workBuffer.preparePairSlots(2u);
    workBuffer.writeSlot(0u, 0u, 1u);
    const fuse::physics::broadphase::PairBufferCompactClampPreflight workPreflight =
        fuse::physics::broadphase::preflightPairBufferCompactClamp(workBuffer);
    expectTrue(workPreflight.canRun(), "compact-clamp preflight accepts compaction work");
    expectTrue(workPreflight.needsCompaction, "compact-clamp preflight marks compaction needed");
    expectEq(workBuffer.compactAndClamp(), 1u, "compactAndClamp uses preflight gate");



               "shouldRunBroadphase false on singleton scene");


void testCellOccupancyPreflightBudgetRemaining() {
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 12u);
    expectEq(withinBudget.budgetRemaining, 4u, "preflight reports occupancy budget remaining");
    expectEq(withinBudget.occupancyCount, 8u, "preflight reports occupancy count");

    const fuse::physics::broadphase::CellOccupancyPreflight emptyPreflight =
        fuse::physics::broadphase::preflightCellOccupancy(inverted, 4u);
    expectEq(emptyPreflight.budgetRemaining, 4u, "empty range leaves full budget in preflight");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight planePreflight =
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 10u);
    expectEq(planePreflight.budgetRemaining, 2u, "2D preflight reports occupancy budget remaining");

void testRefineBroadphaseActivePairCountPreflight() {

    const fuse::physics::broadphase::RefineBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightRefineBroadphase(bodies, shapes, buffer);
    expectEq(emptyPreflight.activePairCount, 0u, "refine preflight reports zero active pairs on empty buffer");

    buffer.push(0u, 2u);

    const fuse::physics::broadphase::RefineBroadphasePreflight validPreflight =
    expectEq(validPreflight.activePairCount, 2u, "refine preflight reports active pair count");
    expectTrue(validPreflight.canRefine(), "refine preflight accepts valid scene with pair count");

void testBroadphaseMergeStatsPreflight() {

    const fuse::physics::broadphase::BroadphaseMergePreflight emptyPreflight =
    expectEq(emptyPreflight.stats.planeBodyCount, 0u, "empty scene has zero plane bodies");
    expectEq(emptyPreflight.stats.dynamicBodyCount, 0u, "empty scene has zero dynamic bodies");

    const fuse::physics::broadphase::BroadphaseMergePreflight planeOnlyPreflight =
    expectEq(planeOnlyPreflight.stats.planeBodyCount, 1u, "plane-only scene reports one plane body");
    expectEq(planeOnlyPreflight.stats.dynamicBodyCount, 0u, "plane-only scene reports zero dynamic bodies");

    const fuse::physics::broadphase::BroadphaseMergePreflight mergePreflight =
    expectEq(mergePreflight.stats.planeBodyCount, 1u, "merge scene reports plane body count");
    expectEq(mergePreflight.stats.dynamicBodyCount, 1u, "merge scene reports dynamic body count");
    expectTrue(mergePreflight.canMerge(), "merge preflight accepts scene with plane and dynamic counts");

void testPairBufferWriteSlotRejectReasonGuards() {

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
             "valid write-slot reports None reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferWriteSlotRejectsForReason(
                   buffer, 0u, 0u, 1u, fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
               "valid write-slot rejects for None");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 2u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidSlot),
             "out-of-range slot reports InvalidSlot reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferWriteSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidSlot),
                           "InvalidSlot") == 0,
               "InvalidSlot write-slot reject reason has stable label");
             "in-range slot with valid pair reports None write-slot reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "shouldRunPairBufferWriteSlot true for valid write");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 4u, 0u, 1u)),
                 fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 4u, 0u, 1u),
               "canSkipPairBufferWriteSlot true for out-of-range slot");
             "out-of-range slot reports OutOfRangeSlot write reject reason");
                               fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
                           "InvalidPair") == 0,
               "InvalidPair write-slot reject reason has stable label");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
             "self-pair write-slot reports InvalidPair reject reason");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 0u, 1u);
    expectTrue(preflight.canWrite(), "write-slot preflight accepts valid slot");
             "self-pair reports InvalidPair write-slot reject reason");

        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 1u, 2u, 3u);
    expectTrue(preflight.canWrite(), "write-slot preflight accepts valid slot write");
             "write-slot preflight carries reject reason");




    expectTrue(preflight.needsSort(), "sort preflight requests work for multiple pairs");

void testPairBufferAcceptPairsRejectReasonGuards() {
    buffer.setMaxCapacity(2u);

                 fuse::physics::broadphase::pairBufferAcceptPairsRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::None),
             "zero additional count vacuously reports None accept-pairs reject reason");
    expectTrue(buffer.canAcceptPairs(0u), "zero additional count is accepted");

    const fuse::physics::broadphase::PairBufferAcceptPairsPreflight emptyAccept =
        fuse::physics::broadphase::preflightPairBufferAcceptPairs(buffer, 2u);
    expectTrue(emptyAccept.canAccept(), "empty buffer accepts two pairs");
    expectTrue(fuse::physics::broadphase::shouldAcceptPairBufferPairs(buffer, 2u),
               "shouldAcceptPairBufferPairs true for fitting count");

                 fuse::physics::broadphase::pairBufferAcceptPairsRejectReason(buffer, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::ExceedsCapacity),
             "partial buffer reports ExceedsCapacity for two more pairs");
    expectTrue(fuse::physics::broadphase::pairBufferAcceptPairsRejectsForReason(
                   buffer, 2u, fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::ExceedsCapacity),
               "partial buffer rejects for ExceedsCapacity");
    expectTrue(!buffer.canAcceptPairs(2u), "partial buffer rejects excess pairs");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferAcceptPairs(buffer, 2u),
               "canSkipPairBufferAcceptPairs true when exceeds capacity");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferAcceptPairsRejectReasonName(
                               fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::ExceedsCapacity),
                           "ExceedsCapacity") == 0,
               "ExceedsCapacity accept-pairs reject reason has stable label");

void testCellSpanClampRejectReasonGuards() {
             "valid range reports None cell-span clamp reject reason");
               "shouldRunCellSpanClamp true for valid range with span limit");

             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::UnlimitedSpan),
             "unlimited span reports UnlimitedSpan reject reason");
               "canSkipCellSpanClamp true for unlimited span");
                               fuse::physics::broadphase::CellSpanClampRejectReason::UnlimitedSpan),
                           "UnlimitedSpan") == 0,
               "UnlimitedSpan cell-span clamp reject reason has stable label");

                 fuse::physics::broadphase::cellSpanClampRejectReason(inverted, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::EmptyRange),
             "inverted range reports EmptyRange cell-span clamp reject reason");
    expectTrue(fuse::physics::broadphase::cellSpanClampRejectsForReason(
               "inverted range rejects for EmptyRange");

        fuse::physics::broadphase::preflightCellSpanClamp(validRange, 4u);
    expectTrue(preflight.needsClamp(), "cell-span clamp preflight requests clamp for valid range");
             "cell-span clamp preflight carries reject reason");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {10, 10}};
    const fuse::physics::broadphase::CellRange2 clamped =
        fuse::physics::broadphase::clampCellRange2(planeRange, 4u);
    expectTrue(clamped.maxCell.x - clamped.minCell.x <= 4, "clampCellRange2 applies span limit via preflight gate");

void testMergePairsIntoBufferPartialCapacityPreflight() {

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{2u, 3u}, {4u, 5u}};
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight preflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, buffer);
    expectTrue(preflight.bufferFull, "full buffer merge preflight marks buffer full");
    expectTrue(!preflight.canMerge(), "full buffer merge preflight cannot merge");

    fuse::physics::broadphase::PairBufferSoA partialBuffer;
    partialBuffer.setMaxCapacity(2u);
    partialBuffer.push(0u, 1u);
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight partialPreflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, partialBuffer);
    expectTrue(partialPreflight.canMerge(), "partial-capacity buffer can merge some pairs");
    expectEq(partialPreflight.mergeablePairCount, 1u, "partial-capacity preflight counts mergeable pairs");
    expectTrue(partialPreflight.partialCapacity, "partial-capacity preflight marks partial merge");
    expectEq(partialPreflight.requestedPairCount, 2u, "partial-capacity preflight records requested count");
             "self-pair reports InvalidPair write reject reason");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight validPreflight =

    expectTrue(validPreflight.canWrite(), "write-slot preflight accepts valid pair");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferWriteSlot(buffer, 1u, 2u, 3u),
               "shouldRunPairBufferWriteSlot true for valid pair");

    buffer.writeSlot(0u, 1u, 1u);
    expectTrue(!buffer.slotIsValid(0u), "writeSlot rejects self-pair via preflight gate");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u),
               "canSkipPairBufferWriteSlot true for self-pair");

void testPairBufferInvalidateSlotRejectReasonGuards() {

    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferPush(buffer, 2u, 3u),
               "wouldSkipPairBufferPush true when buffer is full");

    fuse::physics::broadphase::PairBufferPushRejectReason reason =
        fuse::physics::broadphase::PairBufferPushRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferPush(buffer, 2u, 3u, &reason),
               "wouldSkipPairBufferPush writes reject reason");
    expectEq(static_cast<fuse::u32>(reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "wouldSkipPairBufferPush reports AtCapacity");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferPush(buffer, 2u, 3u) ==
                   fuse::physics::broadphase::canSkipPairBufferPush(buffer, 2u, 3u),
               "wouldSkipPairBufferPush agrees with canSkipPairBufferPush");


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
               "OutOfRangeSlot invalidate reject reason has stable label");

    const fuse::physics::broadphase::PairBufferInvalidateSlotPreflight validPreflight =
        fuse::physics::broadphase::preflightPairBufferInvalidateSlot(buffer, 1u);
    expectTrue(validPreflight.canInvalidate(), "invalidate-slot preflight accepts in-range slot");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 1u),
               "shouldRunPairBufferInvalidateSlot true for in-range slot");

    buffer.invalidateSlot(1u);
    expectTrue(!buffer.slotIsValid(1u), "invalidateSlot clears valid flag via preflight gate");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 8u),
               "canSkipPairBufferInvalidateSlot true for out-of-range slot");

    fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason reason =
        fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u, &reason),
               "wouldSkipPairBufferInvalidateSlot true before first write");
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
             "unwritten slot reports AlreadyInvalid before first write");


    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u),
               "wouldSkipPairBufferInvalidateSlot false for valid slot");
void testPairBufferInvalidateSlotPreflightGuards() {


             "unwritten slot reports AlreadyInvalid invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "canSkipPairBufferInvalidateSlot true for unwritten slot");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot false for unwritten slot");


             "valid slot reports None invalidate reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot true for valid slot");

    buffer.invalidateSlot(0u);
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears valid slot via preflight gate");
               "wouldSkipPairBufferInvalidateSlot true for already-invalid slot");
             "already-invalid slot reports AlreadyInvalid reject reason");
                   buffer, 0u,
                   fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
               "already-invalid slot rejects for AlreadyInvalid");

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 2u)),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 2u),
    buffer.invalidateSlot(2u);
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot ignores out-of-range slot");


void testPairBufferWouldSkipWriteAndPushGuards() {
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "canSkipPairBufferInvalidateSlot on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot false on empty buffer");


             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
                   buffer, 2u,
               "out-of-range slot rejects for OutOfRangeSlot");

    expectTrue(buffer.slotIsValid(0u), "invalidateSlot ignores out-of-range slot via preflight gate");
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears in-range slot via preflight gate");

void testWouldSkipPairBufferWriteInvalidateGuards() {
    expectEq(static_cast<fuse::u32>(
             "out-of-range slot reports OutOfRangeSlot invalidate reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateSlotRejectsForReason(
                   fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReasonName(
                           "AlreadyInvalid") == 0,
               "AlreadyInvalid invalidate reject reason has stable label");

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
             "invalidated slot reports AlreadyInvalid invalidate reject reason");

    const fuse::physics::broadphase::PairBufferInvalidateSlotPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferInvalidateSlot(buffer, 1u);
    expectTrue(preflight.alreadyInvalid, "invalidate preflight marks already-invalid slot");
    expectTrue(!preflight.canInvalidate(), "invalidate preflight cannot invalidate already-invalid slot");
}

void testPairBufferWouldSkipWriteAndInvalidateGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(1u);

    fuse::physics::broadphase::PairBufferWriteSlotRejectReason writeReason =
        fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u, &writeReason),
               "wouldSkipPairBufferWriteSlot false for valid write");
    expectEq(static_cast<fuse::u32>(writeReason),
             "valid write reports None write-slot reject reason");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u),
               "wouldSkipPairBufferWriteSlot true for self-pair");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u) ==
                   fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "wouldSkipPairBufferWriteSlot agrees with canSkipPairBufferWriteSlot for valid write");

    fuse::physics::broadphase::PairBufferPushRejectReason pushReason =
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferPush(buffer, 0u, 1u, &pushReason),
               "wouldSkipPairBufferPush false for valid push");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferPush(buffer, 2u, 2u, &pushReason),
               "wouldSkipPairBufferPush true for self-pair");
    expectEq(static_cast<fuse::u32>(pushReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
             "self-pair push reports InvalidPair reject reason");

    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferPush(buffer, 2u, 3u, &pushReason),
             "full-buffer push reports AtCapacity reject reason");

void testCellCapacityWouldSkipGuards() {

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
             "wouldSkipPairBufferWriteSlot reports None for valid write");

    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u, &writeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
             "wouldSkipPairBufferWriteSlot reports InvalidPair for self-pair");
               "wouldSkipPairBufferWriteSlot agrees with canSkipPairBufferWriteSlot");

    fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason invalidateReason =
        fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u, &invalidateReason),
               "wouldSkipPairBufferInvalidateSlot false for in-range slot");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u) ==
                   fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u),

    buffer.writeSlot(0u, 0u, 1u);
               "wouldSkipPairBufferInvalidateSlot false for valid slot");
    expectEq(static_cast<fuse::u32>(invalidateReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None),
             "wouldSkipPairBufferInvalidateSlot reports None for valid slot");

    buffer.invalidateSlot(0u);
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u, &invalidateReason),
             "wouldSkipPairBufferInvalidateSlot reports AlreadyInvalid");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u) ==
                   fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "wouldSkipPairBufferInvalidateSlot agrees with canSkipPairBufferInvalidateSlot");

void testWouldSkipCellCapacityGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    fuse::physics::broadphase::CellOccupancyRejectReason occupancyReason =
        fuse::physics::broadphase::CellOccupancyRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 8u, &occupancyReason),
               "wouldSkipCellOccupancyIteration false within budget");
    expectEq(static_cast<fuse::u32>(occupancyReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "within-budget occupancy reports None reject reason");
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u, &occupancyReason),
               "wouldSkipCellOccupancyIteration true over budget");
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "over-budget occupancy reports ExceedsBudget reject reason");
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u) ==
                   fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "wouldSkipCellOccupancyIteration agrees with canSkipCellOccupancyIteration over budget");

    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(planeRange, 4u),
               "2D wouldSkipCellOccupancyIteration true over budget");

    fuse::physics::broadphase::CellSpanRejectReason spanReason =
        fuse::physics::broadphase::CellSpanRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(validRange, 4u, &spanReason),
               "wouldSkipCellSpanClamp true within span limit");
    expectEq(static_cast<fuse::u32>(spanReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::None),
             "within-limit span reports None reject reason");
    expectTrue(!fuse::physics::broadphase::wouldSkipCellSpanClamp(validRange, 1u, &spanReason),
               "wouldSkipCellSpanClamp false when span exceeds budget");
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpan),
             "over-span range reports ExceedsSpan reject reason");
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(validRange, 1u) ==
                   fuse::physics::broadphase::canSkipCellSpanClamp(validRange, 1u),
               "wouldSkipCellSpanClamp agrees with canSkipCellSpanClamp over budget");

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


    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 99u, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 128;

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
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {256.f, 0.f, 0.f});
    params.maxCellSpanPerAxis = 0u;
    params.maxCellOccupancy = 8u;

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

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    params.maxCellOccupancy = 0u;

    const fuse::physics::broadphase::ShapeCellInsertPreflight validPreflight =
        fuse::physics::broadphase::preflightShapeCellInsert(1u, bodies, shapes, params, false);
    expectTrue(validPreflight.canInsert(), "normal shape insert preflight can insert");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsert(1u, bodies, shapes, params, false),
               "shouldRunShapeCellInsert true for normal shape");
    expectTrue(validPreflight.occupancyCount > 0u, "normal shape insert preflight reports occupancy count");


                   buffer, 0u, 0u, 1u,
                   fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),

               "OutOfRangeSlot write reject reason has stable label");


               "shouldRunPairBufferWriteSlot true for valid slot");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "canSkipPairBufferWriteSlot false for valid slot");

    buffer.writeSlot(99u, 0u, 1u);
    expectTrue(!buffer.slotIsValid(99u), "writeSlot rejects out-of-range slot via preflight gate");


    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),


                   buffer, 99u,
               "far out-of-range slot rejects for OutOfRangeSlot");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 99u),
               "canSkipPairBufferInvalidateSlot on out-of-range slot");

    expectTrue(preflight.canInvalidate(), "invalidate preflight accepts in-range empty slot");

                 fuse::physics::broadphase::cellPairGenRejectReason(emptyOccupants)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::EmptyOccupants),
             "empty occupants report EmptyOccupants cell-pair reject reason");
               "canSkipCellPairGeneration on empty occupants");
    expectEq(fuse::physics::broadphase::estimatePairCountForCell(emptyOccupants), 0u,
             "estimatePairCountForCell returns zero for empty occupants");

    const std::vector<fuse::u32> singleOccupant = {3u};
                 fuse::physics::broadphase::cellPairGenRejectReason(singleOccupant)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::SingleOccupant),
             "single occupant reports SingleOccupant cell-pair reject reason");
                               fuse::physics::broadphase::CellPairGenRejectReason::SingleOccupant),
                           "SingleOccupant") == 0,
               "SingleOccupant cell-pair reject reason has stable label");

    const std::vector<fuse::u32> duplicateOccupants = {1u, 1u, 1u};
                 fuse::physics::broadphase::cellPairGenRejectReason(duplicateOccupants)),
             "duplicate occupants collapse to single-occupant reject reason");

    const std::vector<fuse::u32> multiOccupants = {0u, 1u, 0u, 2u};
    const fuse::physics::broadphase::CellPairGenPreflight preflight =
    expectTrue(preflight.canGenerate(), "multi-occupant cell-pair preflight can generate");
    expectEq(preflight.uniqueBodyCount, 3u, "cell-pair preflight reports unique body count");
    expectEq(preflight.pairCount, 3u, "cell-pair preflight reports canonical pair count");
    expectEq(fuse::physics::broadphase::estimatePairCountForCell(multiOccupants), 3u,
             "estimatePairCountForCell matches unique-body pair count");
               "shouldRunCellPairGeneration true for multi-occupant cell");

void testCellCapacityInsertRejectReasonGuards() {

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellCapacityInsertRejectReason(
                 2u, 2u, validRange, 8u)),
                 fuse::physics::broadphase::CellCapacityInsertRejectReason::OutOfRangeBody),
             "out-of-range body reports OutOfRangeBody insert reject reason");
    expectTrue(fuse::physics::broadphase::cellCapacityInsertRejectsForReason(
                   2u, 2u, validRange, 8u,
               "out-of-range body rejects for OutOfRangeBody");

                 0u, 2u, validRange, 7u)),
                 fuse::physics::broadphase::CellCapacityInsertRejectReason::ExceedsOccupancyBudget),
             "over-budget range reports ExceedsOccupancyBudget insert reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellCapacityInsertRejectReasonName(
                               fuse::physics::broadphase::CellCapacityInsertRejectReason::
                                   ExceedsOccupancyBudget),
                           "ExceedsOccupancyBudget") == 0,
               "ExceedsOccupancyBudget insert reject reason has stable label");

    const fuse::physics::broadphase::CellCapacityInsertPreflight preflight =
        fuse::physics::broadphase::preflightCellCapacityInsert(0u, 2u, validRange, 8u);
    expectTrue(preflight.canInsert(), "cell-capacity insert preflight accepts valid body and range");
    expectTrue(fuse::physics::broadphase::shouldRunCellCapacityInsert(0u, 2u, validRange, 8u),
               "shouldRunCellCapacityInsert true within budget");
    expectTrue(fuse::physics::broadphase::canSkipCellCapacityInsert(0u, 2u, validRange, 7u),
               "canSkipCellCapacityInsert true over budget");

                 0u, 2u, planeRange, 4u)),
             "2D over-budget range reports ExceedsOccupancyBudget insert reject reason");

void testPairBufferWriteRejectReasonGuards() {

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 0u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteRejectReason::None),
             "in-range valid slot reports None write reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferWrite(buffer, 0u, 0u, 1u),
               "shouldRunPairBufferWrite true for valid slot");

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 2u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteRejectReason::OutOfRangeSlot),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferWriteRejectReasonName(
                               fuse::physics::broadphase::PairBufferWriteRejectReason::OutOfRangeSlot),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWrite(buffer, 2u, 0u, 1u),
               "canSkipPairBufferWrite true for out-of-range slot");

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 0u, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteRejectReason::InvalidPair),

    const fuse::physics::broadphase::PairBufferWritePreflight preflight =
        fuse::physics::broadphase::preflightPairBufferWrite(buffer, 0u, 0u, 1u);
    expectTrue(preflight.canWrite(), "write preflight accepts valid slot with reason None");
             "write preflight carries reject reason");

    expectTrue(!buffer.slotIsValid(0u), "writeSlot rejects out-of-range slot via preflight gate");
    buffer.writeSlot(0u, 2u, 2u);
    expectTrue(!buffer.slotIsValid(0u), "writeSlot rejects invalid pair via preflight gate");
    expectTrue(buffer.slotIsValid(0u), "writeSlot accepts valid pair via preflight gate");

void testPairBufferInvalidateRejectReasonGuards() {

                 fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateRejectReason::None),
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidate(buffer, 0u),
               "shouldRunPairBufferInvalidate true for in-range slot");

                 fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateRejectReason::OutOfRangeSlot),
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateRejectsForReason(
                   buffer, 2u, fuse::physics::broadphase::PairBufferInvalidateRejectReason::OutOfRangeSlot),
               "invalidate rejects for OutOfRangeSlot");


    const fuse::physics::broadphase::PairBufferInvalidatePreflight preflight =
        fuse::physics::broadphase::preflightPairBufferInvalidate(buffer, 1u);
    expectTrue(preflight.canInvalidate(), "invalidate preflight accepts in-range untouched slot");

             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::InsufficientOccupants),
             "empty occupants report InsufficientOccupants cell-pair reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellPairGen(emptyOccupants),
               "canSkipCellPairGen on empty occupants");
    expectEq(fuse::physics::broadphase::countPairsForOccupants(emptyOccupants), 0u,
             "countPairsForOccupants returns zero for empty occupants");

    const std::vector<fuse::u32> singletonOccupants = {1u};
                 fuse::physics::broadphase::cellPairGenRejectReason(singletonOccupants)),
             "singleton occupant reports InsufficientOccupants cell-pair reject reason");

             "duplicate-only occupants report InsufficientOccupants cell-pair reject reason");
    expectEq(fuse::physics::broadphase::uniqueOccupantCount(duplicateOccupants), 1u,
             "uniqueOccupantCount dedupes duplicate occupants");

    const std::vector<fuse::u32> pairOccupants = {0u, 1u, 0u};
                 fuse::physics::broadphase::cellPairGenRejectReason(pairOccupants)),
             "two unique occupants report None cell-pair reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGen(pairOccupants),
               "shouldRunCellPairGen true for two unique occupants");
    expectEq(fuse::physics::broadphase::countPairsForOccupants(pairOccupants), 1u,
             "countPairsForOccupants returns one pair for two unique bodies");

    const std::vector<fuse::u32> tripleOccupants = {0u, 1u, 2u};
    expectEq(fuse::physics::broadphase::countPairsForOccupants(tripleOccupants), 3u,
             "countPairsForOccupants returns three pairs for three unique bodies");

        fuse::physics::broadphase::preflightCellPairGen(tripleOccupants);
    expectTrue(preflight.canGenerate(), "cell-pair preflight accepts triple occupants");
    expectEq(preflight.pairCount, 3u, "cell-pair preflight reports pair count");
                               fuse::physics::broadphase::CellPairGenRejectReason::InsufficientOccupants),
                           "InsufficientOccupants") == 0,
               "InsufficientOccupants cell-pair reject reason has stable label");

    params.maxCellOccupancy = 4u;

             "orphan shape reports OutOfRangeBody cell-capacity insert reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellCapacityInsert(0u, bodies, shapes, params, false),
               "canSkipCellCapacityInsert on orphan shape");

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {512.f, 0.f, 0.f});
    params.maxCellSpanPerAxis = 4u;
    params.cellSize = 1.f;
                 fuse::physics::broadphase::CellCapacityInsertRejectReason::OccupancyRejected),
             "oversized shape reports OccupancyRejected cell-capacity insert reject reason");
               "cell-capacity insert rejects for OccupancyRejected");

    fuse::physics::RigidBodySoA smallBodies;
    fuse::physics::CollisionShapeSoA smallShapes;
    smallBodies.addBody({0.f, 0.f, 0.f}, 1.f);
    smallBodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    smallShapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    smallShapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams smallParams;
    smallParams.cellSize = 1.f;
    smallParams.maxCellSpanPerAxis = 4u;
    smallParams.maxCellOccupancy = 64u;

                 1u, smallBodies, smallShapes, smallParams, false)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityInsertRejectReason::None),
             "small shape reports None cell-capacity insert reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellCapacityInsert(
                   1u, smallBodies, smallShapes, smallParams, false),
               "shouldRunCellCapacityInsert true for small shape");

        fuse::physics::broadphase::preflightCellCapacityInsert(
            1u, smallBodies, smallShapes, smallParams, false);
    expectTrue(preflight.canInsert(), "cell-capacity insert preflight accepts small shape");
                           "OccupancyRejected") == 0,
               "OccupancyRejected cell-capacity insert reject reason has stable label");






















































             "valid write reports None reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferWriteRejectsForReason(
                   buffer, 0u, 0u, 1u, fuse::physics::broadphase::PairBufferWriteRejectReason::None),
               "valid write rejects for None");

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 4u, 0u, 1u)),


    expectTrue(preflight.canWrite(), "write preflight accepts valid slot");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWrite(buffer, 4u, 0u, 1u),

    expectTrue(buffer.slotIsValid(0u), "writeSlot writes via preflight gate");
    expectTrue(buffer.slotIsValid(0u), "writeSlot self-pair rejection leaves prior slot intact");
    expectEq(buffer.bodyA[0u], 0u, "writeSlot self-pair rejection preserves prior bodyA");
    expectEq(buffer.bodyB[0u], 1u, "writeSlot self-pair rejection preserves prior bodyB");


             "valid slot reports None invalidate reject reason");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateRejectReason::AlreadyInvalid),
             "invalidated slot reports AlreadyInvalid reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateRejectReasonName(
                               fuse::physics::broadphase::PairBufferInvalidateRejectReason::AlreadyInvalid),

                 fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 4u)),

    buffer.writeSlot(1u, 2u, 3u);
    expectTrue(preflight.canInvalidate(), "invalidate preflight accepts valid written slot");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidate(buffer, 1u),
               "shouldRunPairBufferInvalidate true for valid written slot");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidate(buffer, 0u),
               "canSkipPairBufferInvalidate true for already-invalid slot");

    fuse::physics::broadphase::PairBufferSoA freshBuffer;
    freshBuffer.preparePairSlots(1u);
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidate(freshBuffer, 0u),
               "canSkipPairBufferInvalidate true for prepared-but-unwritten slot");

void testCellPairGenPreflightGuards() {
             "zero occupants report EmptyOccupants cell-pair reject reason");

    const std::vector<fuse::u32> singleOccupant = {1u};
             "single occupant reports InsufficientOccupants cell-pair reject reason");

        fuse::physics::broadphase::preflightCellPairGen(multiOccupants);
    expectTrue(preflight.canGenerate(), "cell-pair preflight accepts multiple unique occupants");
    expectEq(preflight.uniqueOccupantCount, 3u, "cell-pair preflight dedupes occupants");
    expectEq(preflight.pairCount, 3u, "cell-pair preflight reports n*(n-1)/2 pair count");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGen(multiOccupants),
               "shouldRunCellPairGen true for multi-occupant cell");
    expectEq(fuse::physics::broadphase::estimatePairsForUniqueOccupants(4u), 6u,
             "estimatePairsForUniqueOccupants matches unique-body estimate");

void testShapeCellInsertPreflightGuards() {


    params.tableSize = 64;


    const fuse::physics::broadphase::ShapeCellInsertPreflight overBudget =
    expectTrue(!overBudget.canInsert(), "huge sphere preflight rejects over-budget insert");
    expectTrue(overBudget.exceedsOccupancyBudget, "over-budget preflight marks exceedsOccupancyBudget");
               "canSkipShapeCellInsert true when over budget");

    const fuse::physics::broadphase::ShapeCellInsertPreflight validInsert =
    expectTrue(validInsert.canInsert(), "span-clamped sphere preflight can insert");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsert(0u, bodies, shapes, params, false),
               "shouldRunShapeCellInsert true for valid insert");
                               fuse::physics::broadphase::ShapeCellInsertRejectReason::ExceedsOccupancyBudget),



























void testRefineInvalidateSlotPreflightGuards() {

    bodies.addBody({20.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 2, {1.f, 0.f, 0.f});

               "refine path can invalidate in-range slot");
               "refine path skips out-of-range invalidate slot");

    fuse::physics::broadphase::refineBroadphasePairsParallel(bodies, shapes, buffer);
    expectEq(buffer.activeCount, 1u, "refine invalidate preflight gate removes separated pair");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "refine invalidate preflight gate keeps overlap");

void testPairBufferShouldRunDedupeGuards() {
               "canSkipPairBufferDedupe true when shouldRunPairBufferDedupe false");

               "shouldRunPairBufferDedupe false on single pair");

               "shouldRunPairBufferDedupe false for already-unique pairs");

               "shouldRunPairBufferDedupe true for duplicate pairs");

void testPairBufferMergeRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferMergeRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferMergeRejectReason::EmptyPairs),
             "zero pair count reports EmptyPairs merge reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferMerge(buffer, 0u),
               "canSkipPairBufferMerge on empty pair list");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferMerge(buffer, 0u),
               "shouldRunPairBufferMerge false on empty pair list");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferMergeRejectReason(buffer, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferMergeRejectReason::AtCapacity),
             "full buffer reports AtCapacity merge reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferMergeRejectReasonName(
                               fuse::physics::broadphase::PairBufferMergeRejectReason::AtCapacity),
                           "AtCapacity") == 0,
               "AtCapacity merge reject reason has stable label");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferMergeRejectReason(openBuffer, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferMergeRejectReason::None),
             "open buffer with pairs reports None merge reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferMerge(openBuffer, 2u),
               "shouldRunPairBufferMerge true when buffer can accept pairs");

    const fuse::physics::broadphase::PairBufferMergePreflight preflight =
        fuse::physics::broadphase::preflightPairBufferMerge(openBuffer, 2u);
    expectTrue(preflight.canMerge(), "merge preflight accepts open buffer with pairs");

void testCellSpanRejectReasonGuards() {
    expectTrue(fuse::physics::broadphase::cellSpanWithinLimit(validRange, 2u),
               "small range is within span limit");
    expectTrue(!fuse::physics::broadphase::exceedsCellSpanPerAxis(validRange, 2u),
               "small range does not exceed span limit");
                 fuse::physics::broadphase::cellSpanRejectReason(validRange, 2u)),
             "valid range reports None span reject reason");

    const fuse::physics::broadphase::CellRange3 wideRange = {{0, 0, 0}, {5, 0, 0}};
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(wideRange, 4u),
               "wide range exceeds span limit");
                   wideRange, 4u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpan),
               "wide range rejects for ExceedsSpan");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellSpanRejectReasonName(
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpan),
                           "ExceedsSpan") == 0,
               "ExceedsSpan reject reason has stable label");

                 fuse::physics::broadphase::cellSpanRejectReason(inverted, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
             "inverted range reports EmptyRange span reject reason");

    const fuse::physics::broadphase::CellSpanPreflight preflight =
        fuse::physics::broadphase::preflightCellSpan(validRange, 2u);
    expectTrue(preflight.canIterate(), "span preflight accepts valid range");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanIteration(validRange, 2u),
               "shouldRunCellSpanIteration true within span limit");
    expectTrue(!fuse::physics::broadphase::canSkipCellSpanIteration(validRange, 2u),
               "canSkipCellSpanIteration false within span limit");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {5, 0}};
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(planeRange, 4u),
               "2D wide range exceeds span limit");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanIteration(planeRange, 4u),
               "2D canSkipCellSpanIteration true over span limit");

void testBroadphaseMergeBufferRejectReasonGuards() {

                 fuse::physics::broadphase::mergeBroadphaseBufferRejectReason(bodies, shapes, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::SceneRejected),
             "empty scene reports SceneRejected merge-buffer reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseMergeIntoBuffer(bodies, shapes, buffer),
               "shouldRunBroadphaseMergeIntoBuffer false on empty scene");


             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::None),
             "mergeable scene reports None merge-buffer reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMergeIntoBuffer(bodies, shapes, buffer),
               "shouldRunBroadphaseMergeIntoBuffer true for mergeable scene");

             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::BufferAtCapacity),
             "full buffer reports BufferAtCapacity merge-buffer reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::mergeBroadphaseBufferRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::BufferAtCapacity),
                           "BufferAtCapacity") == 0,
               "BufferAtCapacity merge-buffer reject reason has stable label");

    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer);
    expectTrue(!preflight.canMerge(), "merge-buffer preflight rejects full buffer");
    expectTrue(preflight.bufferAtCapacity, "merge-buffer preflight marks buffer at capacity");

void testPairBufferWriteSlotPreflightGuards() {

             "valid writeSlot reports None reject reason");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),

             "self-pair writeSlot reports InvalidPair reject reason");
               "InvalidPair writeSlot reject reason has stable label");

    expectTrue(preflight.canWrite(), "writeSlot preflight accepts valid slot write");

void testCellSpanPreflightGuards() {
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanIteration(validRange, 8u),
               "shouldRunCellSpanIteration true within span budget");
    expectTrue(!fuse::physics::broadphase::exceedsCellSpanPerAxis(validRange, 8u),
               "range within span budget does not exceed");

    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(validRange, 3u),
               "range exceeding span budget is flagged");
                   validRange, 3u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanBudget),
               "over-span range rejects for ExceedsSpanBudget");
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanBudget),
                           "ExceedsSpanBudget") == 0,
               "ExceedsSpanBudget span reject reason has stable label");

        fuse::physics::broadphase::preflightCellSpan(validRange, 8u);
    expectTrue(preflight.canIterate(), "cell span preflight accepts range within budget");
    expectEq(preflight.spanPerAxis.x, 4, "cell span preflight reports per-axis span");

void testBroadphaseShapeInsertPreflightGuards() {

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 99, {1.f, 0.f, 0.f});


    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseShapeInsertRejectReason(
                 fuse::physics::broadphase::BroadphaseShapeInsertRejectReason::OutOfRangeBody),
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseShapeInsert(0u, bodies, shapes, params, false),
               "canSkipBroadphaseShapeInsert on orphan shape");

                 fuse::physics::broadphase::BroadphaseShapeInsertRejectReason::ExceedsOccupancyBudget),
             "huge sphere reports ExceedsOccupancyBudget insert reject reason");

    const fuse::physics::broadphase::BroadphaseShapeInsertPreflight validPreflight =
        fuse::physics::broadphase::preflightBroadphaseShapeInsert(0u, bodies, shapes, params, false);
    expectTrue(validPreflight.canInsert(), "valid shape insert preflight can insert");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseShapeInsert(0u, bodies, shapes, params, false),
               "shouldRunBroadphaseShapeInsert true for valid shape");

void testRefineBroadphaseAllSlotsInvalidGuard() {

    expectEq(static_cast<fuse::u32>(emptyPreflight.refineReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyBuffer),
             "combined preflight carries refine reject reason");
    expectEq(static_cast<fuse::u32>(emptyPreflight.dedupeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::EmptyBuffer),
             "combined preflight carries dedupe reject reason");


    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight validPreflight =
    expectTrue(validPreflight.canRefine(), "combined preflight can refine valid scene");
    expectTrue(validPreflight.canDedupe(), "combined preflight can dedupe multiple pairs");


                 fuse::physics::broadphase::refineBroadphaseRejectReason(bodies, shapes, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::AllSlotsInvalid),
             "all-invalid slots report AllSlotsInvalid refine reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::refineBroadphaseRejectReasonName(
                               fuse::physics::broadphase::RefineBroadphaseRejectReason::AllSlotsInvalid),
                           "AllSlotsInvalid") == 0,
               "AllSlotsInvalid refine reject reason has stable label");
    expectTrue(!fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase false when all slots invalid");

void testPairBufferSortAlreadySortedGuard() {
    buffer.sortCanonical();

             "sorted buffer reports AlreadySorted sort reject reason");
               "canSkipPairBufferSort on already-sorted buffer");
                               fuse::physics::broadphase::PairBufferSortRejectReason::AlreadySorted),
                           "AlreadySorted") == 0,
               "AlreadySorted sort reject reason has stable label");

    expectTrue(preflight.alreadySorted, "sort preflight marks already-sorted buffer");
    expectTrue(!preflight.needsSort(), "sort preflight skips already-sorted buffer");

void testMergePairsAllInvalidPreflightGuards() {
    const std::vector<fuse::physics::broadphase::CandidatePair> invalidPairs = {{1u, 1u}, {2u, 2u}};

                 fuse::physics::broadphase::mergePairsIntoBufferRejectReason(invalidPairs, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::AllInvalidPairs),
             "all-invalid pair list reports AllInvalidPairs merge reject reason");
    expectTrue(fuse::physics::broadphase::canSkipMergePairsIntoBuffer(invalidPairs, buffer),
               "canSkipMergePairsIntoBuffer on all-invalid pair list");
    expectTrue(std::strcmp(fuse::physics::broadphase::mergePairsIntoBufferRejectReasonName(
                               fuse::physics::broadphase::MergePairsIntoBufferRejectReason::AllInvalidPairs),
                           "AllInvalidPairs") == 0,
               "AllInvalidPairs merge reject reason has stable label");

        fuse::physics::broadphase::preflightMergePairsIntoBuffer(invalidPairs, buffer);
    expectTrue(preflight.allInvalidPairs, "merge preflight marks all-invalid pair list");
    expectTrue(!preflight.canMerge(), "merge preflight rejects all-invalid pair list");

             "in-range slot reports None writeSlot reject reason");
               "OutOfRangeSlot writeSlot reject reason has stable label");

                   buffer, 2u, 0u, 1u,
               "writeSlot rejects for OutOfRangeSlot");

             "self-pair reports InvalidPair writeSlot reject reason");

    expectTrue(validPreflight.canWrite(), "writeSlot preflight accepts valid slot");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 1u, 2u, 3u),

    buffer.writeSlot(2u, 2u, 3u);
    expectEq(buffer.compact(), 1u, "writeSlot rejects out-of-range and self-pair slots via preflight gate");



    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 2u, 0u, 1u),
               "OutOfRangeSlot write-slot reject reason has stable label");

    expectTrue(!buffer.writeSlot(0u, 1u, 1u), "writeSlot rejects self-pair via preflight gate");
    expectTrue(buffer.writeSlot(0u, 0u, 1u), "writeSlot accepts valid pair via preflight gate");


void testPairBufferAcceptPreflightGuards() {

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferAcceptRejectReason(buffer, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptRejectReason::None),
             "empty buffer accepts two pairs");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferAccept(buffer, 2u),
               "shouldRunPairBufferAccept true for two pairs into empty buffer");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferAcceptRejectReason(buffer, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptRejectReason::ExceedsCapacity),
             "empty buffer rejects three pairs");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferAccept(buffer, 3u),
               "canSkipPairBufferAccept true when capacity exceeded");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferAcceptRejectReasonName(
                               fuse::physics::broadphase::PairBufferAcceptRejectReason::ExceedsCapacity),
               "ExceedsCapacity accept reject reason has stable label");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferAcceptRejectReason(buffer, 0u)),
             "zero additional pairs always accepted");

    const fuse::physics::broadphase::PairBufferAcceptPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferAccept(buffer, 1u);
    expectTrue(!preflight.canAccept(), "accept preflight rejects pair on full buffer");
    expectTrue(preflight.exceedsCapacity, "accept preflight marks exceedsCapacity");

void testCellRangeSpanClampPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 wideRange = {
        {-100, -100, -100},
        {100, 100, 100},
    };
                   wideRange, 8u, fuse::physics::broadphase::CellSpanClampRejectReason::None),
               "wide range rejects for None span-clamp reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanClamp(wideRange, 8u),
               "shouldRunCellSpanClamp true when span exceeds max");
    expectTrue(!fuse::physics::broadphase::canSkipCellSpanClamp(wideRange, 8u),
               "canSkipCellSpanClamp false when span exceeds max");
                               fuse::physics::broadphase::CellSpanClampRejectReason::WithinSpan),
                           "WithinSpan") == 0,
               "WithinSpan span-clamp reject reason has stable label");

    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {1, 1, 1}};
                 fuse::physics::broadphase::cellSpanClampRejectReason(unitRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::WithinSpan),
             "unit range reports WithinSpan clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(unitRange, 8u),
               "canSkipCellSpanClamp true when span is within max");

                 fuse::physics::broadphase::cellSpanClampRejectReason(unitRange, 0u)),
             "zero max span reports UnlimitedSpan clamp reject reason");

    const fuse::physics::broadphase::CellSpanClampPreflight emptyPreflight =
        fuse::physics::broadphase::preflightCellSpanClamp(inverted, 4u);
    expectTrue(emptyPreflight.emptyRange, "span-clamp preflight marks empty range");
    expectTrue(!emptyPreflight.canClamp(), "span-clamp preflight cannot clamp empty range");

                   planeRange, 4u, fuse::physics::broadphase::CellSpanClampRejectReason::WithinSpan),
               "2D within-span range rejects for WithinSpan");

void testBroadphasePairSlotRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphasePairSlotRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphasePairSlotRejectReason::ZeroPairSlots),
             "zero slots reports ZeroPairSlots pair-slot reject reason");
    expectTrue(fuse::physics::broadphase::broadphasePairSlotRejectsForReason(
                   0u, fuse::physics::broadphase::BroadphasePairSlotRejectReason::ZeroPairSlots),
               "pair-slot rejects for ZeroPairSlots");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphasePairSlotRejectReasonName(
                               fuse::physics::broadphase::BroadphasePairSlotRejectReason::ZeroPairSlots),
                           "ZeroPairSlots") == 0,
               "ZeroPairSlots pair-slot reject reason has stable label");

    const fuse::physics::broadphase::BroadphasePairSlotPreflight zeroPreflight =
        fuse::physics::broadphase::preflightBroadphasePairSlots(0u);
    expectTrue(zeroPreflight.zeroPairSlots, "pair-slot preflight marks zero slots");
    expectTrue(!zeroPreflight.canWriteSlots(), "pair-slot preflight cannot write zero slots");
    expectTrue(fuse::physics::broadphase::canSkipBroadphasePairSlotWrite(0u),
               "canSkipBroadphasePairSlotWrite on zero slots");

    const fuse::physics::broadphase::BroadphasePairSlotPreflight validPreflight =
        fuse::physics::broadphase::preflightBroadphasePairSlots(4u);
    expectTrue(validPreflight.canWriteSlots(), "pair-slot preflight accepts non-zero slots");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphasePairSlotWrite(4u),
               "shouldRunBroadphasePairSlotWrite true for non-zero slots");

void testPairBufferClampShouldRunWiring() {
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferClamp(buffer),
               "shouldRunPairBufferClamp false when within capacity");
    expectEq(buffer.applyMaxCapacityClamp(), 1u, "applyMaxCapacityClamp early-outs via shouldRun gate");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(2u, 3u);
    overflowBuffer.push(4u, 5u);
    overflowBuffer.setMaxCapacity(2u);
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferClamp(overflowBuffer),
               "shouldRunPairBufferClamp true when overflow exists");
    expectEq(overflowBuffer.applyMaxCapacityClamp(), 2u, "applyMaxCapacityClamp proceeds via shouldRun gate");
void testPairBufferPushShouldRunGuards() {
               "shouldRunPairBufferPush true for valid pair on empty buffer");
               "canSkipPairBufferPush false when shouldRunPairBufferPush true");

               "canSkipPairBufferPush false when push may proceed");

    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferPush(buffer, 2u, 2u),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferPush(buffer, 2u, 2u),

               "shouldRunPairBufferPush false on full buffer");
               "canSkipPairBufferPush true on full buffer");


             "valid slot write reports None reject reason");

               "write slot rejects for OutOfRangeSlot");

             "self-pair slot write reports InvalidPair reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferWriteSlot(buffer, 0u, 1u, 1u),
               "shouldRunPairBufferWriteSlot false for self-pair");


    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellSpanRejectReason(validRange)),
             "valid range reports None cell-span reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanOccupancy(validRange),
               "shouldRunCellSpanOccupancy true for valid range");
    expectTrue(!fuse::physics::broadphase::canSkipCellSpanOccupancy(validRange),
               "canSkipCellSpanOccupancy false for valid range");
                               fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
               "EmptyRange cell-span reject reason has stable label");

                   inverted, fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
               "inverted range rejects for EmptyRange cell span");

        fuse::physics::broadphase::preflightCellSpan(validRange);
    expectTrue(preflight.canUseRange(), "cell-span preflight accepts valid range");
    expectEq(preflight.occupancyCount, 8u, "cell-span preflight reports occupancy count");

    const fuse::physics::broadphase::CellSpanPreflight planePreflight =
        fuse::physics::broadphase::preflightCellSpan(planeRange);
    expectTrue(planePreflight.canUseRange(), "2D cell-span preflight accepts valid range");
    expectEq(planePreflight.occupancyCount, 8u, "2D cell-span preflight reports occupancy count");

    const fuse::physics::broadphase::ShapeCellInsertPreflight withinBudget =
        fuse::physics::broadphase::preflightShapeCellInsert(validRange, 8u);
    expectTrue(withinBudget.canInsert(), "shape-cell insert preflight accepts range within budget");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsert(validRange, 8u),
               "shouldRunShapeCellInsert true within budget");
                               fuse::physics::broadphase::ShapeCellInsertRejectReason::ExceedsBudget),
                           "ExceedsBudget") == 0,
               "ExceedsBudget shape-cell insert reject reason has stable label");

        fuse::physics::broadphase::preflightShapeCellInsert(validRange, 7u);
    expectTrue(!overBudget.canInsert(), "shape-cell insert preflight rejects over-budget range");
    expectTrue(overBudget.exceedsBudget, "shape-cell insert preflight marks exceedsBudget");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsert(validRange, 7u),
               "canSkipShapeCellInsert true over budget");
                   validRange, 7u,
               "over-budget range rejects for ExceedsBudget shape-cell insert");

                 fuse::physics::broadphase::shapeCellInsertRejectReason(inverted, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::EmptyRange),
             "inverted range reports EmptyRange shape-cell insert reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunShapeCellInsert(inverted, 8u),
               "shouldRunShapeCellInsert false for empty range");

                   planeRange, 4u,
               "2D over-budget range rejects for ExceedsBudget shape-cell insert");

             "in-range slot reports None write-slot reject reason");

             "out-of-range slot reports OutOfRangeSlot write-slot reject reason");
               "canSkipPairBufferWriteSlot on out-of-range slot");

    expectTrue(!buffer.canWriteSlot(0u, 1u, 1u), "canWriteSlot rejects self-pair");



             "zero additional pairs reports None accept-pairs reject reason");
    expectTrue(buffer.canAcceptPairs(0u), "zero additional pairs vacuously accepted");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferAcceptPairs(buffer, 0u),
               "shouldRunPairBufferAcceptPairs true for zero count");

    expectTrue(fuse::physics::broadphase::shouldRunPairBufferAcceptPairs(buffer, 2u),
               "empty buffer accepts two pairs via preflight");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferAcceptPairs(buffer, 3u),
               "empty buffer rejects three pairs via preflight");
                   buffer, 3u, fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::AtCapacity),
               "overflow accept-pairs rejects for AtCapacity");
                               fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::AtCapacity),
               "AtCapacity accept-pairs reject reason has stable label");

    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferAcceptPairs(buffer, 1u),
               "full buffer rejects another pair via preflight");
    const fuse::physics::broadphase::PairBufferAcceptPairsPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferAcceptPairs(buffer, 1u);
    expectTrue(preflight.atCapacity, "accept-pairs preflight marks full buffer");
    expectTrue(!preflight.canAccept(), "accept-pairs preflight rejects at capacity");

void testBroadphaseCellSlotRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellSlotRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellSlotRejectReason::ZeroSlots),
             "zero slots reports ZeroSlots cell-slot reject reason");
    expectTrue(fuse::physics::broadphase::broadphaseCellSlotRejectsForReason(
                   0u, fuse::physics::broadphase::BroadphaseCellSlotRejectReason::ZeroSlots),
               "zero slots rejects for ZeroSlots");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseCellPairGeneration(0u),
               "canSkipBroadphaseCellPairGeneration on zero slots");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseCellSlotRejectReasonName(
                               fuse::physics::broadphase::BroadphaseCellSlotRejectReason::ZeroSlots),
                           "ZeroSlots") == 0,
               "ZeroSlots cell-slot reject reason has stable label");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellSlotRejectReason(4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellSlotRejectReason::None),
             "non-zero slots report None cell-slot reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseCellPairGeneration(4u),
               "shouldRunBroadphaseCellPairGeneration true for non-zero slots");

    const fuse::physics::broadphase::BroadphaseCellSlotPreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseCellSlots(4u);
    expectTrue(preflight.canGenerate(), "cell-slot preflight accepts non-zero slot count");
    expectEq(preflight.totalCellSlots, 4u, "cell-slot preflight reports slot count");

void testMergePairsIntoBufferRejectsForReasonGuards() {
    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs;

    expectTrue(fuse::physics::broadphase::mergePairsIntoBufferRejectsForReason(
                   emptyPairs, buffer,
                   fuse::physics::broadphase::MergePairsIntoBufferRejectReason::EmptyPairs),
               "empty pair list rejects for EmptyPairs");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}, {2u, 3u}};
                   pairs, buffer, fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None),
               "valid merge rejects for None");

                   pairs, buffer,
                   fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
               "full buffer merge rejects for BufferFull");
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(wideRange, 8u),
               "wide range exceeds span limit before clamp");
    expectTrue(fuse::physics::broadphase::shouldRunCellRangeSpanClamp(wideRange, 8u),
               "shouldRunCellRangeSpanClamp true when span exceeds limit");

    const fuse::physics::broadphase::CellRange3 clamped =
        fuse::physics::broadphase::clampCellRange3(wideRange, 8u);
    expectTrue(clamped.maxCell.x - clamped.minCell.x < wideRange.maxCell.x - wideRange.minCell.x,
               "clampCellRange3 reduces x span for wide range");
    expectTrue(!fuse::physics::broadphase::exceedsCellSpanPerAxis(clamped, 9u),
               "clampCellRange3 brings span within halfSpan budget");

                 fuse::physics::broadphase::cellRangeSpanClampRejectReason(wideRange, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellRangeSpanClampRejectReason::UnlimitedSpan),
             "zero max span reports UnlimitedSpan reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellRangeSpanClamp(wideRange, 0u),
               "canSkipCellRangeSpanClamp true for unlimited span");

                 fuse::physics::broadphase::cellRangeSpanClampRejectReason(inverted, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellRangeSpanClampRejectReason::EmptyRange),
             "inverted range reports EmptyRange span-clamp reject reason");
    expectTrue(fuse::physics::broadphase::cellRangeSpanClampRejectsForReason(
                   inverted, 8u, fuse::physics::broadphase::CellRangeSpanClampRejectReason::EmptyRange),

    const fuse::physics::broadphase::CellRangeSpanClampPreflight preflight =
        fuse::physics::broadphase::preflightCellRangeSpanClamp(wideRange, 8u);
    expectTrue(preflight.needsClamp(), "span-clamp preflight requests clamp for wide range");
    expectTrue(preflight.exceedsSpanLimit, "span-clamp preflight marks exceedsSpanLimit");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellRangeSpanClampRejectReasonName(
                               fuse::physics::broadphase::CellRangeSpanClampRejectReason::UnlimitedSpan),
               "UnlimitedSpan span-clamp reject reason has stable label");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {20, 1}};
    const fuse::physics::broadphase::CellRangeSpanClampPreflight planePreflight =
        fuse::physics::broadphase::preflightCellRangeSpanClamp(planeRange, 4u);
    expectTrue(planePreflight.needsClamp(), "2D span-clamp preflight requests clamp for wide range");
               "2D range exceeds span limit before clamp");



                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 1u, 1u, 1u)),
                   buffer, 1u, 1u, 1u,
               "write-slot rejects for InvalidPair");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 1u, 1u, 1u),


        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 2u, 3u);

    buffer.writeSlot(0u, 2u, 3u);
    expectTrue(buffer.slotIsValid(0u), "writeSlot proceeds through preflight gate");
    buffer.writeSlot(1u, 1u, 1u);
    expectEq(buffer.compact(), 1u, "writeSlot rejects self-pair through preflight gate");

    const fuse::physics::broadphase::CellRange3 withinRange = {{0, 0, 0}, {3, 3, 3}};
    expectTrue(fuse::physics::broadphase::cellSpanWithinBudget(withinRange, 8u),
               "within-range span fits per-axis budget");
    expectTrue(!fuse::physics::broadphase::exceedsCellSpanPerAxis(withinRange, 8u),
               "within-range span does not exceed per-axis budget");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(withinRange, 8u),
               "canSkipCellSpanClamp true when span fits budget");
    expectTrue(!fuse::physics::broadphase::shouldRunCellSpanClamp(withinRange, 8u),
               "shouldRunCellSpanClamp false when span fits budget");

    fuse::physics::broadphase::CellRange3 wideRange = {
               "wide range exceeds per-axis span budget");
               "shouldRunCellSpanClamp true when span exceeds budget");
                   wideRange, 8u, fuse::physics::broadphase::CellSpanClampRejectReason::ExceedsSpanPerAxis),
               "wide range rejects for ExceedsSpanPerAxis");
                               fuse::physics::broadphase::CellSpanClampRejectReason::ExceedsSpanPerAxis),
                           "ExceedsSpanPerAxis") == 0,
               "ExceedsSpanPerAxis cell-span reject reason has stable label");

    const fuse::physics::broadphase::CellSpanClampPreflight widePreflight =
        fuse::physics::broadphase::preflightCellSpanClamp(wideRange, 8u);
    expectTrue(widePreflight.needsClamp(), "wide-range preflight needs clamp");
    expectTrue(widePreflight.exceedsSpanPerAxis, "wide-range preflight marks exceedsSpanPerAxis");

                 fuse::physics::broadphase::cellSpanClampRejectReason(inverted, 8u)),
             "inverted range reports EmptyRange cell-span reject reason");
    expectTrue(!fuse::physics::broadphase::preflightCellSpanClamp(inverted, 8u).canClamp(),
               "inverted range preflight cannot clamp");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {15, 1}};
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(planeRange, 8u),
               "2D wide range exceeds per-axis span budget");
    const fuse::physics::broadphase::CellSpanClampPreflight planePreflight =
        fuse::physics::broadphase::preflightCellSpanClamp2D(planeRange, 8u);
    expectTrue(planePreflight.needsClamp(), "2D wide-range preflight needs clamp");
    expectEq(planePreflight.spanPerAxis.x, 16, "2D preflight reports x span");

void testShouldRunBroadphasePairGenerationGuards() {

    expectTrue(fuse::physics::broadphase::canSkipBroadphasePairGeneration(bodies, shapes),
               "canSkipBroadphasePairGeneration true when shouldRunBroadphasePairGeneration false");

               "canSkipBroadphasePairGeneration true when shouldRun false");

               "shouldRunBroadphasePairGeneration false on singleton scene");

    expectTrue(!fuse::physics::broadphase::canSkipBroadphasePairGeneration(bodies, shapes),
               "canSkipBroadphasePairGeneration false when shouldRunBroadphasePairGeneration true");

void testBroadphaseCellPairBuildPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellPairBuildRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairBuildRejectReason::NoCellSlots),
             "zero cell slots reports NoCellSlots reject reason");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseCellPairBuild(0u),
               "canSkipBroadphaseCellPairBuild true for zero slots");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseCellPairBuild(0u),
               "shouldRunBroadphaseCellPairBuild false for zero slots");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseCellPairBuildRejectReasonName(
                               fuse::physics::broadphase::BroadphaseCellPairBuildRejectReason::NoCellSlots),
                           "NoCellSlots") == 0,
               "NoCellSlots cell-pair-build reject reason has stable label");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellPairBuildRejectReason(4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairBuildRejectReason::None),
             "non-zero cell slots report None reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseCellPairBuild(4u),
               "shouldRunBroadphaseCellPairBuild true for non-zero slots");

    const fuse::physics::broadphase::BroadphaseCellPairBuildPreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairBuild(2u);
    expectTrue(preflight.canBuild(), "cell-pair-build preflight accepts non-zero slots");
             "cell-pair-build preflight carries reject reason");
               "canSkipBroadphasePairGeneration false when shouldRun true");





    buffer.writeSlot(1u, 2u, 2u);
    expectTrue(!buffer.slotIsValid(1u), "writeSlot rejects self-pair via preflight gate");

        fuse::physics::broadphase::preflightPairBufferWrite(buffer, 0u, 2u, 3u);
    expectTrue(preflight.canWrite(), "write preflight accepts valid slot write");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferWrite(buffer, 0u, 2u, 3u),
               "shouldRunPairBufferWrite true for valid write");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWrite(buffer, 2u, 2u, 3u),

    buffer.preparePairSlots(3u);

                   buffer, 0u, fuse::physics::broadphase::PairBufferInvalidateRejectReason::None),

                 fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 99u)),
             "far out-of-range slot reports OutOfRangeSlot invalidate reject reason");
                               fuse::physics::broadphase::PairBufferInvalidateRejectReason::OutOfRangeSlot),

    buffer.invalidateSlot(99u);
    expectTrue(buffer.slotIsValid(2u), "invalidateSlot ignores far out-of-range slot");

        fuse::physics::broadphase::preflightPairBufferInvalidate(buffer, 2u);
    expectTrue(preflight.canInvalidate(), "invalidate preflight accepts in-range slot");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidate(buffer, 2u),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidate(buffer, 99u),
               "canSkipPairBufferInvalidate true for out-of-range slot");

    expectEq(buffer.compact(), 1u, "compact after invalidate keeps remaining valid slot");

             "zero occupants reports EmptyOccupants cell-pair reject reason");
                   0u, fuse::physics::broadphase::CellPairGenRejectReason::EmptyOccupants),
               "zero occupants rejects for EmptyOccupants");

    expectTrue(fuse::physics::broadphase::canSkipCellPairGeneration(1u),
               "canSkipCellPairGeneration true for single occupant");
    expectTrue(!fuse::physics::broadphase::shouldRunCellPairGeneration(1u),
               "shouldRunCellPairGeneration false for single occupant");

             "multiple occupants report None cell-pair reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGeneration(3u),
               "shouldRunCellPairGeneration true for multiple occupants");

        fuse::physics::broadphase::preflightCellPairGen(2u);
    expectTrue(preflight.canGenerate(), "cell-pair preflight accepts two occupants");
    expectEq(preflight.occupantCount, 2u, "cell-pair preflight reports occupant count");

void testCellShapeInsertRejectReasonGuards() {


    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellShapeInsertRejectReason(
             static_cast<fuse::u32>(fuse::physics::broadphase::CellShapeInsertRejectReason::OutOfRangeBody),
    expectTrue(fuse::physics::broadphase::cellShapeInsertRejectsForReason(
                   fuse::physics::broadphase::CellShapeInsertRejectReason::OutOfRangeBody),
    expectTrue(std::strcmp(fuse::physics::broadphase::cellShapeInsertRejectReasonName(
                               fuse::physics::broadphase::CellShapeInsertRejectReason::OccupancyRejected),
               "OccupancyRejected insert reject reason has stable label");

    const fuse::physics::broadphase::CellShapeInsertPreflight validPreflight =
               "shouldRunShapeCellInsert true for valid shape");

    bodies.addBody({500.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {256.f, 0.f, 0.f});
    const fuse::physics::broadphase::CellShapeInsertPreflight budgetPreflight =
    expectTrue(!budgetPreflight.canInsert(), "oversized shape insert preflight rejects occupancy");
    expectTrue(budgetPreflight.occupancyRejected, "oversized shape insert preflight marks occupancyRejected");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsert(1u, bodies, shapes, params, false),
               "canSkipShapeCellInsert true when occupancy rejected");


               "shouldRunPairBufferWrite true for valid slot write");


                               fuse::physics::broadphase::PairBufferWriteRejectReason::InvalidPair),
               "InvalidPair write reject reason has stable label");

        fuse::physics::broadphase::preflightPairBufferWrite(buffer, 1u, 2u, 3u);


    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 0u)),

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 2u)),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidate(buffer, 2u),

    expectEq(buffer.countValidSlots(), 0u, "invalidateSlot ignores out-of-range slot");

void testCellCapacityInsertPreflightGuards() {

                 fuse::physics::broadphase::cellCapacityInsertRejectReason(validRange, 8u)),
             "within-budget range reports None cell-capacity insert reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellCapacityInsert(validRange, 8u),
    expectTrue(!fuse::physics::broadphase::canSkipCellCapacityInsert(validRange, 8u),
               "canSkipCellCapacityInsert false within budget");

                 fuse::physics::broadphase::cellCapacityInsertRejectReason(validRange, 7u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityInsertRejectReason::ExceedsBudget),
             "over-budget range reports ExceedsBudget cell-capacity insert reject reason");
                   validRange, 7u, fuse::physics::broadphase::CellCapacityInsertRejectReason::ExceedsBudget),
               "cellCapacityInsertRejectsForReason matches over-budget range");

                 fuse::physics::broadphase::cellCapacityInsertRejectReason(inverted, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityInsertRejectReason::EmptyRange),
             "empty range reports EmptyRange cell-capacity insert reject reason");

        fuse::physics::broadphase::preflightCellCapacityInsert(validRange, 8u);
    expectTrue(preflight.canInsert(), "cell-capacity insert preflight accepts within-budget range");
    expectEq(preflight.occupancyCount, 8u, "cell-capacity insert preflight reports occupancy count");

    expectTrue(fuse::physics::broadphase::canSkipCellCapacityInsert(planeRange, 4u),
               "2D canSkipCellCapacityInsert true over budget");

             "empty cell reports SingletonOccupant pair-gen reject reason");
             "single occupant reports SingletonOccupant pair-gen reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellPairGen(1u),
               "canSkipCellPairGen true for single occupant");
    expectTrue(!fuse::physics::broadphase::shouldRunCellPairGen(1u),
               "shouldRunCellPairGen false for single occupant");

             "multi-occupant cell reports None pair-gen reject reason");

        fuse::physics::broadphase::preflightCellPairGen(3u);
    expectTrue(preflight.canGenerate(), "cell-pair gen preflight accepts multi-occupant cell");
    expectEq(preflight.pairCount, 3u, "cell-pair gen preflight reports pair count for three occupants");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGen(3u),

void testBroadphaseCellCapacityInsertIntegration() {


    params.tableSize = 256;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "cell-capacity insert gate skips over-budget shape occupancy");





    expectTrue(preflight.canWrite(), "write-slot preflight accepts valid pair");



                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 3u)),
                   buffer,
                   3u,

    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears valid flag via preflight gate");
    buffer.invalidateSlot(5u);
    expectTrue(buffer.slotIsValid(0u) == false, "out-of-range invalidate is a no-op");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(0u, 0u)),
             "zero occupants report InsufficientOccupants cell-pair reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellPairGeneration(1u, 1u),
               "canSkipCellPairGeneration true for singleton occupants");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(2u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::SingletonOccupants),
             "duplicate occupants report SingletonOccupants cell-pair reject reason");
                               fuse::physics::broadphase::CellPairGenRejectReason::SingletonOccupants),
                           "SingletonOccupants") == 0,
               "SingletonOccupants cell-pair reject reason has stable label");

        fuse::physics::broadphase::preflightCellPairGeneration(3u, 3u);
    expectTrue(preflight.canGenerate(), "cell-pair preflight accepts three unique occupants");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGeneration(3u, 3u),
               "shouldRunCellPairGeneration true for three unique occupants");
    expectEq(fuse::physics::broadphase::estimateCellPairCount(3u), 3u, "estimateCellPairCount matches n*(n-1)/2");


    expectTrue(preflight.canInsert(), "cell-capacity insert preflight accepts valid range");
             "cell-capacity insert preflight carries reject reason");

                   validRange,
                   7u,
                   fuse::physics::broadphase::CellCapacityInsertRejectReason::ExceedsBudget),
               "over-budget range rejects for ExceedsBudget insert reason");
    expectTrue(fuse::physics::broadphase::canSkipCellCapacityInsert(validRange, 7u),

                   inverted,
                   4u,
                   fuse::physics::broadphase::CellCapacityInsertRejectReason::EmptyRange),
               "inverted range rejects for EmptyRange insert reason");
               "EmptyRange cell-capacity insert reject reason has stable label");




                   buffer, 2u, 0u, 1u, fuse::physics::broadphase::PairBufferWriteRejectReason::OutOfRangeSlot),




    expectTrue(!buffer.slotIsValid(2u), "slotIsValid rejects slot at pairSlotCount boundary");
    expectTrue(!buffer.slotIsValid(99u), "slotIsValid rejects far out-of-range slot");

             "out-of-range invalidate reports OutOfRangeSlot reject reason");

    expectTrue(buffer.slotIsValid(0u), "invalidateSlot ignores out-of-range slot");
    expectTrue(buffer.slotIsValid(0u), "invalidateSlot ignores far out-of-range slot");

    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears in-range slot");
               "shouldRunPairBufferInvalidate true for in-range invalid slot");

        fuse::physics::broadphase::preflightPairBufferInvalidate(buffer, 99u);
    expectTrue(!preflight.canInvalidate(), "invalidate preflight rejects out-of-range slot");
    expectTrue(preflight.outOfRangeSlot, "invalidate preflight marks out-of-range slot");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(emptyOccupants)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::TooFewOccupants),
             "empty occupants report TooFewOccupants cell-pair reject reason");

    const std::vector<fuse::u32> singleOccupant = {0u};
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(singleOccupant)),
             "single occupant reports TooFewOccupants cell-pair reject reason");

    const std::vector<fuse::u32> duplicateBodies = {0u, 0u};
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(duplicateBodies)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::SingleUniqueBody),
             "duplicate occupants report SingleUniqueBody cell-pair reject reason");
                               fuse::physics::broadphase::CellPairGenRejectReason::SingleUniqueBody),
                           "SingleUniqueBody") == 0,
               "SingleUniqueBody cell-pair reject reason has stable label");

    const std::vector<fuse::u32> validOccupants = {0u, 1u, 1u};
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(validOccupants)),
             "multi-body occupants report None cell-pair reject reason");
    expectEq(fuse::physics::broadphase::countPairsForCellOccupants(validOccupants), 1u,
             "countPairsForCellOccupants returns one pair for two unique bodies");

        fuse::physics::broadphase::preflightCellPairGeneration(validOccupants);
    expectTrue(preflight.canGenerate(), "cell-pair preflight accepts valid occupants");
    expectEq(preflight.pairCount, 1u, "cell-pair preflight reports pair count");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGeneration(validOccupants),
               "shouldRunCellPairGeneration true for valid occupants");

                 0u, 2u, validRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::None),
             "in-range body with valid range reports None shape insert reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsert(0u, 2u, validRange, 8u),

             "out-of-range body reports OutOfRangeBody shape insert reject reason");

             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::OccupancyRejected),
             "over-budget range reports OccupancyRejected shape insert reject reason");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsert(0u, 2u, validRange, 7u),

             "2D over-budget range reports OccupancyRejected shape insert reject reason");
                               fuse::physics::broadphase::ShapeCellInsertRejectReason::OccupancyRejected),
               "OccupancyRejected shape insert reject reason has stable label");

    const fuse::physics::broadphase::ShapeCellInsertPreflight preflight =
        fuse::physics::broadphase::preflightShapeCellInsert(0u, 2u, validRange, 8u);
    expectTrue(preflight.canInsert(), "shape insert preflight accepts valid insert");
             "shape insert preflight carries reject reason");


               "shouldRunPairBufferWriteSlot true for valid slot write");






    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),

    fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason reason =
               "wouldSkipPairBufferInvalidateSlot true before first write");
             "unwritten slot reports AlreadyInvalid before first write");

    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot true for valid slot");

             "already-invalid slot reports AlreadyInvalid invalidate reject reason");
               "canSkipPairBufferInvalidateSlot on already-invalid slot");
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears slot via preflight gate");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 2u)),

                   99u,


    expectTrue(fuse::physics::broadphase::canSkipCellPairGeneration(0u),

             "singleton occupant reports SingletonOccupant cell-pair reject reason");
               "shouldRunCellPairGeneration false for singleton occupant");

        fuse::physics::broadphase::preflightCellPairGeneration(3u);
    expectTrue(multiPreflight.canGenerate(), "cell-pair preflight accepts multiple occupants");
    expectEq(multiPreflight.pairCount, 3u, "cell-pair preflight reports n*(n-1)/2 pair count");

    const std::vector<fuse::u32> occupants = {0u, 1u, 2u};
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGeneration(occupants),
               "shouldRunCellPairGeneration true for occupant vector");
               "SingletonOccupant cell-pair reject reason has stable label");

    expectTrue(validPreflight.canInsert(), "shape cell-insert preflight accepts valid range");
    expectEq(validPreflight.occupancyCount, 8u, "shape cell-insert preflight reports occupancy count");

                 fuse::physics::broadphase::shapeCellInsertRejectReason(2u, 2u, validRange, 8u)),
             "out-of-range body reports OutOfRangeBody shape cell-insert reject reason");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsert(2u, 2u, validRange, 8u),
               "canSkipShapeCellInsert on out-of-range body");

    const fuse::physics::broadphase::CellRange3 overBudget = {{0, 0, 0}, {2, 2, 2}};
                 fuse::physics::broadphase::shapeCellInsertRejectReason(0u, 2u, overBudget, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::ExceedsOccupancy),
             "over-budget range reports ExceedsOccupancy shape cell-insert reject reason");

    const fuse::physics::broadphase::CellRange3 emptyRange = {{2, 0, 0}, {1, 0, 0}};
    const fuse::physics::broadphase::ShapeCellInsertPreflight emptyPreflight =
        fuse::physics::broadphase::preflightShapeCellInsert(0u, 2u, emptyRange, 8u);
    expectTrue(emptyPreflight.emptyRange, "shape cell-insert preflight marks empty range");
    expectTrue(!emptyPreflight.canInsert(), "shape cell-insert preflight rejects empty range");

                   0u,
                   2u,
                   planeRange,
                   fuse::physics::broadphase::ShapeCellInsertRejectReason::ExceedsOccupancy),
               "2D shape cell-insert rejects for ExceedsOccupancy");
                               fuse::physics::broadphase::ShapeCellInsertRejectReason::EmptyRange),
               "EmptyRange shape cell-insert reject reason has stable label");

void testPairBufferCanSkipCompactAndClamp() {
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer skips compact-and-clamp");

    expectTrue(buffer.canSkipCompactAndClamp(), "synced within-capacity buffer skips compact-and-clamp");

    expectTrue(!buffer.canSkipCompactAndClamp(), "prepared slots with stale activeCount need compact-and-clamp");

    expectTrue(!buffer.canSkipCompactAndClamp(), "invalid slots need compact-and-clamp");














    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 8u, &reason),
               "wouldSkipPairBufferInvalidateSlot true for out-of-range slot");
             "wouldSkipPairBufferInvalidateSlot reports OutOfRangeSlot");

    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 8u) ==
                   fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 8u),





    fuse::physics::broadphase::CellPairGenRejectReason reason =
        fuse::physics::broadphase::CellPairGenRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipCellPairGeneration(emptyOccupants, &reason),
               "wouldSkipCellPairGeneration true on empty cell");
             "wouldSkipCellPairGeneration reports EmptyCell");


    expectTrue(fuse::physics::broadphase::wouldSkipCellPairGeneration(multiOccupants) ==
                   fuse::physics::broadphase::canSkipCellPairGeneration(multiOccupants),
               "wouldSkipCellPairGeneration agrees with canSkipCellPairGeneration");








    fuse::physics::broadphase::ShapeCellInsertRejectReason reason =
        fuse::physics::broadphase::ShapeCellInsertRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipShapeCellInsert(0u, bodies, shapes, params, false, &reason),
               "wouldSkipShapeCellInsert true for oversized shape");
             "wouldSkipShapeCellInsert reports OccupancySkipped");




                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 0u, 1u, 2u)),
             "in-range valid write reports None reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferWrite(buffer, 0u, 1u, 2u),

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 2u, 1u, 2u)),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWrite(buffer, 2u, 1u, 2u),
               "canSkipPairBufferWrite on out-of-range slot");

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 0u, 2u, 2u)),

        fuse::physics::broadphase::preflightPairBufferWrite(buffer, 0u, 1u, 2u);

    buffer.writeSlot(0u, 1u, 2u);
    expectTrue(buffer.bodyA[0u] == 1u && buffer.bodyB[0u] == 2u,
               "writeSlot rejects self-pair via preflight gate");



               "canSkipPairBufferInvalidate on out-of-range slot");


    const std::vector<fuse::u32> duplicateOccupants = {1u, 1u};
    const std::vector<fuse::u32> pairOccupants = {1u, 2u};

             "empty cell reports InsufficientOccupants pair-gen reject reason");
    expectEq(fuse::physics::broadphase::countPairsForCell(emptyOccupants), 0u,
             "countPairsForCell returns zero for empty cell");

             "duplicate singleton cell reports InsufficientOccupants pair-gen reject reason");
    expectEq(fuse::physics::broadphase::countPairsForCell(duplicateOccupants), 0u,
             "countPairsForCell returns zero for duplicate singleton cell");

    const fuse::physics::broadphase::CellPairGenPreflight validPreflight =
        fuse::physics::broadphase::preflightCellPairGen(pairOccupants);
    expectTrue(validPreflight.canGenerate(), "pair-gen preflight accepts two unique occupants");
    expectEq(validPreflight.uniqueBodyCount, 2u, "pair-gen preflight reports unique body count");
    expectEq(fuse::physics::broadphase::countPairsForCell(pairOccupants), 1u,
             "countPairsForCell returns one pair for two occupants");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGeneration(pairOccupants),
               "shouldRunCellPairGeneration true for pair occupants");
                   singletonOccupants,
               "singleton cell rejects for InsufficientOccupants");

    const fuse::physics::broadphase::CellRange3 validRange = {
        {0, 0, 0},
        {1, 1, 1},
    const fuse::physics::broadphase::CellRange3 emptyRange = {
        {1, 0, 0},

                 5u, 4u, validRange, 0u)),
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsert(5u, 4u, validRange, 0u),

                 0u, 4u, emptyRange, 0u)),
             "empty range reports EmptyRange insert reject reason");

                 0u, 4u, validRange, 7u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::ExceedsBudget),
             "over-budget range reports ExceedsBudget insert reject reason");
               "ExceedsBudget insert reject reason has stable label");

        fuse::physics::broadphase::preflightShapeCellInsert(0u, 4u, validRange, 8u);
    expectTrue(preflight.canInsert(), "insert preflight accepts valid range within budget");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsert(0u, 4u, validRange, 8u),

    const fuse::physics::broadphase::CellRange2 planeRange = {
        {0, 0},
        {3, 1},
                 0u, 2u, planeRange, 7u)),
             "2D over-budget range reports ExceedsBudget insert reject reason");





    expectTrue(validPreflight.canWrite(), "write-slot preflight accepts valid slot");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight invalidPreflight =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 3u, 2u, 3u);
    expectTrue(invalidPreflight.outOfRangeSlot, "write-slot preflight marks out-of-range slot");
    expectTrue(!invalidPreflight.canWrite(), "write-slot preflight rejects out-of-range slot");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 3u, 2u, 3u),

    expectTrue(!buffer.slotIsValid(2u), "writeSlot rejects out-of-range slot via preflight gate");


             "in-range slot reports None invalidate-slot reject reason");
               "valid invalidate-slot rejects for None");

             "out-of-range slot reports OutOfRangeSlot invalidate-slot reject reason");
               "OutOfRangeSlot invalidate-slot reject reason has stable label");


    expectTrue(!buffer.slotIsValid(1u), "invalidateSlot clears slot via preflight gate");
    buffer.invalidateSlot(9u);
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 9u),

             "single occupant reports SingletonOccupant cell-pair reject reason");
             "three occupants report None cell-pair reject reason");
                   1u, fuse::physics::broadphase::CellPairGenRejectReason::SingletonOccupant),
               "cellPairGenRejectsForReason matches singleton occupant");
                               fuse::physics::broadphase::CellPairGenRejectReason::EmptyOccupants),
                           "EmptyOccupants") == 0,
               "EmptyOccupants cell-pair reject reason has stable label");

    expectEq(fuse::physics::broadphase::estimatePairCountForCellOccupants(3u), 3u,
             "three unique occupants yield three cell pairs");
    expectEq(fuse::physics::broadphase::estimatePairCountForCellOccupants(1u), 0u,
             "single occupant yields zero cell pairs");

        fuse::physics::broadphase::preflightCellPairGen(4u);
    expectEq(multiPreflight.pairCount, 6u, "cell-pair preflight reports pair count");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGeneration(4u),
               "canSkipCellPairGeneration true for empty occupants");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellCapacityInsertRejectReason(validRange, 8u)),
             "valid range reports None cell-capacity insert reject reason");
                   validRange, 8u, fuse::physics::broadphase::CellCapacityInsertRejectReason::None),
               "valid range rejects for None insert reason");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellCapacityInsertRejectReason(inverted, 4u)),
             "inverted range reports EmptyRange cell-capacity insert reject reason");
               "ExceedsBudget cell-capacity insert reject reason has stable label");

    const fuse::physics::broadphase::CellCapacityInsertPreflight insertPreflight =
    expectTrue(insertPreflight.canInsert(), "cell-capacity insert preflight accepts valid range");
    expectEq(insertPreflight.occupancyCount, 8u, "cell-capacity insert preflight reports occupancy count");

    const fuse::physics::broadphase::CellCapacityInsertPreflight planePreflight =
        fuse::physics::broadphase::preflightCellCapacityInsert(planeRange, 10u);
    expectTrue(planePreflight.canInsert(), "2D cell-capacity insert preflight accepts valid range");

                 fuse::physics::broadphase::PairBufferWriteSlotRejectReason::UnpreparedBuffer),
             "unprepared buffer reports UnpreparedBuffer writeSlot reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "canSkipPairBufferWriteSlot on unprepared buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "shouldRunPairBufferWriteSlot false on unprepared buffer");

             "prepared valid write reports None reject reason");
               "shouldRunPairBufferWriteSlot true for valid prepared write");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 3u, 0u, 1u)),
             "out-of-range slot reports OutOfRangeSlot writeSlot reject reason");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 2u, 2u)),
                   buffer, 0u, 2u, 2u,
               "self-pair writeSlot rejects for InvalidPair");

    expectTrue(preflight.canWrite(), "writeSlot preflight accepts valid prepared write");
             "writeSlot preflight carries reject reason");

             "empty buffer reports OutOfRangeSlot invalidate reject reason");


             "slot beyond pairSlotCount reports OutOfRangeSlot invalidate reject reason");


        fuse::physics::broadphase::preflightPairBufferInvalidateSlot(buffer, 0u);
    expectTrue(preflight.canInvalidate(), "invalidateSlot preflight accepts in-range slot");
             "invalidateSlot preflight carries reject reason");

void testShapeCellOccupancyPreflightGuards() {

    const fuse::physics::broadphase::CellOccupancyPreflight withinPreflight =
        fuse::physics::broadphase::preflightShapeCellOccupancy(validRange, params);
    expectTrue(withinPreflight.canIterate(), "shape cell occupancy preflight accepts within budget");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellOccupancyIteration(validRange, params),
               "shouldRunShapeCellOccupancyIteration true within budget");
    expectTrue(!fuse::physics::broadphase::canSkipShapeCellOccupancyIteration(validRange, params),
               "canSkipShapeCellOccupancyIteration false within budget");

    params.maxCellOccupancy = 7u;
    const fuse::physics::broadphase::CellOccupancyPreflight overPreflight =
    expectTrue(!overPreflight.canIterate(), "shape cell occupancy preflight rejects over budget");
    expectTrue(overPreflight.exceedsBudget, "shape cell occupancy preflight marks exceedsBudget");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellOccupancyIteration(validRange, params),
               "canSkipShapeCellOccupancyIteration true over budget");

    expectTrue(fuse::physics::broadphase::canSkipShapeCellOccupancyIteration(planeRange, params),
               "2D shape cell occupancy skip true over budget");









               "canSkipPairBufferInvalidateSlot true for already-invalid slot");

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 99u)),

    expectTrue(preflight.canInvalidate(), "invalidate preflight accepts valid slot");




             "orphan shape reports OutOfRangeBody cell-insert reject reason");
               "canSkipShapeCellInsert true for orphan shape");

    expectTrue(!overBudget.canInsert(), "huge sphere preflight rejects over-budget occupancy");
    expectTrue(overBudget.exceedsBudget, "huge sphere preflight marks exceedsBudget");
    expectTrue(overBudget.occupancyCount > 8u, "huge sphere preflight reports occupancy count");

    const fuse::physics::broadphase::ShapeCellInsertPreflight unlimited =
    expectTrue(unlimited.canInsert(), "unlimited occupancy budget allows huge sphere insert");
               "shouldRunShapeCellInsert true under unlimited budget");
               "ExceedsBudget cell-insert reject reason has stable label");

void testMergePairPushPreflightGuards() {

                 fuse::physics::broadphase::mergePairPushRejectReason(buffer, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairPushRejectReason::None),
             "valid merge push reports None reject reason");

                 fuse::physics::broadphase::mergePairPushRejectReason(buffer, 2u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairPushRejectReason::InvalidPair),
             "self-pair merge push reports InvalidPair reject reason");
    expectTrue(fuse::physics::broadphase::mergePairPushRejectsForReason(
                   buffer, 2u, 2u, fuse::physics::broadphase::MergePairPushRejectReason::InvalidPair),
               "mergePairPushRejectsForReason matches self-pair");

                 fuse::physics::broadphase::mergePairPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairPushRejectReason::AtCapacity),
             "full buffer merge push reports AtCapacity reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::mergePairPushRejectReasonName(
                               fuse::physics::broadphase::MergePairPushRejectReason::AtCapacity),
               "AtCapacity merge-pair push reject reason has stable label");

    const fuse::physics::broadphase::MergePairPushPreflight preflight =
        fuse::physics::broadphase::preflightMergePairPush(buffer, 2u, 3u);
    expectTrue(!preflight.canPush(), "merge-pair push preflight rejects at-capacity pair");
    expectTrue(preflight.atCapacity, "merge-pair push preflight marks atCapacity");

void testBroadphaseMergeBodyCountPreflight() {

    bodies.addBody({1.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 2, {0.5f, 0.f, 0.f});

    const fuse::physics::broadphase::BroadphaseMergePreflight preflight =
    expectEq(preflight.planeBodyCount, 1u, "merge preflight counts plane bodies");
    expectEq(preflight.dynamicBodyCount, 2u, "merge preflight counts dynamic bodies");
    expectTrue(preflight.canMerge(), "multi-body merge preflight can merge");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteRejectReason::UnpreparedSlots),
             "unprepared buffer reports UnpreparedSlots write reject reason");
                               fuse::physics::broadphase::PairBufferWriteRejectReason::UnpreparedSlots),
                           "UnpreparedSlots") == 0,
               "UnpreparedSlots write reject reason has stable label");


    const fuse::physics::broadphase::PairBufferWritePreflight validWrite =
    expectTrue(validWrite.canWrite(), "write preflight accepts valid prepared slot");

    expectTrue(buffer.slotIsValid(0u), "writeSlot succeeds through write preflight gate");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferWrite(buffer, 0u, 2u, 2u),
               "shouldRunPairBufferWrite false for self-pair");

               "canSkipPairBufferInvalidate on empty buffer");

    const fuse::physics::broadphase::PairBufferInvalidatePreflight validInvalidate =
        fuse::physics::broadphase::preflightPairBufferInvalidate(buffer, 0u);
    expectTrue(validInvalidate.canInvalidate(), "invalidate preflight accepts in-range slot");

    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears slot through preflight gate");
    expectEq(buffer.countValidSlots(), 0u, "invalidateSlot reduces valid slot count");
    expectEq(buffer.countValidSlots(), 0u, "out-of-range invalidate is a no-op");



             "empty scene reports OutOfRangeBody cell-insert reject reason");

                 fuse::physics::broadphase::ShapeCellInsertRejectReason::CellOccupancyRejected),
             "huge sphere reports CellOccupancyRejected cell-insert reject reason");
                           "CellOccupancyRejected") == 0,
               "CellOccupancyRejected cell-insert reject reason has stable label");

    expectTrue(validInsert.canInsert(), "unbounded occupancy allows shape cell insert");
               "shouldRunShapeCellInsert true with unbounded occupancy");

void testRefinePairRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::refinePairRejectReason(buffer, 0u, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairRejectReason::OutOfRangeSlot),
             "empty buffer reports OutOfRangeSlot refine-pair reject reason");
    expectTrue(fuse::physics::broadphase::canSkipRefinePair(buffer, 0u, 0u),
               "canSkipRefinePair on empty buffer");

    const fuse::physics::broadphase::RefinePairPreflight validPair =
        fuse::physics::broadphase::preflightRefinePair(buffer, 0u, 2u);
    expectTrue(validPair.canRefine(), "refine-pair preflight accepts valid pushed pair");
    expectTrue(fuse::physics::broadphase::shouldRunRefinePair(buffer, 0u, 2u),
               "shouldRunRefinePair true for valid pair");

    buffer.bodyA[0u] = 1u;
    buffer.bodyB[0u] = 1u;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::refinePairRejectReason(buffer, 0u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairRejectReason::InvalidPair),
             "self-pair slot reports InvalidPair refine-pair reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::refinePairRejectReasonName(
                               fuse::physics::broadphase::RefinePairRejectReason::InvalidPair),
               "InvalidPair refine-pair reject reason has stable label");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::refinePairRejectReason(buffer, 1u, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairRejectReason::InvalidSlot),
             "invalid slot reports InvalidSlot refine-pair reject reason");

void testMergePairPushRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::mergePairPushRejectReason(buffer, 0u, 1u)),
             "valid merge pair push reports None reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunMergePairPush(buffer, 0u, 1u),
               "shouldRunMergePairPush true for valid pair into empty buffer");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::mergePairPushRejectReason(buffer, 2u, 2u)),
             "self-pair reports InvalidPair merge push reject reason");
               "AtCapacity merge push reject reason has stable label");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::mergePairPushRejectReason(buffer, 2u, 3u)),
             "full buffer reports AtCapacity merge push reject reason");
    expectTrue(!preflight.canPush(), "merge push preflight rejects at-capacity pair");
    expectTrue(fuse::physics::broadphase::canSkipMergePairPush(buffer, 2u, 3u),
               "canSkipMergePairPush true when buffer is full");








void testPairBufferInvalidateSlotPreflightGuards() {


               "invalidate slot rejects for OutOfRangeSlot");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 2u),
               "shouldRunPairBufferInvalidateSlot false for out-of-range slot");

    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot ignores far out-of-range slot");









             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteRejectReason::NoPreparedSlots),
             "unprepared buffer reports NoPreparedSlots write reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWrite(buffer, 0u, 0u, 1u),
               "canSkipPairBufferWrite on unprepared buffer");

             "prepared slot reports None write reject reason");
               "shouldRunPairBufferWrite true for valid prepared slot");

               "write rejects for OutOfRangeSlot");


    expectTrue(preflight.canWrite(), "write preflight accepts valid prepared slot");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateRejectReason::NoPreparedSlots),
             "unprepared buffer reports NoPreparedSlots invalidate reject reason");
               "canSkipPairBufferInvalidate on unprepared buffer");

               "shouldRunPairBufferInvalidate true for valid slot");


             "idempotent invalidate on cleared slot reports None reject reason");

    expectTrue(preflight.canInvalidate(), "invalidate preflight accepts valid unwritten slot");

void testPairBufferSlotValidityBounds() {

    expectTrue(!buffer.slotIsValid(99u), "slotIsValid rejects out-of-range slot");



void testShapeCellOccupancyParamsPreflightGuards() {

    expectTrue(withinBudget.canIterate(), "preflightShapeCellOccupancy accepts range within budget");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsertion(validRange, params),
               "shouldRunShapeCellInsertion true within budget");
    expectTrue(!fuse::physics::broadphase::canSkipShapeCellInsertion(validRange, params),
               "canSkipShapeCellInsertion false within budget");

    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(validRange, params),
               "canSkipShapeCellInsertion true over budget via params");

        fuse::physics::broadphase::preflightShapeCellOccupancy(planeRange, params);
    expectTrue(!planePreflight.canIterate(), "2D params preflight rejects over-budget range");
    expectTrue(!fuse::physics::broadphase::shouldRunShapeCellInsertion(planeRange, params),
               "shouldRunShapeCellInsertion false over budget via params");

void testMergePairIntoBufferPreflightGuards() {
    const fuse::physics::broadphase::MergePairIntoBufferPreflight validPreflight =
        fuse::physics::broadphase::preflightMergePairIntoBuffer(buffer, 0u, 1u);
    expectTrue(validPreflight.canMerge(), "single-pair merge preflight accepts valid pair into empty buffer");
    expectTrue(fuse::physics::broadphase::shouldRunMergePairIntoBuffer(buffer, 0u, 1u),
               "shouldRunMergePairIntoBuffer true for valid pair");

                 fuse::physics::broadphase::mergePairIntoBufferRejectReason(buffer, 2u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairIntoBufferRejectReason::InvalidPair),
             "self-pair reports InvalidPair single merge reject reason");
    expectTrue(fuse::physics::broadphase::mergePairIntoBufferRejectsForReason(
                   buffer, 2u, 2u, fuse::physics::broadphase::MergePairIntoBufferRejectReason::InvalidPair),
               "single merge rejects for InvalidPair");
    expectTrue(std::strcmp(fuse::physics::broadphase::mergePairIntoBufferRejectReasonName(
                               fuse::physics::broadphase::MergePairIntoBufferRejectReason::BufferFull),
               "BufferFull single merge reject reason has stable label");

                 fuse::physics::broadphase::mergePairIntoBufferRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairIntoBufferRejectReason::BufferFull),
             "full buffer reports BufferFull single merge reject reason");
    expectTrue(fuse::physics::broadphase::canSkipMergePairIntoBuffer(buffer, 2u, 3u),
               "canSkipMergePairIntoBuffer on full buffer");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteRejectReason::NoSlotStorage),
             "unprepared buffer reports NoSlotStorage write reject reason");


                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 3u, 0u, 1u)),
                   buffer, 3u, 0u, 1u,

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 1u, 2u, 2u)),


             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateRejectReason::NoSlotStorage),
             "unprepared buffer reports NoSlotStorage invalidate reject reason");


               "invalidate rejects for AlreadyInvalid");

                 fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 5u)),

    expectTrue(!preflight.canInvalidate(), "invalidate preflight rejects empty slot");

void testShapeCellCapacityPreflightGuards() {
    const fuse::physics::broadphase::ShapeCellCapacityPreflight withinBudget =
        fuse::physics::broadphase::preflightShapeCellCapacity(validRange, 8u, 8u);
    expectTrue(withinBudget.canInsert(), "shape cell capacity preflight accepts within budget");
    expectEq(withinBudget.occupancyCount, 8u, "shape cell capacity reports occupancy count");
    expectEq(withinBudget.budgetRemaining, 0u, "shape cell capacity at budget leaves zero headroom");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsertion(validRange, 8u, 8u),

    const fuse::physics::broadphase::ShapeCellCapacityPreflight overBudget =
        fuse::physics::broadphase::preflightShapeCellCapacity(validRange, 8u, 7u);
    expectTrue(!overBudget.canInsert(), "shape cell capacity preflight rejects over budget");
    expectTrue(overBudget.exceedsBudget, "shape cell capacity marks exceedsBudget");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(validRange, 8u, 7u),
               "canSkipShapeCellInsertion true over budget");

    const fuse::physics::broadphase::ShapeCellCapacityPreflight planePreflight =
        fuse::physics::broadphase::preflightShapeCellCapacity(planeRange, 8u, 4u);
    expectTrue(!planePreflight.canInsert(), "2D shape cell capacity preflight rejects over budget");
    expectEq(planePreflight.occupancyCount, 8u, "2D shape cell capacity reports occupancy count");

    const fuse::physics::broadphase::ShapeCellCapacityPreflight clampedPreflight =
        fuse::physics::broadphase::preflightShapeCellCapacity(wideRange, 4u, 0u);
    expectTrue(clampedPreflight.canInsert(), "clamped wide range fits unlimited occupancy budget");
    expectTrue(clampedPreflight.occupancyCount <= 125u, "span clamp limits occupancy before budget check");

void testRefineDedupePreflightPairCountGuards() {

    const fuse::physics::broadphase::RefineBroadphasePreflight emptyRefine =
    expectEq(emptyRefine.pairCount, 0u, "empty refine preflight reports zero pair count");

    const fuse::physics::broadphase::DedupeBroadphasePreflight emptyDedupe =
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectEq(emptyDedupe.pairCount, 0u, "empty dedupe preflight reports zero pair count");


    const fuse::physics::broadphase::RefineBroadphasePreflight refinePreflight =
    expectEq(refinePreflight.pairCount, 2u, "refine preflight reports active pair count");

    const fuse::physics::broadphase::DedupeBroadphasePreflight dedupePreflight =
    expectEq(dedupePreflight.pairCount, 2u, "dedupe preflight reports active pair count");
    expectTrue(dedupePreflight.canDedupe(), "dedupe preflight accepts multiple pairs");

void testBroadphaseMergePreflightBodyCountGuards() {

    expectEq(emptyPreflight.planeBodyCount, 0u, "empty scene has zero plane bodies");
    expectEq(emptyPreflight.dynamicBodyCount, 0u, "empty scene has zero dynamic bodies");

    expectEq(planeOnlyPreflight.planeBodyCount, 1u, "plane-only scene reports one plane body");

    expectEq(mergePreflight.planeBodyCount, 1u, "merge scene reports plane body count");
    expectEq(mergePreflight.dynamicBodyCount, 1u, "merge scene reports dynamic body count");
    expectTrue(mergePreflight.canMerge(), "merge preflight accepts plane plus dynamic scene");

void testMergePairsIntoBufferInsufficientCapacityPreflight() {

    expectTrue(preflight.canMerge(), "partial merge preflight still accepts when some pairs fit");
    expectTrue(preflight.insufficientCapacity, "preflight flags insufficient capacity for full list");
    expectEq(preflight.pairsToMerge, 2u, "preflight reports pairs to merge count");
    expectEq(preflight.remainingCapacity, 1u, "preflight reports remaining capacity");
                               fuse::physics::broadphase::MergePairsIntoBufferRejectReason::InsufficientCapacity),
                           "InsufficientCapacity") == 0,
               "InsufficientCapacity merge reject reason has stable label");

                 fuse::physics::broadphase::mergePairsIntoBufferRejectReason(pairs, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
             "full buffer still reports BufferFull merge reject reason");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::UnpreparedBuffer),
             "unprepared buffer reports UnpreparedBuffer write-slot reject reason");


                   buffer, 0u, 1u, 1u, fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
               "self-pair write-slot rejects for InvalidPair");

    expectEq(static_cast<fuse::u32>(validPreflight.reason),


             "empty buffer reports OutOfRangeSlot invalidate-slot reject reason");

             "far slot reports OutOfRangeSlot invalidate-slot reject reason");

             "already-invalid slot reports AlreadyInvalid invalidate-slot reject reason");
               "AlreadyInvalid invalidate-slot reject reason has stable label");
                   buffer, 0u, fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),

    expectTrue(validPreflight.canInvalidate(), "invalidate-slot preflight accepts valid slot");

    expectTrue(!buffer.slotIsValid(1u), "invalidateSlot ignores far out-of-range slot");

void testCellOccupancyPreflightRemainingBudgetGuards() {
    expectEq(withinBudget.remainingBudget, 0u, "at-budget range reports zero remaining budget");

    const fuse::physics::broadphase::CellOccupancyPreflight headroom =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 10u);
    expectEq(headroom.remainingBudget, 2u, "within-budget range reports remaining budget headroom");
    expectTrue(headroom.canIterate(), "preflight with headroom can iterate");

    expectEq(planePreflight.occupancyCount, 8u, "2D preflight reports occupancy count");
    expectEq(planePreflight.remainingBudget, 2u, "2D preflight reports remaining budget");

void testRefineAndDedupePreflightCountGuards() {

    expectEq(emptyRefine.validPairCount, 0u, "empty refine preflight reports zero valid pairs");


    const fuse::physics::broadphase::RefineBroadphasePreflight slotRefine =
    expectEq(slotRefine.validPairCount, 2u, "prepared slots refine preflight counts valid slots");

    buffer.compact();
    expectEq(dedupePreflight.activePairCount, 2u, "dedupe preflight reports active pair count");

void testMergePairsIntoBufferCapacityPreflightGuards() {

    const fuse::physics::broadphase::MergePairsIntoBufferPreflight openPreflight =
    expectEq(openPreflight.incomingPairCount, 2u, "merge preflight reports incoming pair count");
    expectTrue(openPreflight.remainingCapacity > 0u, "unlimited buffer reports remaining capacity");
    expectTrue(openPreflight.canMerge(), "merge preflight accepts pairs into open buffer");

    buffer.push(4u, 5u);
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight fullPreflight =
    expectEq(fullPreflight.remainingCapacity, 0u, "full buffer merge preflight reports zero remaining capacity");
    expectTrue(fullPreflight.bufferFull, "full buffer merge preflight marks buffer full");
    expectTrue(!fullPreflight.canMerge(), "merge preflight rejects full buffer");

    buffer.clear();
    expectEq(partialPreflight.remainingCapacity, 2u, "partial-capacity merge preflight reports remaining slots");
    expectTrue(partialPreflight.canMerge(), "merge preflight accepts when capacity fits incoming pairs");


               "valid slot write rejects for None");







                   buffer, 0u, fuse::physics::broadphase::PairBufferInvalidateRejectReason::AlreadyInvalid),


    expectTrue(!preflight.canInvalidate(), "invalidate preflight rejects already-invalid slot");

void testCellSpanCapacityGuards() {
    expectTrue(fuse::physics::broadphase::cellSpanWithinPerAxisLimit(withinRange, 4u),
               "within-range span is within per-axis limit");
    expectTrue(!fuse::physics::broadphase::exceedsCellSpanPerAxis(withinRange, 4u),
               "within-range span does not exceed per-axis limit");
    expectTrue(fuse::physics::broadphase::isUnboundedCellSpanPerAxis(0u),
               "zero maxSpanPerAxis is unbounded span stub");
    expectTrue(!fuse::physics::broadphase::exceedsCellSpanPerAxis(withinRange, 0u),
               "unbounded span never exceeds per-axis limit");

    const fuse::physics::broadphase::CellRange3 wideRange = {{-10, 0, 0}, {10, 0, 0}};
               "wide range exceeds per-axis span limit");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanCapacityCheck(withinRange, 8u),
               "shouldRunCellSpanCapacityCheck true within span limit");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {10, 0}};
               "2D wide range exceeds per-axis span limit");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanCapacityCheck(planeRange, 8u),
               "2D canSkipCellSpanCapacityCheck true when span exceeds limit");

    expectTrue(fuse::physics::broadphase::cellSpanWithinPerAxisLimit(inverted, 4u),
               "empty range is within any span limit");

void testCellSpanCapacityRejectReasonGuards() {
                 fuse::physics::broadphase::cellSpanCapacityRejectReason(validRange, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanCapacityRejectReason::None),
             "within-limit range reports None span-capacity reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellSpanCapacityRejectReasonName(
                               fuse::physics::broadphase::CellSpanCapacityRejectReason::ExceedsSpanPerAxis),
               "ExceedsSpanPerAxis span-capacity reject reason has stable label");

                 fuse::physics::broadphase::cellSpanCapacityRejectReason(wideRange, 8u)),
             "wide range reports ExceedsSpanPerAxis span-capacity reject reason");
    expectTrue(fuse::physics::broadphase::cellSpanCapacityRejectsForReason(
                   wideRange, 8u, fuse::physics::broadphase::CellSpanCapacityRejectReason::ExceedsSpanPerAxis),

                 fuse::physics::broadphase::cellSpanCapacityRejectReason(inverted, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanCapacityRejectReason::EmptyRange),
             "inverted range reports EmptyRange span-capacity reject reason");

    const fuse::physics::broadphase::CellSpanCapacityPreflight preflight =
        fuse::physics::broadphase::preflightCellSpanCapacity(wideRange, 8u);
    expectTrue(!preflight.withinSpanLimit(), "span-capacity preflight rejects wide range");
    expectTrue(preflight.exceedsSpanPerAxis, "span-capacity preflight marks exceedsSpanPerAxis");

    const fuse::physics::broadphase::CellSpanCapacityPreflight planePreflight =
        fuse::physics::broadphase::preflightCellSpanCapacity(planeRange, 8u);
    expectTrue(planePreflight.exceedsSpanPerAxis, "2D span-capacity preflight marks exceedsSpanPerAxis");

               "mergePairsIntoBufferRejectsForReason matches empty pair list");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}};

               "mergePairsIntoBufferRejectsForReason matches full buffer");

void testDedupeBroadphasePairBufferLayerGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::dedupeBroadphaseRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::dedupeBroadphaseRejectReason(buffer)),
             "dedupe broadphase and SoA layers agree on empty buffer");

               "shouldRunDedupeBroadphase true for multiple pairs");
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::None),
             "dedupe layers agree on None reject reason for multiple pairs");





    const fuse::physics::broadphase::PairBufferWritePreflight validPreflight =
    expectTrue(validPreflight.canWrite(), "write preflight accepts valid slot");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferWrite(buffer, 1u, 2u, 3u),

    expectTrue(buffer.slotIsValid(0u), "writeSlot rejects self-pair without mutating slot");
    expectEq(buffer.bodyA[0u], 0u, "rejected self-pair write preserves prior slot bodyA");
    expectEq(buffer.bodyB[0u], 1u, "rejected self-pair write preserves prior slot bodyB");


               "in-range slot invalidate rejects for None");
                               fuse::physics::broadphase::PairBufferInvalidateRejectReason::None),
                           "None") == 0,
               "None invalidate reject reason has stable label");

    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidate(buffer, 4u),
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferInvalidate(buffer, 4u),
               "shouldRunPairBufferInvalidate false for out-of-range slot");


void testShapeCellInsertionPreflightGuards() {

               "shouldRunShapeCellInsertion true within params budget");
               "canSkipShapeCellInsertion false within params budget");
                 fuse::physics::broadphase::cellOccupancyRejectReasonForParams(validRange, params)),
             "params-level occupancy reports None within budget");

               "canSkipShapeCellInsertion true over params budget");
    const fuse::physics::broadphase::CellOccupancyPreflight preflight =
        fuse::physics::broadphase::preflightCellOccupancyForParams(validRange, params);
    expectTrue(!preflight.canIterate(), "params-level preflight rejects over-budget range");
    expectTrue(preflight.exceedsBudget, "params-level preflight marks exceedsBudget");

    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(planeRange, params),
               "2D canSkipShapeCellInsertion true over params budget");


                   fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None),
               "non-empty list into empty buffer rejects for None");

               "full buffer rejects for BufferFull");
    expectTrue(!fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, buffer).canMerge(),
               "merge-into-buffer preflight rejects full buffer");

               "unprepared buffer rejects for UnpreparedBuffer");


    expectTrue(validPreflight.canWrite(), "write-slot preflight accepts valid pair into prepared slot");

    expectTrue(buffer.slotIsValid(0u), "writeSlot writes valid pair through preflight gate");



    expectTrue(buffer.slotIsValid(1u), "invalidateSlot ignores out-of-range slot for other valid slots");

             "cleared slot reports AlreadyInvalid invalidate reject reason");

    expectTrue(buffer.invalidateSlotWithPreflight(0u), "invalidateSlotWithPreflight clears valid slot");
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlotWithPreflight leaves slot invalid");
    expectTrue(!buffer.invalidateSlotWithPreflight(0u), "invalidateSlotWithPreflight is no-op on invalid slot");
    expectTrue(!buffer.invalidateSlotWithPreflight(99u), "invalidateSlotWithPreflight ignores out-of-range slot");

    const fuse::physics::broadphase::ShapeCellInsertionPreflight withinBudget =
        fuse::physics::broadphase::preflightShapeCellInsertion(validRange, 8u);
    expectTrue(withinBudget.canInsert(), "shape cell-insertion preflight accepts range within budget");
    expectEq(withinBudget.occupancy.occupancyCount, 8u, "shape cell-insertion preflight reports occupancy count");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsertion(validRange, 8u),
    expectTrue(!fuse::physics::broadphase::canSkipShapeCellInsertion(validRange, 8u),

    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(validRange, 7u),
    expectTrue(!fuse::physics::broadphase::shouldRunShapeCellInsertion(validRange, 7u),
               "shouldRunShapeCellInsertion false over budget");
    const fuse::physics::broadphase::ShapeCellInsertionPreflight validPreflight =
    expectTrue(validPreflight.canInsert(), "shape cell-insertion preflight accepts within-budget range");
    expectEq(validPreflight.occupancyCount, 8u, "shape cell-insertion preflight reports occupancy count");
    expectEq(validPreflight.budgetRemaining, 0u, "shape cell-insertion preflight reports zero budget remaining at limit");

    const fuse::physics::broadphase::CellRange3 overBudgetRange = {{0, 0, 0}, {2, 1, 1}};
                 fuse::physics::broadphase::shapeCellInsertionRejectReason(overBudgetRange, 5u)),
                 fuse::physics::broadphase::ShapeCellInsertionRejectReason::ExceedsOccupancyBudget),
             "over-budget range reports ExceedsOccupancyBudget shape cell-insertion reject reason");
    expectTrue(fuse::physics::broadphase::shapeCellInsertionRejectsForReason(
                   overBudgetRange, 5u,
               "over-budget range rejects for ExceedsOccupancyBudget");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(overBudgetRange, 5u),
               "canSkipShapeCellInsertion true for over-budget range");
    expectTrue(std::strcmp(fuse::physics::broadphase::shapeCellInsertionRejectReasonName(
                               fuse::physics::broadphase::ShapeCellInsertionRejectReason::EmptyRange),
               "EmptyRange shape cell-insertion reject reason has stable label");

    const fuse::physics::broadphase::ShapeCellInsertionPreflight planePreflight =
        fuse::physics::broadphase::preflightShapeCellInsertion(planeRange, 4u);
    expectTrue(!planePreflight.canInsert(), "2D shape cell-insertion preflight rejects over-budget range");

void testRefinePairSlotPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::refinePairSlotRejectReason(buffer, 0u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairSlotRejectReason::OutOfRangeSlot),
             "empty buffer reports OutOfRangeSlot refine-pair-slot reject reason");
    expectTrue(fuse::physics::broadphase::canSkipRefinePairSlot(buffer, 0u, 2u),
               "canSkipRefinePairSlot on empty buffer");

    buffer.push(0u, 5u);
             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairSlotRejectReason::None),
             "valid pushed pair reports None refine-pair-slot reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunRefinePairSlot(buffer, 0u, 2u),
               "shouldRunRefinePairSlot true for valid pair");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::refinePairSlotRejectReason(buffer, 1u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairSlotRejectReason::InvalidPair),
             "out-of-range pair reports InvalidPair refine-pair-slot reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::refinePairSlotRejectReasonName(
                               fuse::physics::broadphase::RefinePairSlotRejectReason::InvalidPair),
               "InvalidPair refine-pair-slot reject reason has stable label");

             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairSlotRejectReason::InvalidSlot),
             "unwritten slot reports InvalidSlot refine-pair-slot reject reason");

void testMergeBroadphasePushPreflightGuards() {
                 fuse::physics::broadphase::mergeBroadphasePushRejectReason(buffer, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergeBroadphasePushRejectReason::None),
    expectTrue(fuse::physics::broadphase::shouldRunMergeBroadphasePush(buffer, 0u, 1u),
               "shouldRunMergeBroadphasePush true for valid pair into empty buffer");

                 fuse::physics::broadphase::mergeBroadphasePushRejectReason(buffer, 2u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergeBroadphasePushRejectReason::InvalidPair),
    expectTrue(std::strcmp(fuse::physics::broadphase::mergeBroadphasePushRejectReasonName(
                               fuse::physics::broadphase::MergeBroadphasePushRejectReason::BufferFull),
               "BufferFull merge push reject reason has stable label");

                 fuse::physics::broadphase::mergeBroadphasePushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergeBroadphasePushRejectReason::BufferFull),
             "full buffer reports BufferFull merge push reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunMergeBroadphasePush(buffer, 2u, 3u),
               "shouldRunMergeBroadphasePush false when buffer is full");

    const fuse::physics::broadphase::MergeBroadphasePushPreflight preflight =
        fuse::physics::broadphase::preflightMergeBroadphasePush(buffer, 2u, 3u);
             "merge push preflight carries reject reason");


             "in-range valid pair reports None write reject reason");
               "shouldRunPairBufferWrite true for valid in-range slot");


                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 0u, 3u, 3u)),

    expectTrue(preflight.canWrite(), "write preflight accepts valid in-range pair");




               "cleared slot rejects for AlreadyInvalid");
               "canSkipPairBufferInvalidate on already-invalid slot");


    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot ignores out-of-range slot via preflight gate");


    expectTrue(preflight.canMerge(), "partial-capacity merge preflight still allows merge");
    expectTrue(preflight.insufficientCapacity,
               "partial-capacity merge preflight flags insufficient batch capacity");
    expectTrue(fuse::physics::broadphase::shouldRunMergePairsIntoBuffer(pairs, buffer),
               "shouldRunMergePairsIntoBuffer true when at least one pair fits");

    const fuse::u32 beforeCount = buffer.activeCount;
    fuse::physics::broadphase::PairBufferSoA mergeBuffer = buffer;
    const std::vector<fuse::physics::broadphase::CandidatePair> mergePairs = pairs;
    for (const fuse::physics::broadphase::CandidatePair& pair : mergePairs) {
        if (!fuse::physics::broadphase::preflightPairBufferPush(mergeBuffer, pair.bodyA, pair.bodyB)
                 .canPush()) {
            break;
        mergeBuffer.push(pair.bodyA, pair.bodyB);
    expectEq(mergeBuffer.activeCount, 2u, "partial merge still pushes pairs until capacity");
    expectTrue(mergeBuffer.activeCount > beforeCount, "partial merge increases active count");

void testCellOccupancyPreflightShapeInsertGuards() {
    const fuse::physics::broadphase::CellRange3 withinRange = {
        fuse::physics::broadphase::preflightCellOccupancy(withinRange, 8u);
    expectTrue(withinPreflight.canIterate(), "small range passes cell-occupancy preflight");
    expectTrue(fuse::physics::broadphase::shouldRunCellOccupancyIteration(withinRange, 8u),
               "shouldRunCellOccupancyIteration true for within-budget range");

    const fuse::physics::broadphase::CellRange3 overBudgetRange = {
        {3, 3, 3},
        fuse::physics::broadphase::preflightCellOccupancy(overBudgetRange, 8u);
    expectTrue(!overPreflight.canIterate(), "large range fails cell-occupancy preflight");
    expectTrue(overPreflight.exceedsBudget, "over-budget preflight flags exceedsBudget");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(overBudgetRange, 8u),
               "canSkipCellOccupancyIteration true when preflight rejects");





































    expectEq(planePreflight.occupancyCount, 8u, "2D shape cell-insertion preflight reports occupancy count");

void testDedupePairBufferSoAWithPreflightGuards() {
    fuse::physics::broadphase::dedupePairBufferSoAWithPreflight(buffer);
    expectTrue(buffer.isEmpty(), "SoA dedupe with preflight is no-op on empty buffer");

    expectEq(buffer.activeCount, 1u, "SoA dedupe with preflight is no-op on single pair");

    expectEq(buffer.activeCount, 2u, "SoA dedupe with preflight removes duplicate pairs");
    expectTrue(buffer.isSortedCanonical(), "SoA dedupe with preflight leaves canonical order");

void testMergePairsIntoBufferPreflightCapacityFields() {
    buffer.setMaxCapacity(4u);

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{4u, 5u}, {6u, 7u}};
    expectTrue(preflight.canMerge(), "merge preflight accepts pairs with remaining capacity");
    expectEq(preflight.incomingPairCount, 2u, "merge preflight reports incoming pair count");
    expectEq(preflight.remainingCapacity, 2u, "merge preflight reports remaining capacity");

    buffer.push(8u, 9u);
    buffer.push(10u, 11u);
    expectTrue(!fullPreflight.canMerge(), "merge preflight rejects when buffer is full");
    expectEq(fullPreflight.remainingCapacity, 0u, "merge preflight reports zero remaining capacity on full buffer");

    expectTrue(!buffer.slotIsValid(0u), "out-of-range invalidate leaves valid slot unchanged");

    expectTrue(preflight.alreadyInvalid, "preflight marks already-invalid slot");
    expectTrue(!preflight.canInvalidate(), "preflight cannot invalidate already-invalid slot");

void testShapeCellInsertionRejectReasonAndPreflight() {
                 fuse::physics::broadphase::shapeCellInsertionRejectReason(validRange, 4u, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertionRejectReason::None),
             "valid range reports None shape cell-insertion reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsertion(validRange, 4u, 8u),
               "shouldRunShapeCellInsertion true within span and budget");

                 fuse::physics::broadphase::shapeCellInsertionRejectReason(validRange, 1u, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertionRejectReason::ExceedsSpan),
             "over-span range reports ExceedsSpan shape cell-insertion reject reason");
                   validRange, 1u, 8u,
                   fuse::physics::broadphase::ShapeCellInsertionRejectReason::ExceedsSpan),
               "over-span range rejects for ExceedsSpan");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(validRange, 1u, 8u),
               "canSkipShapeCellInsertion true when span exceeds budget");

                 fuse::physics::broadphase::shapeCellInsertionRejectReason(validRange, 4u, 7u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertionRejectReason::ExceedsBudget),
             "over-budget range reports ExceedsBudget shape cell-insertion reject reason");
                               fuse::physics::broadphase::ShapeCellInsertionRejectReason::ExceedsBudget),
               "ExceedsBudget shape cell-insertion reject reason has stable label");

    const fuse::physics::broadphase::ShapeCellInsertionPreflight preflight =
        fuse::physics::broadphase::preflightShapeCellInsertion(validRange, 4u, 8u);
    expectTrue(preflight.canInsert(), "shape cell-insertion preflight accepts valid range");
    expectEq(preflight.occupancyCount, 8u, "shape cell-insertion preflight reports occupancy count");

        fuse::physics::broadphase::preflightShapeCellInsertion(planeRange, 4u, 4u);
    expectTrue(planePreflight.exceedsBudget, "2D shape cell-insertion preflight marks exceedsBudget");

void testRefineDedupeMergeWithPreflightReturnGuards() {
    fuse::physics::broadphase::PairBufferSoA dedupeBuffer;
    expectTrue(!fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer),
               "dedupe with preflight returns false on empty buffer");

    dedupeBuffer.push(0u, 1u);
               "dedupe with preflight returns false on single pair");

    dedupeBuffer.push(2u, 3u);
    expectTrue(fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer),
               "dedupe with preflight returns true when dedupe runs");
    expectEq(dedupeBuffer.activeCount, 2u, "dedupe with preflight return true removes duplicate pairs");

    fuse::physics::broadphase::PairBufferSoA mergeBuffer;
    const std::vector<fuse::physics::broadphase::CandidatePair> mergePairs = {{0u, 1u}, {2u, 3u}};
    expectEq(fuse::physics::broadphase::mergePairsIntoBufferWithPreflight(mergePairs, mergeBuffer),
             "merge with preflight returns pushed pair count");

    partialBuffer.setMaxCapacity(3u);
    partialBuffer.push(4u, 5u);
    partialBuffer.push(6u, 7u);
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(mergePairs, partialBuffer);
    expectTrue(partialPreflight.insufficientCapacity, "merge preflight marks insufficientCapacity for partial fit");
    expectEq(partialPreflight.pairsThatFit, 1u, "merge preflight reports pairsThatFit");
    expectEq(fuse::physics::broadphase::mergePairsIntoBufferWithPreflight(mergePairs, partialBuffer),
             1u,
             "merge with preflight returns partial pushed count when capacity is limited");


    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight emptyScenePreflight =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, pairs, buffer);
    expectTrue(emptyScenePreflight.emptyPlaneBodies, "combined merge preflight marks empty plane bodies");
    expectTrue(!emptyScenePreflight.canMerge(), "combined merge preflight cannot merge empty scene");

    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight planeOnlyPreflight =
    expectTrue(planeOnlyPreflight.emptyDynamicBodies, "combined merge preflight marks empty dynamic bodies");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMergeIntoBuffer(bodies, shapes, pairs, buffer),
               "canSkipBroadphaseMergeIntoBuffer on plane-only scene");

    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight validPreflight =
    expectTrue(validPreflight.canMerge(), "combined merge preflight accepts mergeable scene and buffer");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMergeIntoBuffer(bodies, shapes, pairs, buffer),

    buffer.setMaxCapacity(0u);
    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight fullBufferPreflight =
    expectTrue(fullBufferPreflight.bufferFull, "combined merge preflight marks full buffer");
    expectTrue(!fullBufferPreflight.canMerge(), "combined merge preflight cannot merge into full buffer");



             static_cast<fuse::u32>(
               "canSkipPairBufferInvalidateSlot true for out-of-range slot");

                           "OutOfRangeSlot") == 0,
               "OutOfRangeSlot invalidate reject reason has stable label");



        fuse::physics::broadphase::PairBufferPushRejectReason::None;


    buffer.setMaxCapacity(1u);
    buffer.push(0u, 1u);
               "wouldSkipPairBufferPush true when buffer is full");
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),



    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};


void testRefineDedupeMergeWouldSkipGuards() {
    expectTrue(fuse::physics::broadphase::wouldSkipShapeCellInsert(1u, bodies, shapes, params, false) ==
                   fuse::physics::broadphase::canSkipShapeCellInsert(1u, bodies, shapes, params, false),
               "wouldSkipShapeCellInsert agrees with canSkipShapeCellInsert");

void testBroadphaseWouldSkipGuards() {



                   buffer, 3u,

    fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason skipReason =
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u, &skipReason),
    expectEq(static_cast<fuse::u32>(skipReason),
             "wouldSkipPairBufferInvalidateSlot reports reject reason");

void testPairBufferSlotReservationPreflightGuards() {







    const fuse::physics::broadphase::PairBufferSlotReservationPreflight zeroSlots =
        fuse::physics::broadphase::preflightPairBufferSlotReservation(buffer, 0u);
    expectTrue(zeroSlots.zeroSlots, "slot-reservation preflight marks zero slots");
    expectTrue(!zeroSlots.canReserve(), "slot-reservation preflight rejects zero slots");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSlotReservation(buffer, 0u),
               "canSkipPairBufferSlotReservation on zero slots");

    const fuse::physics::broadphase::PairBufferSlotReservationPreflight validReservation =
        fuse::physics::broadphase::preflightPairBufferSlotReservation(buffer, 4u);
    expectTrue(validReservation.canReserve(), "slot-reservation preflight accepts valid count");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSlotReservation(buffer, 4u),
               "shouldRunPairBufferSlotReservation true for valid count");

    const fuse::physics::broadphase::PairBufferSlotReservationPreflight exceedsCapacity =
    buffer.setMaxCapacity(2u);
        fuse::physics::broadphase::preflightPairBufferSlotReservation(buffer, 4u);
    expectTrue(exceedsCapacity.exceedsCapacity, "slot-reservation preflight marks exceeds capacity");
    expectTrue(!exceedsCapacity.canReserve(), "slot-reservation preflight rejects over-capacity count");
    expectEq(exceedsCapacity.requestedSlots, 4u, "slot-reservation preflight reports requested slots");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSlotReservationRejectReasonName(
                               fuse::physics::broadphase::PairBufferSlotReservationRejectReason::ExceedsCapacity),
                           "ExceedsCapacity") == 0,
               "ExceedsCapacity slot-reservation reject reason has stable label");

    fuse::physics::broadphase::PairBufferSlotReservationRejectReason skipReason =
        fuse::physics::broadphase::PairBufferSlotReservationRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferSlotReservation(buffer, 4u, &skipReason),
               "wouldSkipPairBufferSlotReservation true for over-capacity count");
             "wouldSkipPairBufferSlotReservation reports reject reason");

void testRefineDedupeMergePreflightCounts() {



    expectEq(slotRefine.validPairCount, 2u, "refine preflight reports valid slot count");



        fuse::physics::broadphase::preflightRefineBroadphase(bodies, shapes, buffer);

    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);


    expectEq(static_cast<fuse::u32>(skipReason),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferSlotReservationRejectReason::ExceedsCapacity),
}

    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    const fuse::physics::broadphase::RefineBroadphasePreflight emptyRefine =
    expectEq(emptyRefine.validPairCount, 0u, "empty refine preflight reports zero valid pairs");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.writeSlot(1u, 2u, 3u);

    const fuse::physics::broadphase::RefineBroadphasePreflight slotRefine =

    buffer.compact();
    const fuse::physics::broadphase::DedupeBroadphasePreflight dedupePreflight =
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectEq(dedupePreflight.pairCount, 2u, "dedupe preflight reports pair count");

    bodies.addBody({0.f, 0.f, 0.f}, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 2, {0.f, 1.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight mergePreflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectEq(mergePreflight.planeBodyCount, 1u, "merge preflight reports plane body count");
    expectTrue(mergePreflight.dynamicBodyCount >= 2u, "merge preflight reports dynamic body count");
    expectEq(mergePreflight.estimatedMergePairs,
             mergePreflight.planeBodyCount * mergePreflight.dynamicBodyCount,
             "merge preflight estimates plane-dynamic pair count");

void testWouldSkipBroadphaseGuards() {

}

    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    fuse::physics::broadphase::BroadphaseRejectReason broadphaseReason =
        fuse::physics::broadphase::BroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphase(bodies, shapes, &broadphaseReason),
               "wouldSkipBroadphase true on empty scene");
    expectEq(static_cast<fuse::u32>(broadphaseReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::EmptyInput),
             "wouldSkipBroadphase reports EmptyInput");
             "wouldSkipBroadphase reports EmptyInput on empty scene");
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphase(bodies, shapes) ==
                   fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "wouldSkipBroadphase agrees with canSkipBroadphase");


             "wouldSkipCellOccupancyIteration reports None within budget");
    expectEq(static_cast<fuse::u32>(occupancyReason),
             "wouldSkipCellOccupancyIteration reports ExceedsBudget over budget");
               "wouldSkipCellOccupancyIteration agrees with canSkipCellOccupancyIteration");

    const fuse::physics::broadphase::CellRange3 wideRange = {{0, 0, 0}, {5, 5, 5}};
    expectTrue(!fuse::physics::broadphase::wouldSkipCellSpanClamp(wideRange, 4u, &spanReason),
             "wouldSkipCellSpanClamp reports ExceedsSpan when clamp needed");
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(wideRange, 4u) ==
                   !fuse::physics::broadphase::shouldRunCellSpanClamp(wideRange, 4u),
               "wouldSkipCellSpanClamp agrees with shouldRunCellSpanClamp inversion");
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(wideRange, 0u),
               "wouldSkipCellSpanClamp true when span budget is unlimited");

void testWouldSkipRefineDedupeMergeGuards() {
             "wouldSkipCellOccupancyIteration reports ExceedsBudget");


    const fuse::physics::broadphase::CellRange3 overSpanRange = {{0, 0, 0}, {4, 0, 0}};
    expectTrue(!fuse::physics::broadphase::wouldSkipCellSpanClamp(overSpanRange, 3u, &spanReason),
             "wouldSkipCellSpanClamp reports ExceedsSpan");
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(overSpanRange, 3u) ==
                   fuse::physics::broadphase::canSkipCellSpanClamp(overSpanRange, 3u),
               "wouldSkipCellSpanClamp agrees with canSkipCellSpanClamp");

    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    fuse::physics::broadphase::RefineBroadphaseRejectReason refineReason =
        fuse::physics::broadphase::RefineBroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer, &refineReason),
               "wouldSkipRefineBroadphase true on empty scene");
    expectEq(static_cast<fuse::u32>(refineReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyBuffer),
             "empty refine reports EmptyBuffer reject reason");
    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer) ==
                   fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "wouldSkipRefineBroadphase agrees with canSkipRefineBroadphase on empty scene");
             "wouldSkipRefineBroadphase reports EmptyBuffer on empty scene");
               "wouldSkipRefineBroadphase agrees with canSkipRefineBroadphase");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.push(0u, 1u);
    expectTrue(!fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer, &refineReason),
               "wouldSkipRefineBroadphase false for valid refine scene");
    fuse::physics::broadphase::RefineBroadphaseRejectReason refineReason =
        fuse::physics::broadphase::RefineBroadphaseRejectReason::None;
    expectEq(static_cast<fuse::u32>(refineReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::None),
             "wouldSkipRefineBroadphase reports None for valid refine scene");

             "wouldSkipRefineBroadphase reports EmptyBuffer on empty buffer");
               "wouldSkipRefineBroadphase false on valid scene");
             "wouldSkipRefineBroadphase reports None on valid scene");

    fuse::physics::broadphase::DedupeBroadphaseRejectReason dedupeReason =
        fuse::physics::broadphase::DedupeBroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer, &dedupeReason),
               "wouldSkipDedupeBroadphase true on empty buffer");
    expectEq(static_cast<fuse::u32>(dedupeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::EmptyBuffer),
             "empty dedupe reports EmptyBuffer reject reason");
    expectTrue(fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer) ==
                   fuse::physics::broadphase::canSkipDedupeBroadphase(buffer),
               "wouldSkipDedupeBroadphase agrees with canSkipDedupeBroadphase");
               "wouldSkipDedupeBroadphase true for single pair");
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::SinglePair),
             "wouldSkipDedupeBroadphase reports SinglePair");
             "wouldSkipDedupeBroadphase reports EmptyBuffer on empty buffer");
               "wouldSkipDedupeBroadphase true on single pair");

    fuse::physics::broadphase::BroadphaseMergeRejectReason mergeReason =
        fuse::physics::broadphase::BroadphaseMergeRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphaseMerge(bodies, shapes, &mergeReason),
               "wouldSkipBroadphaseMerge true on empty scene");
    expectEq(static_cast<fuse::u32>(mergeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "empty merge reports EmptyPlaneBodies reject reason");

    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs;
    fuse::physics::broadphase::MergePairsIntoBufferRejectReason mergeIntoReason =
        fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(emptyPairs, buffer, &mergeIntoReason),
               "wouldSkipMergePairsIntoBuffer true on empty pair list");
    expectEq(static_cast<fuse::u32>(mergeIntoReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::EmptyPairs),
             "empty merge-into-buffer reports EmptyPairs reject reason");

    expectTrue(!fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer),
               "wouldSkipRefineBroadphase false on valid scene");

    expectTrue(!fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer),
               "wouldSkipDedupeBroadphase false for multiple pairs");

    expectTrue(!fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, mergeBuffer),
               "wouldSkipMergePairsIntoBuffer false for valid merge into empty buffer");

void testBroadphaseWouldSkipGuards() {
    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    buffer.push(2u, 3u);

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}, {2u, 3u}};
    fuse::physics::broadphase::PairBufferSoA mergeBuffer;
}




    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    fuse::physics::broadphase::BroadphaseRejectReason reason =
        fuse::physics::broadphase::BroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphase(bodies, shapes, &reason),
               "wouldSkipBroadphase true on empty scene");
    expectEq(static_cast<fuse::u32>(reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::EmptyInput),
             "empty broadphase reports EmptyInput reject reason");
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphase(bodies, shapes) ==
                   fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "wouldSkipBroadphase agrees with canSkipBroadphase on empty scene");

    expectTrue(!fuse::physics::broadphase::wouldSkipBroadphase(bodies, shapes, &reason),
               "wouldSkipBroadphase false on populated scene");
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::None),
             "populated broadphase reports None reject reason");
               "wouldSkipBroadphaseMerge true without plane bodies");
             "wouldSkipBroadphaseMerge reports EmptyPlaneBodies");
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphaseMerge(bodies, shapes) ==
                   fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "wouldSkipBroadphaseMerge agrees with canSkipBroadphaseMerge");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}};
    expectTrue(!fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, buffer, &mergeIntoReason),
             "wouldSkipBroadphaseMerge reports EmptyPlaneBodies on empty scene");

               "wouldSkipMergePairsIntoBuffer false for valid merge");
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None),
             "wouldSkipMergePairsIntoBuffer reports None for valid merge");

    buffer.setMaxCapacity(1u);
    expectTrue(fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, buffer, &mergeIntoReason),
               "wouldSkipMergePairsIntoBuffer true when buffer is full");
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
             "wouldSkipMergePairsIntoBuffer reports BufferFull when buffer is full");
    expectTrue(fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, buffer) ==
                   fuse::physics::broadphase::canSkipMergePairsIntoBuffer(pairs, buffer),
               "wouldSkipMergePairsIntoBuffer agrees with canSkipMergePairsIntoBuffer");

    const fuse::physics::broadphase::CellRange3 overBudget = {{0, 0, 0}, {4, 4, 4}};
    fuse::physics::broadphase::CellOccupancyRejectReason occupancyReason =
        fuse::physics::broadphase::CellOccupancyRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(overBudget, 8u, &occupancyReason),
               "wouldSkipCellOccupancyIteration true when range exceeds budget");
    expectEq(static_cast<fuse::u32>(occupancyReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "wouldSkipCellOccupancyIteration reports ExceedsBudget");



    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};


    expectTrue(!fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 8u, &occupancyReason),
               "wouldSkipCellOccupancyIteration false within budget");
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u, &occupancyReason),
               "wouldSkipCellOccupancyIteration true over budget");
    expectEq(static_cast<fuse::u32>(occupancyReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "wouldSkipCellOccupancyIteration reports ExceedsBudget");

    const fuse::physics::broadphase::CellRange3 wideRange = {{0, 0, 0}, {7, 0, 0}};
    fuse::physics::broadphase::CellSpanRejectReason spanReason =
        fuse::physics::broadphase::CellSpanRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipCellSpanClamp(wideRange, 4u, &spanReason),
               "wouldSkipCellSpanClamp false for over-span range");
    expectEq(static_cast<fuse::u32>(spanReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpan),
             "wouldSkipCellSpanClamp reports ExceedsSpan");

    fuse::physics::broadphase::PairBufferWriteSlotRejectReason writeReason =
        fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None;
    buffer.preparePairSlots(2u);
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u, &writeReason),
               "wouldSkipPairBufferWriteSlot false for valid write");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u, &writeReason),
               "wouldSkipPairBufferWriteSlot true for self-pair");
    expectEq(static_cast<fuse::u32>(writeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
             "wouldSkipPairBufferWriteSlot reports InvalidPair");
    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(reason),
    expectTrue(!fuse::physics::broadphase::wouldSkipBroadphase(bodies, shapes, &reason),
               "wouldSkipBroadphase false on populated scene");
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::None),
             "populated broadphase reports None reject reason");
    buffer.clear();
    buffer.setMaxCapacity(0u);
    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    expectTrue(!fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer),
               "wouldSkipRefineBroadphase false for valid refine scene");
    expectTrue(!fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer),
               "wouldSkipDedupeBroadphase false for multiple pairs in populated refine scene");
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectTrue(!fuse::physics::broadphase::wouldSkipBroadphaseMerge(bodies, shapes, &mergeReason),
               "wouldSkipBroadphaseMerge false with plane and dynamic bodies");
    expectEq(static_cast<fuse::u32>(mergeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "wouldSkipBroadphaseMerge reports None for mergeable scene");

    fuse::physics::broadphase::PairBufferSoA mergeBuffer;
    fuse::physics::broadphase::MergePairsIntoBufferRejectReason mergeIntoReason =
        fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, mergeBuffer, &mergeIntoReason),
    expectEq(static_cast<fuse::u32>(mergeIntoReason),

    mergeBuffer.setMaxCapacity(1u);
    mergeBuffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, mergeBuffer, &mergeIntoReason),
             "wouldSkipMergePairsIntoBuffer reports BufferFull");
    expectTrue(fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, mergeBuffer) ==
                   fuse::physics::broadphase::canSkipMergePairsIntoBuffer(pairs, mergeBuffer),
}

void testBroadphaseMergeRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
               "shouldRunBroadphase false on singleton scene");

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectTrue(!fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),

    fuse::physics::broadphase::PairBufferSoA buffer;

    expectTrue(fuse::physics::broadphase::pairBufferSortRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
               "empty buffer rejects for EmptyBuffer sort reason");
                           "SinglePair") == 0,

    buffer.push(0u, 1u);

    buffer.push(2u, 3u);

        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectTrue(preflight.needsSort(), "sort preflight accepts multiple pairs with reason None");
    expectEq(static_cast<fuse::u32>(preflight.reason),

void testShouldRunPairBufferDedupeAndSortGuards() {

    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferSort true on empty buffer");

               "shouldRunPairBufferSort false on single pair");

    expectTrue(!fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort false when shouldRunPairBufferSort true");

void testPairSlotPreflightGuards() {
    buffer.setMaxCapacity(2u);

    const fuse::physics::broadphase::PairSlotPreflight zeroSlots =
        fuse::physics::broadphase::preflightPairSlots(0u, buffer);
    expectTrue(zeroSlots.skipped, "zero slot count is skipped");
    expectTrue(!zeroSlots.canPrepare(), "zero slot preflight cannot prepare");

    const fuse::physics::broadphase::PairSlotPreflight withinCapacity =
        fuse::physics::broadphase::preflightPairSlots(2u, buffer);
    expectTrue(withinCapacity.canPrepare(), "slot count within max capacity can prepare");
    expectTrue(!withinCapacity.exceedsBufferCapacity,
               "slot count at max capacity does not exceed buffer capacity");

    const fuse::physics::broadphase::PairSlotPreflight exceedsCapacity =
        fuse::physics::broadphase::preflightPairSlots(4u, buffer);
    expectTrue(exceedsCapacity.canPrepare(), "exceeding slot count may still prepare slots");
    expectTrue(exceedsCapacity.exceedsBufferCapacity,
               "slot count above max capacity flags exceedsBufferCapacity");

    expectEq(static_cast<fuse::u32>(
             "empty buffer reports EmptyBuffer compact+clamp reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(buffer),
               "shouldRunPairBufferCompactAndClamp false on empty buffer");

                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWorkNeeded),
             "all-valid within-capacity buffer reports NoWorkNeeded");
    expectTrue(buffer.canSkipCompactAndClamp(), "all-valid buffer skips compact+clamp");
    expectEq(buffer.compactAndClamp(), 2u, "compactAndClamp no-op preserves active count");

    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(2u, 2u, 3u);
               "sparse slots need compact+clamp work");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactAndClampRejectReasonName(
                           "NoWorkNeeded") == 0,
               "NoWorkNeeded compact+clamp reject reason has stable label");
    expectEq(buffer.compactAndClamp(), 2u, "compactAndClamp gathers sparse valid slots");

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

void testPairListGuards() {
    std::vector<fuse::physics::broadphase::CandidatePair> pairs = {
        {0u, 1u},
        {1u, 1u},
        {0u, 3u},
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

void testPairBufferInvalidPairPrune() {
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

void testBroadphaseOccupancyBudgetIntegration() {

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({500.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {256.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    params.cellSize = 1.f;
    params.tableSize = 256;
    params.maxCellSpanPerAxis = 0u;
    params.maxCellOccupancyCount = 8u;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "occupancy budget skips huge sphere cell insertion");

void testBroadphaseSkipInputGuards() {
    expectTrue(fuse::physics::broadphase::shouldSkipBroadphaseInput(0u, 0u),
               "empty bodies and shapes skip broadphase input");
    expectTrue(fuse::physics::broadphase::shouldSkipCellPairGeneration(1u),
               "single occupant skips cell pair generation");
    expectTrue(!fuse::physics::broadphase::shouldSkipCellPairGeneration(2u),
               "two occupants may generate cell pairs");
    expectTrue(fuse::physics::broadphase::shouldSkipBroadphaseRefine(0u, 0u, 1u, 1u),
               "empty buffer skips refine even with scene data");

    expectTrue(buffer.isEmpty(), "skip-input guard leaves empty pair buffer");



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

    buffer.setMaxCapacity(1u);

    expectTrue(buffer.lastRejectReason == fuse::physics::broadphase::CandidatePairRejectReason::SelfPair,

    expectTrue(buffer.lastRejectReason == fuse::physics::broadphase::CandidatePairRejectReason::BufferFull,
    expectTrue(buffer.wouldRejectPush(2u, 3u), "wouldRejectPush reports full buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(1u, 0u, 1u, 2u);
    expectEq(buffer.compact(), 1u, "writeSlot rejects self-pair and keeps in-range pair");
    expectTrue(buffer.canSkipCompaction(), "compacted buffer skips subsequent compact");

void testPairBufferInvalidateInvalidPairs() {
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

void testCandidatePairRejectReasonWithRefine() {
    expectTrue(fuse::physics::broadphase::isOutOfRangeCandidatePair(0u, 2u, 2u),
               "isOutOfRangeCandidatePair detects OOB indices");
    expectTrue(!fuse::physics::broadphase::isOutOfRangeCandidatePair(0u, 1u, 2u),
               "isOutOfRangeCandidatePair accepts in-range pair");
    expectTrue(fuse::physics::broadphase::isRejectedCandidatePair(1u, 1u),
               "isRejectedCandidatePair flags self-pair");

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});

            fuse::physics::broadphase::CandidatePairRejectReason::AabbSeparated,
    expectTrue(std::strcmp(fuse::physics::broadphase::candidatePairRejectReasonName(
                               fuse::physics::broadphase::CandidatePairRejectReason::BufferFull),
               "BufferFull reject reason has stable label");

void testBroadphase2DCellSpanClampIntegration() {




void testPairBufferSetMaxCapacityTrim() {
    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    expectEq(buffer.activeCount, 2u, "buffer holds two pairs before trim");

    expectTrue(buffer.isFull(), "buffer reports full at max capacity");
    expectEq(buffer.activeCount, 1u, "setMaxCapacity trims excess pairs");
    expectEq(buffer.droppedCount, 1u, "setMaxCapacity records dropped pairs");

void testMaxCellOccupancyBudgetHelpers() {
    expectEq(fuse::physics::broadphase::maxCellOccupancyBudget3D(0u), 0u,
             "zero span clamp means unlimited 3D occupancy budget");
    expectEq(fuse::physics::broadphase::maxCellOccupancyBudget3D(4u), 64u,
             "3D occupancy budget is span cubed");
    expectEq(fuse::physics::broadphase::maxCellOccupancyBudget2D(4u), 16u,
             "2D occupancy budget is span squared");

void testShapeCellOccupancyPreflight() {
    const auto withinBudget =
        fuse::physics::broadphase::preflight_shape_cell_occupancy(unitRange, 4u);
    expectTrue(withinBudget.can_insert(), "clamped unit range passes occupancy preflight");
    expectEq(withinBudget.estimatedCells, 8u, "preflight reports estimated cell count");
    expectEq(withinBudget.maxCells, 64u, "preflight reports max cell budget");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const auto skipped = fuse::physics::broadphase::preflight_shape_cell_occupancy(inverted, 4u);
    expectTrue(skipped.skipped, "empty range skips occupancy preflight");
    expectTrue(!skipped.can_insert(), "skipped occupancy preflight cannot insert");

    const auto planePreflight =
        fuse::physics::broadphase::preflight_shape_cell_occupancy(planeRange, 2u);
    expectTrue(planePreflight.exceedsBudget, "2D preflight flags range above span budget");
    expectTrue(!planePreflight.can_insert(), "over-budget 2D range cannot insert");

void testBroadphasePreflightGuards() {

    const auto emptyPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(emptyPreflight.skipped, "empty scene preflight is skipped");
    expectTrue(!emptyPreflight.can_run(), "empty scene preflight cannot run");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase matches empty preflight");

    const auto validPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(!validPreflight.skipped, "populated scene preflight is not skipped");
    expectTrue(validPreflight.can_run(), "populated scene preflight can run");
    expectEq(validPreflight.bodyCount, 1u, "preflight reports body count");
    expectEq(validPreflight.shapeCount, 1u, "preflight reports shape count");

void testBroadphaseRefinePreflightGuards() {

    const auto emptyPreflight =
        fuse::physics::broadphase::preflight_broadphase_refine(bodies, shapes, buffer);
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty input and buffer");
    expectTrue(!emptyPreflight.can_refine(), "empty refine preflight cannot refine");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseRefine(bodies, shapes, buffer),
               "canSkipBroadphaseRefine matches empty preflight");

    const auto validPreflight =
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid buffer");
    expectTrue(validPreflight.can_refine(), "refine preflight can refine valid buffer");
    expectEq(validPreflight.pairCount, 1u, "refine preflight reports pair count");
    expectTrue(!buffer.canSkipRefine(), "non-empty valid buffer does not skip refine");

void testPairBufferDedupePreflights() {
    const auto emptyPreflight = buffer.preflight_dedupe();
    expectTrue(emptyPreflight.skipped, "empty buffer skips dedupe preflight");
    expectTrue(!emptyPreflight.needs_dedupe(), "empty buffer does not need dedupe");
    expectTrue(!buffer.needsDedupe(), "needsDedupe early-outs on empty buffer");
    expectTrue(!buffer.hasDuplicateCanonicalPairs(), "empty buffer has no duplicate pairs");

    const auto singlePreflight = buffer.preflight_dedupe();
    expectTrue(singlePreflight.skipped, "single-pair buffer skips dedupe preflight");
    expectTrue(!buffer.needsDedupe(), "single pair does not need dedupe");

    expectTrue(buffer.hasDuplicateCanonicalPairs(), "duplicate canonical pair detected");
    const auto duplicatePreflight = buffer.preflight_dedupe();
    expectTrue(!duplicatePreflight.skipped, "duplicate buffer runs dedupe preflight");
    expectEq(duplicatePreflight.duplicateCount, 1u, "preflight counts duplicate pairs");
    expectTrue(duplicatePreflight.needs_dedupe(), "duplicate buffer needs dedupe");
    expectTrue(buffer.needsDedupe(), "needsDedupe matches preflight");

void testBroadphaseInputPreflight() {

    const fuse::physics::broadphase::BroadphaseInputPreflight emptyPreflight =
        fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(emptyPreflight.skipped, "empty input preflight is skipped");
    expectTrue(emptyPreflight.emptyBodies, "empty input preflight marks empty bodies");
    expectTrue(emptyPreflight.emptyShapes, "empty input preflight marks empty shapes");
    expectTrue(!emptyPreflight.can_run(), "empty input preflight cannot run");
    expectTrue(fuse::physics::broadphase::should_skip_broadphase(bodies, shapes),
               "should_skip_broadphase on empty scene");

    const fuse::physics::broadphase::BroadphaseInputPreflight missingShapes =
    expectTrue(missingShapes.skipped, "bodies-only preflight is skipped");
    expectTrue(!missingShapes.emptyBodies, "bodies-only preflight has bodies");
    expectTrue(missingShapes.emptyShapes, "bodies-only preflight marks empty shapes");

    const fuse::physics::broadphase::BroadphaseInputPreflight validPreflight =
    expectTrue(!validPreflight.skipped, "populated input preflight is not skipped");
    expectTrue(validPreflight.can_run(), "populated input preflight can run");
    expectTrue(!fuse::physics::broadphase::should_skip_broadphase(bodies, shapes),
               "should_skip_broadphase on populated scene");

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

    const fuse::physics::broadphase::CellOccupancyPreflight emptyRange =
        fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(emptyRange.skipped, "empty range preflight is skipped");
    expectTrue(emptyRange.emptyRange, "empty range preflight marks empty range");
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyInsert(inverted, 4u),
               "empty range skips insert");

    const fuse::physics::broadphase::CellOccupancyPreflight planePreflight =
        fuse::physics::broadphase::preflight_cell_occupancy(planeRange, 4u);
    expectTrue(planePreflight.exceedsBudget, "2D occupancy preflight flags exceed");

void testBroadphaseCellOccupancyIntegration() {


    params.maxCellSpanPerAxis = 64u;
    params.maxCellOccupancy = 8u;

    expectTrue(buffer.isEmpty(), "occupancy-budgeted huge sphere skips distant body pair");

void testRefineBroadphasePreflight() {

    const fuse::physics::broadphase::RefineBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflight_refine_broadphase(bodies, shapes, buffer);
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty buffer and input");
    expectTrue(emptyPreflight.emptyBuffer, "refine preflight marks empty buffer");
    expectTrue(emptyPreflight.emptyInput, "refine preflight marks empty input");
    expectTrue(fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on empty scene");

    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);

    const fuse::physics::broadphase::RefineBroadphasePreflight validPreflight =
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid pair buffer");
    expectTrue(validPreflight.can_refine(), "valid refine preflight can refine");
    expectTrue(!fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on valid pair buffer");

void testPairBufferDedupePreflight() {

    expectTrue(emptyPreflight.emptyInput, "preflight marks empty scene");
    expectTrue(emptyPreflight.skipped, "preflight skips empty scene");
    expectTrue(!emptyPreflight.can_dispatch(), "preflight cannot dispatch empty scene");
    expectTrue(fuse::physics::broadphase::can_skip_broadphase_dispatch(bodies, shapes),
               "can_skip_broadphase_dispatch on empty scene");

    const auto singletonPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(singletonPreflight.singletonInput, "preflight marks singleton scene");
    expectTrue(singletonPreflight.skipped, "preflight skips singleton scene");

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    expectTrue(!validPreflight.skipped, "preflight does not skip populated scene");
    expectTrue(validPreflight.can_dispatch(), "preflight can dispatch populated scene");
    expectTrue(!fuse::physics::broadphase::can_skip_broadphase_dispatch(bodies, shapes),
               "populated scene does not skip dispatch");

void testRefineBroadphasePreflightGuards() {

    expectTrue(emptyPreflight.skipped, "refine preflight skips empty buffer");
    expectTrue(!emptyPreflight.can_refine(), "refine preflight cannot refine empty buffer");
               "should_skip_refine_broadphase on empty buffer");

    const auto singletonPreflight =
    expectTrue(singletonPreflight.skippedBroadphase, "refine preflight skips singleton broadphase");
    expectTrue(singletonPreflight.skipped, "refine preflight skips singleton scene");

    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid scene");
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

    const auto emptyPreflight = fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(emptyPreflight.emptyRange, "preflight marks empty range");
    expectTrue(emptyPreflight.skipped, "preflight skips empty range");
    expectEq(emptyPreflight.budgetRemaining, 4u, "empty range leaves full budget");

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

    const fuse::physics::broadphase::PairBufferDedupePreflight emptyPreflight =
        fuse::physics::broadphase::preflight_dedupe_pair_buffer(buffer);
    expectTrue(fuse::physics::broadphase::should_skip_dedupe_pair_buffer(buffer),
               "should_skip_dedupe_pair_buffer on empty buffer");

    const fuse::physics::broadphase::PairBufferDedupePreflight singlePreflight =
    expectTrue(!singlePreflight.needs_dedupe(), "single pair does not need dedupe");

    const fuse::physics::broadphase::PairBufferDedupePreflight multiPreflight =
    expectTrue(!multiPreflight.skipped, "multi-pair dedupe preflight is not skipped");
    expectTrue(multiPreflight.needs_dedupe(), "multi-pair buffer needs dedupe");
    expectTrue(multiPreflight.can_dedupe(), "multi-pair buffer can dedupe");
    expectTrue(!fuse::physics::broadphase::should_skip_dedupe_pair_buffer(buffer),
               "should_skip_dedupe_pair_buffer on multi-pair buffer");

void testPairBufferClampPreflight() {

    const fuse::physics::broadphase::PairBufferClampPreflight emptyPreflight =
        fuse::physics::broadphase::preflight_pair_buffer_clamp(buffer);
    expectTrue(emptyPreflight.skipped, "clamp preflight skips empty buffer");
    expectTrue(!emptyPreflight.needs_clamp(), "empty buffer does not need clamp");

    buffer.setMaxCapacity(2u);

    const fuse::physics::broadphase::PairBufferClampPreflight overflowPreflight =
    expectTrue(!overflowPreflight.skipped, "overflow clamp preflight is not skipped");
    expectTrue(overflowPreflight.needs_clamp(), "overflow buffer needs clamp");
    expectTrue(overflowPreflight.can_clamp(), "overflow buffer can clamp");
    expectEq(overflowPreflight.excessCount, 1u, "clamp preflight counts excess pairs");

void testPairBufferDedupeAndClampGuards() {
    expectTrue(buffer.canSkipDedupeAndClamp(), "empty buffer skips dedupe and clamp");

    expectTrue(buffer.canSkipDedupeAndClamp(), "single pair skips dedupe and clamp");

    expectTrue(!buffer.canSkipDedupeAndClamp(), "multi pair does not skip dedupe");

    expectTrue(!buffer.canSkipDedupeAndClamp(), "overflow buffer does not skip clamp");

void testEmptyBroadphaseOutputGuard() {
    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseOutput(buffer),
               "empty buffer is empty broadphase output");

    buffer.preparePairSlots(0u);
               "zero-slot buffer is empty broadphase output");

    expectTrue(!fuse::physics::broadphase::isEmptyBroadphaseOutput(buffer),
               "active pair buffer is not empty output");
































        fuse::physics::broadphase::preflight_cell_occupancy(unitRange, 8u);
    expectTrue(withinBudget.can_insert_cells(), "preflight allows occupancy within budget");
    expectTrue(!withinBudget.skipped, "within-budget preflight is not skipped");

        fuse::physics::broadphase::preflight_cell_occupancy(unitRange, 7u);
    expectTrue(!overBudget.can_insert_cells(), "preflight rejects occupancy over budget");
    expectTrue(overBudget.exceedsBudget, "preflight flags budget overflow");
    expectTrue(overBudget.skipped, "over-budget preflight is skipped");

    const auto emptyRange = fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(emptyRange.emptyRange, "preflight marks inverted range empty");
    expectTrue(!emptyRange.can_insert_cells(), "empty range cannot insert cells");

    expectTrue(planePreflight.exceedsBudget, "2D preflight flags budget overflow");

void testBroadphaseDispatchPreflightGuards() {

        fuse::physics::broadphase::preflight_broadphase_dispatch(bodies, shapes);
    expectTrue(emptyPreflight.emptyInput, "dispatch preflight marks empty scene");
    expectTrue(!emptyPreflight.can_dispatch(), "empty scene cannot dispatch broadphase");

    const auto populatedPreflight =
    expectTrue(!populatedPreflight.emptyInput, "populated scene is not empty input");
    expectTrue(populatedPreflight.can_dispatch(), "populated scene can dispatch broadphase");


    expectTrue(emptyPreflight.skipped, "refine preflight skips empty scene and buffer");


    expectEq(validPreflight.validPairCount, 1u, "refine preflight counts valid pairs");

void testCanSkipRefineBroadphaseIntegration() {

    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase on empty buffer");

    fuse::physics::broadphase::refineBroadphasePairsParallel(bodies, shapes, buffer);
    expectTrue(buffer.isEmpty(), "refine on skipped preflight leaves buffer empty");

void testPairBufferCanSkipCompactAndClamp() {
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer skips compactAndClamp");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs when empty");

    buffer.writeSlot(0u, 0u, 1u);
    expectTrue(buffer.canSkipCompactAndClamp(), "all-invalid slots skip compactAndClamp");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp clears all-invalid slot storage");

    buffer.clear();
    expectTrue(!buffer.canSkipCompactAndClamp(), "overflow buffer does not skip compactAndClamp");
    expectEq(buffer.compactAndClamp(), 1u, "compactAndClamp gathers and clamps overflow");

void testPairBufferSlotValidityBounds() {
    expectTrue(buffer.slotIsValid(0u), "in-range slot is valid");
    expectTrue(!buffer.slotIsValid(2u), "out-of-range slot is invalid");
    expectTrue(buffer.slotIsValid(0u), "out-of-range invalidate is a no-op");

void testPairBufferSparseCanonicalSort() {
    buffer.writeSlot(0u, 2u, 3u);
    buffer.writeSlot(2u, 0u, 1u);
    expectTrue(!buffer.isSortedCanonical(), "sparse unsorted slots are not canonical");
    expectTrue(!buffer.canSkipDedupe(), "multi-slot buffer does not skip dedupe");
    expectEq(buffer.compact(), 2u, "compact gathers sparse valid slots");
    expectTrue(!buffer.isSortedCanonical(), "compact alone does not canonicalize order");
    expectTrue(buffer.isSortedCanonical(), "sortCanonical orders compacted pairs");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "sortCanonical preserves first pair");
    expectTrue(buffer.containsCanonicalPair(2u, 3u), "sortCanonical preserves second pair");

void testBroadphaseInputPreflightGuards() {

    expectTrue(emptyPreflight.emptyBodies, "empty scene reports empty bodies");
    expectTrue(emptyPreflight.emptyShapes, "empty scene reports empty shapes");
    expectTrue(!emptyPreflight.can_build(), "empty scene cannot build broadphase");
    expectTrue(fuse::physics::broadphase::should_skip_broadphase_build(bodies, shapes),
               "should_skip_broadphase_build on empty scene");

    expectTrue(missingShapes.skipped, "bodies without shapes preflight is skipped");
    expectTrue(!missingShapes.emptyBodies, "bodies without shapes still has bodies");
    expectTrue(missingShapes.emptyShapes, "bodies without shapes reports empty shapes");

    expectTrue(validPreflight.can_build(), "populated scene can build broadphase");
    expectTrue(!fuse::physics::broadphase::should_skip_broadphase_build(bodies, shapes),
               "should_skip_broadphase_build on populated scene");

    expectTrue(withinBudget.within_budget(), "occupancy within budget");
    expectTrue(withinBudget.can_insert(), "occupancy can insert within budget");

    expectTrue(overBudget.exceedsBudget, "preflight flags over-budget occupancy");
    expectTrue(!overBudget.within_budget(), "over-budget preflight is not within budget");

    expectTrue(emptyRange.emptyRange, "empty range preflight reports empty range");
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insert(inverted),
               "should_skip_shape_cell_insert on empty range");

    expectEq(planePreflight.occupancyCount, 8u, "2D preflight reports occupancy count");
    expectTrue(planePreflight.exceedsBudget, "2D preflight flags over-budget occupancy");

void testPairBufferPreflightGuards() {
    const fuse::physics::broadphase::PairBufferPreflight emptyPreflight =
        fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(emptyPreflight.skipped, "empty buffer preflight is skipped");
    expectTrue(emptyPreflight.skipDedupe, "empty buffer skips dedupe");
    expectTrue(emptyPreflight.skipCompaction, "empty buffer skips compaction");
    expectTrue(fuse::physics::broadphase::should_skip_pair_buffer_dedupe(buffer),
               "should_skip_pair_buffer_dedupe on empty buffer");
    expectTrue(fuse::physics::broadphase::should_skip_pair_buffer_compaction(buffer),
               "should_skip_pair_buffer_compaction on empty buffer");

    const fuse::physics::broadphase::PairBufferPreflight partialPreflight =
    expectTrue(!partialPreflight.skipped, "partial buffer preflight is not skipped");
    expectTrue(partialPreflight.can_push(1u), "partial buffer can push one more pair");
    expectTrue(!partialPreflight.can_push(2u), "partial buffer rejects two more pairs");
    expectTrue(!partialPreflight.full, "partial buffer is not full");
    expectEq(partialPreflight.remainingCapacity, 1u, "preflight reports remaining capacity");

    const fuse::physics::broadphase::PairBufferPreflight fullPreflight =
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


    expectTrue(emptyPreflight.emptyInput, "refine preflight reports empty input");
    expectTrue(emptyPreflight.emptyBuffer, "refine preflight reports empty buffer");


    expectTrue(!validPreflight.emptyInput, "refine preflight has populated input");
    expectTrue(!validPreflight.emptyBuffer, "refine preflight has valid buffer");
               "should_skip_refine_broadphase on valid scene");

void testPairBufferCompactionPreflightGuards() {
    buffer.writeSlot(1u, 2u, 3u);

    const fuse::physics::broadphase::PairBufferPreflight compactPreflight =
    expectTrue(compactPreflight.skipCompaction, "all-valid slots skip compaction in preflight");
               "should_skip_pair_buffer_compaction on all-valid slots");

    const fuse::physics::broadphase::PairBufferPreflight needsCompactPreflight =
    expectTrue(!needsCompactPreflight.skipCompaction, "invalid slot needs compaction");
    expectTrue(!fuse::physics::broadphase::should_skip_pair_buffer_compaction(buffer),
               "should_skip_pair_buffer_compaction false when invalid slots exist");

void testCellOccupancyPreflightReasonGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectEq(static_cast<fuse::u32>(withinBudget.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "within-budget preflight carries None reason");
    expectTrue(withinBudget.canIterate(), "within-budget preflight can iterate");

        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectEq(static_cast<fuse::u32>(overBudget.reason),
             "over-budget preflight carries ExceedsBudget reason");

        fuse::physics::broadphase::preflightCellOccupancy(inverted, 4u);
    expectEq(static_cast<fuse::u32>(emptyRange.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
             "inverted range preflight carries EmptyRange reason");
    expectEq(static_cast<fuse::u32>(mergePreflight.reason),
             "merge-ready scene carries None merge reject reason");
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
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::broadphaseMergeRejectsForReason(
                   bodies, shapes,
               "broadphaseMergeRejectsForReason matches plane-only scene");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge-ready scene reports None merge reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge true when merge is viable");
}

void testPairBufferPushRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);

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
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity push reject reason");

    const fuse::physics::broadphase::PairBufferPushPreflight validPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u);
    expectEq(static_cast<fuse::u32>(validPush.reason),
             "push preflight carries AtCapacity reason on full buffer");

void testPairBufferCompactionRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compaction reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferCompactionRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
               "pairBufferCompactionRejectsForReason matches empty buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
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

void testPairBufferClampRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer clamp reject reason");

    buffer.setMaxCapacity(2u);
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

void testPairBufferDedupeSortRejectReasonGuards() {
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

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
             "single pair reports SinglePair dedupe reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSortRejectReasonName(
                               fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair sort reject reason has stable label");

    buffer.push(2u, 3u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::None),
             "multiple pairs report None dedupe reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe true for multiple pairs");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort true for multiple pairs");

void testShouldRunBroadphaseAndRefineGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase true on empty scene");

    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");

    expectTrue(fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase true with valid pair buffer");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase false when refine is viable");

void testBroadphaseMergeRejectReasonGuards() {

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
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
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies merge reject reason");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
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
             "empty range preflight carries EmptyRange reason");
    expectTrue(!emptyRange.canIterate(), "empty range preflight cannot iterate");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectEq(static_cast<fuse::u32>(overBudget.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "over-budget preflight carries ExceedsBudget reason");
}

void testPairBufferPushRejectReasonGuards() {
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
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPushRejectReasonName(
                               fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
                           "AtCapacity") == 0,
               "AtCapacity push reject reason has stable label");

    buffer.push(0u, 1u);
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity push reject reason");

    const fuse::physics::broadphase::PairBufferPushPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             "push preflight carries reject reason on full buffer");

void testPairBufferCompactionRejectReasonGuards() {

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
                           "SinglePair") == 0,
               "SinglePair sort reject reason has stable label");

    buffer.push(2u, 3u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::None),
             "multiple pairs report None sort reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(buffer),
               "shouldRunPairBufferSort true for multiple pairs");

    const fuse::physics::broadphase::PairBufferSortPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectTrue(preflight.needsSort(), "sort preflight accepts multiple pairs with reason None");

void testPairBufferDedupeShouldRunGuards() {
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe false on empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe on empty buffer");

               "shouldRunPairBufferDedupe false on single pair");

    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "shouldRunPairBufferDedupe true for multiple pairs");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe false for multiple pairs");

void testPairBufferWriteSlotPreflightGuards() {
    buffer.preparePairSlots(2u);

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight validWrite =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 0u, 1u);
    expectTrue(validWrite.canWrite(), "writeSlot preflight accepts valid slot and pair");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight invalidPair =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 1u, 1u);
    expectTrue(invalidPair.invalidPair, "writeSlot preflight marks self-pair invalid");
    expectTrue(!invalidPair.canWrite(), "writeSlot preflight rejects self-pair");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight outOfRange =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 4u, 0u, 1u);
    expectTrue(outOfRange.outOfRangeSlot, "writeSlot preflight marks out-of-range slot");
    expectEq(static_cast<fuse::u32>(outOfRange.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),
             "writeSlot preflight carries reject reason");

    buffer.writeSlot(0u, 1u, 1u);
    buffer.writeSlot(1u, 0u, 2u);
    expectEq(buffer.compact(), 1u, "writeSlot rejects invalid pair via preflight gate");

void testPairBufferCompactAndClampPreflightGuards() {
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compact-and-clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompactAndClamp(buffer),
               "canSkipPairBufferCompactAndClamp on empty buffer");

    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
             "all-valid slots report AllValid compaction reject reason");

    buffer.preparePairSlots(3u);
    buffer.writeSlot(2u, 2u, 3u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "invalid slots report None compaction reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferCompaction(buffer),
               "canSkipPairBufferCompaction false when compaction is needed");

    const fuse::physics::broadphase::PairBufferCompactionPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferCompaction(buffer);
    expectTrue(preflight.needsCompaction(), "compaction preflight requests work when reason is None");

void testPairBufferClampRejectReasonGuards() {

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

    buffer.setMaxCapacity(2u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
             "within-capacity buffer reports WithinCapacity clamp reject reason");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(2u, 3u);
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(4u, 5u);
    overflowBuffer.setMaxCapacity(2u);
                 fuse::physics::broadphase::pairBufferClampRejectReason(overflowBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::None),
             "overflow buffer reports None clamp reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferClamp(overflowBuffer),
               "canSkipPairBufferClamp false when clamp is needed");

    const fuse::physics::broadphase::PairBufferClampPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferClamp(overflowBuffer);
    expectTrue(preflight.needsClamp(), "clamp preflight requests work when reason is None");

void testShouldRunBroadphaseGuards() {
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWorkNeeded),
             "all-valid within-capacity buffer reports NoWorkNeeded");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(buffer),
               "shouldRunPairBufferCompactAndClamp false when no work needed");

    buffer.invalidateSlot(1u);
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompactAndClamp(buffer),
               "shouldRunPairBufferCompactAndClamp true when invalid slots exist");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactAndClampRejectReasonName(
                           "NoWorkNeeded") == 0,
               "NoWorkNeeded compact-and-clamp reject reason has stable label");

void testBroadphaseShouldRunGuards() {
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
               "canSkipBroadphase true when shouldRunBroadphase false");

               "shouldRunBroadphase false on singleton scene");


    expectTrue(fuse::physics::broadphase::canSkipBroadphasePairGeneration(bodies, shapes),
               "canSkipBroadphasePairGeneration true on singleton scene");

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphasePairGeneration(bodies, shapes),
               "shouldRunBroadphasePairGeneration true on populated scene");

void testCellOccupancyIterationSkipGuards() {
    expectTrue(fuse::physics::broadphase::shouldIterateCellOccupancy(validRange, 8u),
               "shouldIterateCellOccupancy true within budget");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 8u),
               "canSkipCellOccupancyIteration false within budget");

    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(inverted, 4u),
               "canSkipCellOccupancyIteration true on empty range");
    expectTrue(!fuse::physics::broadphase::shouldIterateCellOccupancy(inverted, 4u),
               "shouldIterateCellOccupancy false on empty range");

    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "canSkipCellOccupancyIteration true over budget");

    const fuse::physics::broadphase::CellOccupancyPreflight preflight =
             "cell occupancy preflight carries reject reason");

void testShouldRunRefineBroadphaseGuards() {

    expectTrue(!fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase false on empty scene");

    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);

    expectTrue(fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase true with valid pairs");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase mirrors shouldRunRefineBroadphase inversion");

void testBroadphaseMergeRejectReasonGuards() {

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

    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectTrue(fuse::physics::broadphase::broadphaseMergeRejectsForReason(
                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
               "plane-only scene rejects for EmptyDynamicBodies");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge-ready scene reports None merge reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge true on merge-ready scene");

    const fuse::physics::broadphase::BroadphaseMergePreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
             "merge preflight carries reject reason");


                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 1u, 1u)),

                   buffer, 2u, 3u, fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
               "pairBufferPushRejectsForReason matches AtCapacity");

    const fuse::physics::broadphase::PairBufferPushPreflight pushPreflight =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 3u);
    expectEq(static_cast<fuse::u32>(pushPreflight.reason),
             "push preflight carries reject reason");
    const fuse::physics::broadphase::PairBufferPushPreflight fullPush =
    expectEq(static_cast<fuse::u32>(fullPush.reason),
             "push preflight carries AtCapacity reason");

    fuse::physics::broadphase::PairBufferSoA slotBuffer;
    slotBuffer.preparePairSlots(2u);
    slotBuffer.writeSlot(0u, 0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(slotBuffer)),
             "partial slot buffer reports None compaction reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompaction(slotBuffer),
               "shouldRunPairBufferCompaction true when invalid slots exist");

    slotBuffer.writeSlot(1u, 2u, 3u);
                   slotBuffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
               "all-valid slots reject for AllValid compaction reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompaction(slotBuffer),
               "canSkipPairBufferCompaction on all-valid slots");
             "partial invalid slots report None compaction reject reason");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferCompaction(slotBuffer),
               "partial invalid slots need compaction");

               "all-valid slots skip compaction");

    fuse::physics::broadphase::PairBufferSoA clampBuffer;
    clampBuffer.push(2u, 3u);
    clampBuffer.push(0u, 1u);
    clampBuffer.setMaxCapacity(1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(clampBuffer)),
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

    dedupeBuffer.push(2u, 3u);
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(dedupeBuffer),

    fuse::physics::broadphase::PairBufferSoA sortBuffer;
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSort(sortBuffer),
    sortBuffer.push(2u, 3u);
    sortBuffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSort(sortBuffer),


    expectTrue(!fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase false when shouldRunBroadphase true");

void testCellSpanRejectReasonAndPreflight() {
    const fuse::physics::broadphase::CellRange3 wideRange = {
        {-10, -10, -10},
        {10, 10, 10},
    };
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(wideRange, 8u),
               "wide range exceeds per-axis span budget");
    expectTrue(fuse::physics::broadphase::cellSpanRejectsForReason(
                   wideRange, 8u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsMaxSpan),
               "wide range rejects for ExceedsMaxSpan");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellSpanRejectReasonName(
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsMaxSpan),
                           "ExceedsMaxSpan") == 0,
               "ExceedsMaxSpan reject reason has stable label");

    const fuse::physics::broadphase::CellSpanPreflight clampPreflight =
        fuse::physics::broadphase::preflightCellSpan(wideRange, 8u);
    expectTrue(clampPreflight.needsClamp(), "span preflight requests clamp for wide range");
    expectTrue(clampPreflight.exceedsMaxSpan, "span preflight marks exceedsMaxSpan");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanClamp(wideRange, 8u),
               "shouldRunCellSpanClamp true for wide range");
    expectTrue(!fuse::physics::broadphase::canSkipCellSpanClamp(wideRange, 8u),
               "canSkipCellSpanClamp false for wide range");

    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {1, 1, 1}};
    expectTrue(!fuse::physics::broadphase::exceedsCellSpanPerAxis(unitRange, 8u),
               "unit range is within span budget");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(unitRange, 8u),
               "canSkipCellSpanClamp true within budget");

                 fuse::physics::broadphase::cellSpanRejectReason(inverted, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
             "inverted range reports EmptyRange span reject reason");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {12, 0}};
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(planeRange, 4u),
               "2D range exceeds per-axis span budget");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanClamp(planeRange, 4u),
               "2D shouldRunCellSpanClamp true for wide range");



    const fuse::physics::broadphase::BroadphasePreflight preflight =
        fuse::physics::broadphase::preflightBroadphase(bodies, shapes);
    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes) == preflight.canRun(),
               "shouldRunBroadphase mirrors preflightBroadphase.canRun");

void testCellSpanClampPreflightGuards() {
        {-100, -100, -100},
        {100, 100, 100},
               "wide range should run span clamp");
               "wide range does not skip span clamp");

               "small range skips span clamp within limit");
    expectTrue(!fuse::physics::broadphase::shouldRunCellSpanClamp(unitRange, 8u),
               "small range should not run span clamp");

    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(unitRange, 0u),
               "zero max span skips clamp as unlimited");
    const fuse::physics::broadphase::CellSpanClampPreflight unlimitedPreflight =
        fuse::physics::broadphase::preflightCellSpanClamp(unitRange, 0u);
    expectTrue(unlimitedPreflight.unlimitedSpan, "preflight marks unlimited span clamp");

    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(inverted, 4u),
               "empty range skips span clamp");
    const fuse::physics::broadphase::CellSpanClampPreflight emptyPreflight =
        fuse::physics::broadphase::preflightCellSpanClamp(inverted, 4u);
    expectTrue(emptyPreflight.emptyRange, "preflight marks empty range for span clamp");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {10, 1}};
               "2D wide range should run span clamp");

void testCellSpanClampRejectReasonGuards() {
                 fuse::physics::broadphase::cellSpanClampRejectReason(wideRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::None),
             "wide range reports None span clamp reject reason");
    expectTrue(fuse::physics::broadphase::cellSpanClampRejectsForReason(
                   wideRange, 8u, fuse::physics::broadphase::CellSpanClampRejectReason::None),
               "wide range rejects for None");

                 fuse::physics::broadphase::cellSpanClampRejectReason(unitRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::WithinSpanLimit),
             "small range reports WithinSpanLimit span clamp reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellSpanClampRejectReasonName(
                               fuse::physics::broadphase::CellSpanClampRejectReason::UnlimitedSpan),
                           "UnlimitedSpan") == 0,
               "UnlimitedSpan span clamp reject reason has stable label");

                 fuse::physics::broadphase::cellSpanClampRejectReason(unitRange, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::UnlimitedSpan),
             "zero max span reports UnlimitedSpan reject reason");
void testCellOccupancyPreflightBudgetRemaining() {
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 12u);
    expectEq(withinBudget.budgetRemaining, 4u, "3D preflight reports occupancy budget remaining");
    expectEq(withinBudget.occupancyCount, 8u, "3D preflight still reports occupancy count");

    const fuse::physics::broadphase::CellOccupancyPreflight atBudget =
    expectEq(atBudget.budgetRemaining, 0u, "3D preflight reports zero budget remaining at limit");

    expectEq(emptyRange.budgetRemaining, 4u, "empty range leaves full budget in preflight");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight planePreflight =
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 10u);
    expectEq(planePreflight.budgetRemaining, 2u, "2D preflight reports occupancy budget remaining");

void testPairBufferWriteSlotRejectReasonGuards() {

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
             "valid write-slot reports None reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferWriteSlotRejectsForReason(
                   buffer, 0u, 0u, 1u,
                   fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
               "valid write-slot rejects for None");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
             "self-pair write-slot reports InvalidPair reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferWriteSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),
                           "OutOfRangeSlot") == 0,
               "OutOfRangeSlot write-slot reject reason has stable label");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 4u, 0u, 1u)),
             "out-of-range slot reports OutOfRangeSlot reject reason");


    expectTrue(validWrite.canWrite(), "write-slot preflight accepts valid slot");
    expectEq(static_cast<fuse::u32>(validWrite.reason),
             "write-slot preflight carries reject reason");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight invalidWrite =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 1u, 2u, 2u);
    expectTrue(invalidWrite.invalidPair, "write-slot preflight marks self-pair invalid");
    expectTrue(!invalidWrite.canWrite(), "write-slot preflight rejects self-pair");

    expectEq(buffer.compact(), 1u, "writeSlot wired through preflight rejects self-pair slots");


    expectTrue(fuse::physics::broadphase::pairBufferSortRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
               "empty buffer sort rejects for EmptyBuffer");



                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
                   bodies, shapes,
                   fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
               "broadphaseMergeRejectsForReason matches empty scene");

             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies merge reject reason");

    expectTrue(!fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge false when merge is viable");

             "preflightBroadphaseMerge carries merge reject reason");

void testCellOccupancyCanSkipIterationGuards() {
               "valid range does not skip occupancy iteration");

               "empty range skips occupancy iteration");
    expectTrue(fuse::physics::broadphase::cellOccupancyRejectsForReason(
                   inverted, 4u, fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
               "empty range rejects for EmptyRange");

               "over-budget range skips occupancy iteration");

             "preflightCellOccupancy carries occupancy reject reason");




    expectTrue(!pushPreflight.canPush(), "push preflight rejects at-capacity pair");

                 fuse::physics::broadphase::pairBufferCompactionRejectReason(slotBuffer)),
             "invalid slot reports None compaction reject reason");
    expectTrue(fuse::physics::broadphase::preflightPairBufferCompaction(slotBuffer).needsCompaction(),
               "invalid slot compaction preflight needs work");


                 fuse::physics::broadphase::pairBufferClampRejectReason(clampBuffer)),
    expectTrue(fuse::physics::broadphase::preflightPairBufferClamp(clampBuffer).needsClamp(),
               "overflow clamp preflight needs work");

    clampBuffer.applyMaxCapacityClamp();
    expectTrue(fuse::physics::broadphase::canSkipPairBufferClamp(clampBuffer),
               "canSkipPairBufferClamp on within-capacity buffer");

                 fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
             "single pair reports SinglePair dedupe reject reason");
                   buffer, fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
               "pairBufferDedupeRejectsForReason matches single pair");

                 fuse::physics::broadphase::pairBufferDedupeRejectReason(dedupeBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::None),
             "multiple pairs report None dedupe reject reason");

    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight emptyPreflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(buffer);
    expectTrue(emptyPreflight.canSkipAll(), "empty buffer skips compact-and-clamp");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs on empty buffer");

    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight compactionOnly =
    expectTrue(!compactionOnly.canSkipAll(), "invalid slot buffer needs compact-and-clamp work");
    expectTrue(compactionOnly.compactionNeeded, "invalid slot buffer needs compaction");
    expectTrue(!compactionOnly.clampNeeded, "invalid slot buffer does not need clamp");

    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight bothNeeded =
    expectTrue(bothNeeded.clampNeeded, "two-slot buffer over max capacity needs clamp after compact");
    expectEq(buffer.compactAndClamp(), 1u, "compactAndClamp compacts then clamps overflow slots");
    expectEq(buffer.activeCount, 1u, "compactAndClamp active count after clamp");

void testCellOccupancyPreflightReasonGuards() {
    expectTrue(withinBudget.canIterate(), "within-budget preflight can iterate");


             "inverted range preflight carries EmptyRange reason");



               "broadphaseMergeRejectsForReason matches plane-only scene");

               "shouldRunBroadphaseMerge true when merge is viable");


                   buffer, 1u, 1u, fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),


    const fuse::physics::broadphase::PairBufferPushPreflight validPush =
    expectEq(static_cast<fuse::u32>(validPush.reason),
             "push preflight carries AtCapacity reason on full buffer");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),


    fuse::physics::broadphase::PairBufferSoA sparseBuffer;
    sparseBuffer.preparePairSlots(2u);
    sparseBuffer.writeSlot(0u, 0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(sparseBuffer)),
             "sparse slots report None compaction reject reason");
    expectTrue(fuse::physics::broadphase::preflightPairBufferCompaction(sparseBuffer).needsCompaction(),
               "sparse slots compaction preflight needs work");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),


    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(overflowBuffer)),
    expectTrue(fuse::physics::broadphase::preflightPairBufferClamp(overflowBuffer).needsClamp(),
               "overflow clamp preflight needs truncation");

void testPairBufferDedupeSortRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer dedupe reject reason");



void testShouldRunBroadphaseAndRefineGuards() {
void testPairBufferSlotWritePreflightGuards() {

    const fuse::physics::broadphase::PairBufferSlotWritePreflight validWrite =
        fuse::physics::broadphase::preflightPairBufferSlotWrite(buffer, 0u, 0u, 1u);
    expectTrue(validWrite.canWrite(), "slot-write preflight accepts valid in-range pair");
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSlotWriteRejectReason::None),
             "valid slot write reports None reject reason");

    const fuse::physics::broadphase::PairBufferSlotWritePreflight outOfRange =
        fuse::physics::broadphase::preflightPairBufferSlotWrite(buffer, 3u, 0u, 1u);
    expectTrue(outOfRange.outOfRangeSlot, "slot-write preflight marks out-of-range slot");
    expectTrue(!outOfRange.canWrite(), "slot-write preflight rejects out-of-range slot");
    expectTrue(fuse::physics::broadphase::pairBufferSlotWriteRejectsForReason(
                   buffer, 3u, 0u, 1u,
                   fuse::physics::broadphase::PairBufferSlotWriteRejectReason::OutOfRangeSlot),
               "out-of-range slot rejects for OutOfRangeSlot");

    const fuse::physics::broadphase::PairBufferSlotWritePreflight invalidPair =
        fuse::physics::broadphase::preflightPairBufferSlotWrite(buffer, 1u, 2u, 2u);
    expectTrue(invalidPair.invalidPair, "slot-write preflight marks self-pair invalid");
    expectTrue(!invalidPair.canWrite(), "slot-write preflight rejects self-pair");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSlotWriteRejectReasonName(
                               fuse::physics::broadphase::PairBufferSlotWriteRejectReason::InvalidPair),
                           "InvalidPair") == 0,
               "InvalidPair slot-write reject reason has stable label");

void testPairBufferSlotReservationPreflightGuards() {

void testPairBufferSlotInvalidatePreflightGuards() {


void testPairBufferInvalidateSlotPreflightGuards() {
    expectTrue(buffer.slotIsValid(0u), "writeSlot wired through slot-write preflight");
    buffer.writeSlot(1u, 2u, 2u);
    expectTrue(!buffer.slotIsValid(1u), "writeSlot rejects self-pair via preflight gate");


    const fuse::physics::broadphase::PairBufferSlotInvalidatePreflight validInvalidate =
        fuse::physics::broadphase::preflightPairBufferSlotInvalidate(buffer, 0u);
    expectTrue(validInvalidate.canInvalidate(), "slot-invalidate preflight accepts in-range slot");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSlotInvalidate(buffer, 0u),
               "shouldRunPairBufferSlotInvalidate true for in-range slot");

    buffer.invalidateSlot(0u);
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot wired through slot-invalidate preflight");

    const fuse::physics::broadphase::PairBufferSlotInvalidatePreflight outOfRange =
        fuse::physics::broadphase::preflightPairBufferSlotInvalidate(buffer, 4u);
    expectTrue(outOfRange.outOfRangeSlot, "slot-invalidate preflight marks out-of-range slot");
    expectTrue(!outOfRange.canInvalidate(), "slot-invalidate preflight rejects out-of-range slot");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSlotInvalidate(buffer, 4u),
               "canSkipPairBufferSlotInvalidate on out-of-range slot");
    expectTrue(fuse::physics::broadphase::pairBufferSlotInvalidateRejectsForReason(
                   buffer, 4u,
                   fuse::physics::broadphase::PairBufferSlotInvalidateRejectReason::OutOfRangeSlot),
               "out-of-range invalidate rejects for OutOfRangeSlot");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSlotInvalidateRejectReasonName(
               "OutOfRangeSlot slot-invalidate reject reason has stable label");

    expectEq(static_cast<fuse::u32>(validInvalidate.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSlotInvalidateRejectReason::None),
             "valid slot invalidate reports None reject reason");

        fuse::physics::broadphase::preflightPairBufferSlotInvalidate(buffer, 3u);
                   buffer, 3u,

    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears in-range slot via preflight gate");
    buffer.invalidateSlot(99u);
    expectEq(buffer.countValidSlots(), 0u, "invalidateSlot ignores out-of-range slot");
    const fuse::physics::broadphase::PairBufferInvalidateSlotPreflight validInvalidate =
        fuse::physics::broadphase::preflightPairBufferInvalidateSlot(buffer, 0u);
    expectTrue(validInvalidate.canInvalidate(), "invalidate preflight accepts valid slot");
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None),
             "valid invalidate reports None reject reason");

    const fuse::physics::broadphase::PairBufferInvalidateSlotPreflight outOfRange =
        fuse::physics::broadphase::preflightPairBufferInvalidateSlot(buffer, 5u);
    expectTrue(outOfRange.outOfRangeSlot, "invalidate preflight marks out-of-range slot");
    expectTrue(!outOfRange.canInvalidate(), "invalidate preflight rejects out-of-range slot");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 5u),
               "canSkipPairBufferInvalidateSlot on out-of-range slot");

    const fuse::physics::broadphase::PairBufferInvalidateSlotPreflight alreadyInvalid =
    expectTrue(alreadyInvalid.alreadyInvalid, "invalidate preflight marks already-invalid slot");
    expectTrue(!alreadyInvalid.canInvalidate(), "invalidate preflight rejects already-invalid slot");
void testPairBufferInvalidateSlotRejectReasonGuards() {
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "empty buffer reports OutOfRangeSlot invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "canSkipPairBufferInvalidateSlot on empty buffer");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
                           "AlreadyInvalid") == 0,
               "AlreadyInvalid invalidate reject reason has stable label");

             "valid slot reports None invalidate reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot true for valid slot");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
             "invalidated slot reports AlreadyInvalid reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateSlotRejectsForReason(
                   buffer, 0u,
                   fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
               "already-invalid slot rejects for AlreadyInvalid");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
               "OutOfRangeSlot invalidate reject reason has stable label");


    buffer.invalidateSlot(4u);
    expectTrue(!buffer.slotIsValid(0u), "out-of-range invalidateSlot remains no-op");


    const fuse::physics::broadphase::PairBufferSlotReservationPreflight zeroSlots =
        fuse::physics::broadphase::preflightPairBufferSlotReservation(buffer, 0u);
    expectTrue(zeroSlots.zeroSlots, "slot-reservation preflight marks zero slots");
    expectTrue(!zeroSlots.canReserve(), "slot-reservation preflight rejects zero slots");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferSlotReservation(buffer, 0u),
               "canSkipPairBufferSlotReservation on zero slots");

    const fuse::physics::broadphase::PairBufferSlotReservationPreflight validReservation =
        fuse::physics::broadphase::preflightPairBufferSlotReservation(buffer, 4u);
    expectTrue(validReservation.canReserve(), "slot-reservation preflight accepts valid count");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSlotReservation(buffer, 4u),
               "shouldRunPairBufferSlotReservation true for valid count");

    const fuse::physics::broadphase::PairBufferSlotReservationPreflight exceedsCapacity =
    expectTrue(exceedsCapacity.exceedsCapacity, "slot-reservation preflight marks exceeds capacity");
    expectTrue(!exceedsCapacity.canReserve(), "slot-reservation preflight rejects over-capacity count");
    expectEq(exceedsCapacity.requestedSlots, 4u, "slot-reservation preflight reports requested slots");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSlotReservationRejectReasonName(
                               fuse::physics::broadphase::PairBufferSlotReservationRejectReason::ExceedsCapacity),
                           "ExceedsCapacity") == 0,
               "ExceedsCapacity slot-reservation reject reason has stable label");

void testPairBufferSortSkipGuards() {

               "canSkipPairBufferSort on single pair");

    expectTrue(!fuse::physics::broadphase::canSkipPairBufferSort(buffer),
               "canSkipPairBufferSort false for multiple pairs");

void testCellSpanPreflightGuards() {







    const fuse::physics::broadphase::CellSpanPreflight validPreflight =
        fuse::physics::broadphase::preflightCellSpan(validRange, 4u);
    expectTrue(validPreflight.canClamp(), "cell-span preflight accepts clampable range");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(validRange, 4u),
               "canSkipCellSpanClamp true when span within limit");
    expectTrue(!fuse::physics::broadphase::shouldRunCellSpanClamp(validRange, 4u),
               "shouldRunCellSpanClamp false when span within limit");

    const fuse::physics::broadphase::CellRange3 wideRange = {{0, 0, 0}, {7, 0, 0}};
    const fuse::physics::broadphase::CellSpanPreflight widePreflight =
        fuse::physics::broadphase::preflightCellSpan(wideRange, 4u);
    expectTrue(widePreflight.exceedsMaxSpan, "cell-span preflight marks exceeds max span");
    expectTrue(!widePreflight.canClamp(), "cell-span preflight rejects over-span range");
                   wideRange, 4u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsMaxSpan),
               "over-span range rejects for ExceedsMaxSpan");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanClamp(wideRange, 4u),
               "shouldRunCellSpanClamp true for over-span range");
               "ExceedsMaxSpan cell-span reject reason has stable label");




                   inverted, 4u, fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
               "inverted range rejects for EmptyRange");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {5, 0}};
    const fuse::physics::broadphase::CellSpanPreflight2D planePreflight =
        fuse::physics::broadphase::preflightCellSpan(planeRange, 4u);
    expectTrue(planePreflight.exceedsMaxSpan, "2D cell-span preflight marks exceeds max span");
               "2D shouldRunCellSpanClamp true for over-span range");


               "canSkipBroadphase true on empty scene");



               "shouldRunRefineBroadphase true with valid pair buffer");
               "canSkipRefineBroadphase false when refine is viable");

void testMergeBroadphaseRejectReasonGuards() {

                 fuse::physics::broadphase::mergeBroadphaseRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergeBroadphaseRejectReason::EmptyPlaneBodies),
    expectTrue(fuse::physics::broadphase::mergeBroadphaseRejectsForReason(
                   fuse::physics::broadphase::MergeBroadphaseRejectReason::EmptyPlaneBodies),
               "mergeBroadphaseRejectsForReason matches empty scene");
    expectTrue(std::strcmp(fuse::physics::broadphase::mergeBroadphaseRejectReasonName(
                               fuse::physics::broadphase::MergeBroadphaseRejectReason::EmptyDynamicBodies),

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {0.5f, 0.f, 0.f});
             "dynamic-only scene reports EmptyPlaneBodies merge reject reason");

    shapes.clear();
             static_cast<fuse::u32>(fuse::physics::broadphase::MergeBroadphaseRejectReason::EmptyDynamicBodies),

             static_cast<fuse::u32>(fuse::physics::broadphase::MergeBroadphaseRejectReason::None),
             "plane plus dynamic scene reports None merge reject reason");





    expectTrue(!validPush.canPush(), "push preflight rejects at-capacity pair");

void testPairBufferSortAndCompactionSkipGuards() {

               "canSkipPairBufferCompaction on dense push buffer");


               "canSkipPairBufferCompaction false when invalid slots exist");

    expectTrue(emptyPreflight.canSkip(), "empty buffer compact-and-clamp preflight can skip");

    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight compactionPreflight =
    expectTrue(compactionPreflight.needsCompaction, "slot buffer needs compaction");
    expectTrue(!compactionPreflight.needsClamp, "slot buffer without max capacity skips clamp");
    expectEq(buffer.compactAndClamp(), 1u, "compactAndClamp gathers valid slot");

    overflowBuffer.setMaxCapacity(1u);
    const fuse::physics::broadphase::PairBufferClampPreflight overflowClampPreflight =
    expectTrue(overflowClampPreflight.needsClamp(), "overflow buffer needs post-compact clamp");
    expectEq(overflowBuffer.compactAndClamp(), 1u, "compactAndClamp clamps overflow buffer");

             "within-budget preflight carries None reject reason");

    const fuse::physics::broadphase::CellOccupancyPreflight emptyRangePreflight =
    expectEq(static_cast<fuse::u32>(emptyRangePreflight.reason),
             "empty range preflight carries EmptyRange reject reason");

             "over-budget preflight carries ExceedsBudget reject reason");


    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase inverse of shouldRunRefineBroadphase on empty scene");


               "shouldRunRefineBroadphase true when refine is viable");
               "canSkipRefineBroadphase false when shouldRunRefineBroadphase true");

             "cell occupancy preflight carries None reason within budget");
    expectTrue(withinBudget.canIterate(), "cell occupancy preflight can iterate within budget");

             "cell occupancy preflight carries EmptyRange reason");
    expectTrue(!emptyRange.canIterate(), "cell occupancy preflight cannot iterate empty range");


               "shouldRunBroadphaseMerge false when canSkipBroadphaseMerge true");


               "shouldRunBroadphaseMerge true for merge-ready scene");


void testRefineBroadphaseShouldRunGuards() {



               "shouldRunRefineBroadphase true for valid refine scene");







             "buffer with invalid slots reports None compaction reject reason");
    const fuse::physics::broadphase::PairBufferCompactionPreflight needsWork =
    expectTrue(needsWork.needsCompaction(), "compaction preflight needs work when reason is None");

             "all-valid buffer reports AllValid compaction reject reason");



                   buffer, fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
               "pairBufferClampRejectsForReason matches within-capacity buffer");

    expectTrue(preflight.needsClamp(), "clamp preflight needs work when reason is None");
             "clamp preflight carries reject reason");

void testPairBufferSortAndDedupeSkipGuards() {

               "shouldRunPairBufferDedupe false when canSkipPairBufferDedupe true");


               "canSkipPairBufferDedupe false when shouldRunPairBufferDedupe true");
               "overflow buffer needs clamp");

    fuse::physics::broadphase::PairBufferSoA withinBuffer;
    withinBuffer.push(0u, 1u);
    withinBuffer.setMaxCapacity(2u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(withinBuffer)),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferClamp(withinBuffer),
               "within-capacity buffer skips clamp");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(dedupeBuffer)),


    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(dedupeBuffer)),
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferSort(dedupeBuffer),
               "multiple pairs need sort");
                   dedupeBuffer, fuse::physics::broadphase::PairBufferSortRejectReason::None),
               "sort rejects for None on multi-pair buffer");


             "occupancy preflight carries reject reason");

    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(planeRange, 4u),
               "2D over-budget range skips occupancy iteration");







    expectTrue(validWrite.canWrite(), "write-slot preflight accepts valid slot and pair");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight invalidSlot =
    expectTrue(invalidSlot.invalidSlot, "write-slot preflight marks out-of-range slot");
    expectTrue(!invalidSlot.canWrite(), "write-slot preflight rejects out-of-range slot");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight selfPair =
    expectTrue(selfPair.invalidPair, "write-slot preflight marks self-pair invalid");
    expectTrue(!selfPair.canWrite(), "write-slot preflight rejects self-pair");

        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 0u, 5u, 2u);
    expectTrue(outOfRange.invalidPair, "write-slot preflight marks out-of-range body indices");

void testPairBufferMergePreflightGuards() {
    const fuse::physics::broadphase::PairBufferMergePreflight emptyIncoming =
        fuse::physics::broadphase::preflightPairBufferMerge(buffer, 0u);
    expectTrue(emptyIncoming.emptyIncoming, "merge preflight marks empty incoming");
    expectTrue(!emptyIncoming.canMergeAny(), "merge preflight cannot merge empty incoming");

    const fuse::physics::broadphase::PairBufferMergePreflight partialMerge =
        fuse::physics::broadphase::preflightPairBufferMerge(buffer, 2u);
    expectTrue(partialMerge.wouldTruncate, "merge preflight marks truncation when incoming exceeds capacity");
    expectTrue(partialMerge.canMergeAny(), "merge preflight can merge at least one pair");
    expectTrue(!partialMerge.canMergeAll(), "merge preflight cannot merge all when truncating");

    const fuse::physics::broadphase::PairBufferMergePreflight atCapacity =
        fuse::physics::broadphase::preflightPairBufferMerge(buffer, 1u);
    expectTrue(atCapacity.bufferAtCapacity, "merge preflight marks full buffer");
    expectTrue(!atCapacity.canMergeAny(), "merge preflight cannot merge into full buffer");

void testPairBufferPushBodyCountPreflightGuards() {
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u, 2u);
    expectTrue(validPush.canPush(), "push preflight accepts in-range pair with body count");

    const fuse::physics::broadphase::PairBufferPushPreflight outOfRangePush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 2u, 2u);
    expectTrue(outOfRangePush.invalidPair, "push preflight marks out-of-range body with body count");
    expectTrue(!outOfRangePush.canPush(), "push preflight rejects out-of-range body");

void testRefineBroadphaseEmptyInputRejectReason() {

                 fuse::physics::broadphase::refineBroadphaseRejectReason(bodies, shapes, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyInput),
             "refine with empty input reports EmptyInput reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::refineBroadphaseRejectReasonName(
                               fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyInput),
                           "EmptyInput") == 0,
               "EmptyInput refine reject reason has stable label");

void testRefineBroadphaseNoValidPairsRejectReason() {


    buffer.preparePairSlots(1u);
    expectEq(buffer.activeCount, 0u, "prepared slot buffer has zero active pairs");

             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::NoValidPairs),
             "slot buffer with no valid pairs reports NoValidPairs refine reject reason");
    expectTrue(fuse::physics::broadphase::refineBroadphaseRejectsForReason(
                   bodies, shapes, buffer,
                   fuse::physics::broadphase::RefineBroadphaseRejectReason::NoValidPairs),
               "refineBroadphaseRejectsForReason matches no-valid-pairs buffer");

void testBroadphasePlaneDynamicMergeIntegration() {

    const fuse::u32 sphere = bodies.addBody({0.f, 0.5f, 0.f}, 1.f);
    const fuse::u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sphere, {0.5f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f});

    const fuse::physics::broadphase::BroadphaseMergePreflight mergePreflight =
    expectTrue(mergePreflight.canMerge(), "sphere-over-plane scene passes merge preflight");
               "merge phase runs for sphere-over-plane scene");

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 128;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.containsCanonicalPair(sphere, ground),
               "plane-dynamic merge retains sphere-ground pair");
    expectEq(buffer.activeCount, 1u, "merge dedupe leaves exactly one sphere-ground pair");

void testBroadphaseDedupeRemovesDuplicates() {

    bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {2.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {2.f, 0.f, 0.f});

    params.cellSize = 1.f;
    params.tableSize = 64;
    params.maxCellSpanPerAxis = 0u;

    expectEq(buffer.activeCount, 1u, "dedupe removes duplicate pairs from multi-cell overlap");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "dedupe retains canonical overlapping pair");

               "within-budget range does not skip occupancy iteration");

               "canSkipCellOccupancyIteration on over-budget range");



    expectTrue(fuse::physics::broadphase::canSkipPairBufferPush(buffer, 2u, 3u),
               "canSkipPairBufferPush on full buffer");


void testPairBufferCompactionClampRejectReasonGuards() {

                   buffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
               "pairBufferCompactionRejectsForReason matches all-valid slots");

    clampBuffer.setMaxCapacity(2u);
               "canSkipPairBufferClamp when within capacity");

               "canSkipPairBufferClamp false when overflow needs clamp");

void testPairBufferDedupeRejectReasonGuards() {
                   buffer, fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
               "pairBufferDedupeRejectsForReason matches empty buffer");



    const fuse::physics::broadphase::PairBufferDedupePreflight preflight =
        fuse::physics::broadphase::preflightPairBufferDedupe(buffer);
    expectTrue(preflight.canDedupe(), "dedupe preflight accepts multiple pairs with reason None");

void testRefineAndMergeRejectReasonGuards() {






                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
                           "EmptyPlaneBodies") == 0,
               "EmptyPlaneBodies merge reject reason has stable label");





















void testCanSkipCellOccupancyIterationGuards() {
               "valid range within budget does not skip occupancy iteration");




               "canSkipRefineBroadphase inverse of shouldRun on empty scene");


               "shouldRunRefineBroadphase true on valid refine scene");
               "canSkipRefineBroadphase false when shouldRun true");




               "canSkipBroadphaseMerge false when merge may proceed");


void testPairBufferRejectReasonAndSkipGuards() {

                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::None),
             "valid push reports None push reject reason");
                   buffer, 2u, 2u,
                   fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
               "self-pair rejects for InvalidPair");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferPush(buffer, 2u, 2u),
               "canSkipPairBufferPush on self-pair");


    fuse::physics::broadphase::PairBufferSoA compactionBuffer;
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(compactionBuffer)),
                 fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompaction(compactionBuffer),

    compactionBuffer.push(0u, 1u);

               "canSkipPairBufferClamp false when clamp needed");

                 fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer SoA dedupe reject reason");
                 fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
             "single pair reports SinglePair SoA dedupe reject reason");

    expectTrue(!fuse::physics::broadphase::canSkipPairBufferSort(sortBuffer),
                 fuse::physics::broadphase::pairBufferSortRejectReason(sortBuffer)),
             "multi-pair buffer reports None sort reject reason");

    expectTrue(!emptyPreflight.needsWork(), "empty buffer compact-and-clamp preflight needs no work");

    expectTrue(compactionPreflight.compaction.needsCompaction(),
               "invalid slot compact-and-clamp preflight needs compaction");
    expectTrue(!compactionPreflight.clamp.needsClamp(),
               "within-capacity compact-and-clamp preflight skips clamp");
    expectTrue(compactionPreflight.needsWork(), "invalid slot buffer needs compact-and-clamp work");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferCompactAndClamp(buffer),
               "canSkipPairBufferCompactAndClamp false when compaction needed");

    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight clampPreflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(overflowBuffer);
    expectTrue(!clampPreflight.compaction.needsCompaction(),
               "all-valid overflow buffer skips compaction sub-preflight");
    expectTrue(clampPreflight.clamp.needsClamp(),
               "overflow buffer compact-and-clamp preflight needs clamp");
    expectTrue(clampPreflight.needsWork(), "overflow buffer needs compact-and-clamp work");

void testRefineDedupeMergePreflightCounts() {

void testCellPairGenPreflightGuards() {
    const std::vector<fuse::u32> emptyOccupants;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(emptyOccupants)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::EmptyOccupants),
             "empty occupants report EmptyOccupants cell-pair reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellPairGen(emptyOccupants),
               "canSkipCellPairGen on empty occupants");
    expectEq(fuse::physics::broadphase::countPairsForCellOccupants(emptyOccupants), 0u,
             "empty occupants yield zero pair count");

    const std::vector<fuse::u32> singleOccupant = {3u};
    const fuse::physics::broadphase::CellPairGenPreflight singlePreflight =
        fuse::physics::broadphase::preflightCellPairGen(singleOccupant);
    expectTrue(singlePreflight.singleOccupant, "single occupant preflight marks single occupant");
    expectTrue(!singlePreflight.canGenerate(), "single occupant preflight rejects generation");
    expectEq(singlePreflight.uniqueBodyCount, 1u, "single occupant preflight reports unique body count");

    const std::vector<fuse::u32> multiOccupants = {1u, 2u, 1u, 3u};
    const fuse::physics::broadphase::CellPairGenPreflight multiPreflight =
        fuse::physics::broadphase::preflightCellPairGen(multiOccupants);
    expectTrue(multiPreflight.canGenerate(), "multi-occupant preflight accepts generation");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGen(multiOccupants),
               "shouldRunCellPairGen true for multi-occupant cell");
    expectEq(multiPreflight.uniqueBodyCount, 3u, "multi-occupant preflight dedupes unique bodies");
    expectEq(multiPreflight.pairCount, 3u, "three unique bodies yield three pairs");
    expectEq(fuse::physics::broadphase::countUniqueCellOccupants(multiOccupants), 3u,
             "countUniqueCellOccupants dedupes duplicates");
void testPairBufferWriteRejectReasonGuards() {

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 0u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteRejectReason::None),
             "valid write reports None reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferWrite(buffer, 0u, 0u, 1u),
               "shouldRunPairBufferWrite true for valid slot write");

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 0u, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteRejectReason::InvalidPair),
             "self-pair write reports InvalidPair reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferWriteRejectReasonName(
                               fuse::physics::broadphase::PairBufferWriteRejectReason::OutOfRangeSlot),
               "OutOfRangeSlot write reject reason has stable label");

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 2u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot write reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWrite(buffer, 2u, 0u, 1u),
               "canSkipPairBufferWrite true for out-of-range slot");

    const fuse::physics::broadphase::PairBufferWritePreflight preflight =
        fuse::physics::broadphase::preflightPairBufferWrite(buffer, 1u, 2u, 3u);
    expectTrue(preflight.canWrite(), "write preflight accepts valid in-range slot");
             "write preflight carries reject reason");

void testPairBufferInvalidateRejectReasonGuards() {

                 fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 0u)),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 0u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteRejectReason::UnpreparedBuffer),
             "unprepared buffer reports UnpreparedBuffer write reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWrite(buffer, 0u, 0u, 1u),
               "canSkipPairBufferWrite on unprepared buffer");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 2u, 0u, 1u)),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 0u, 1u, 1u)),
             "self-pair reports InvalidPair write reject reason");

    const fuse::physics::broadphase::PairBufferWritePreflight validWrite =
        fuse::physics::broadphase::preflightPairBufferWrite(buffer, 0u, 0u, 1u);
    expectTrue(validWrite.canWrite(), "write preflight accepts valid slot pair");
               "shouldRunPairBufferWrite true for valid slot pair");
                               fuse::physics::broadphase::PairBufferWriteRejectReason::InvalidPair),
               "InvalidPair write reject reason has stable label");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateRejectReason::OutOfRangeSlot),
             "empty buffer reports OutOfRangeSlot invalidate reject reason");


             "valid slot write reports None write reject reason");

             "self-pair slot write reports InvalidPair write reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWrite(buffer, 0u, 1u, 1u),
               "canSkipPairBufferWrite true for invalid pair");

                 fuse::physics::broadphase::pairBufferWriteRejectReason(buffer, 4u, 0u, 1u)),
             "out-of-range slot write reports OutOfRangeSlot write reject reason");

        fuse::physics::broadphase::preflightPairBufferWrite(buffer, 0u, 2u, 3u);
    expectTrue(preflight.canWrite(), "write preflight accepts valid slot write");


             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateRejectReason::None),
             "valid slot reports None invalidate reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidate(buffer, 0u),
               "shouldRunPairBufferInvalidate true for valid slot");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateRejectReason::AlreadyInvalid),
             "already-invalid slot reports AlreadyInvalid reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateRejectsForReason(
                   buffer, 0u, fuse::physics::broadphase::PairBufferInvalidateRejectReason::AlreadyInvalid),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidate(buffer, 0u),
               "canSkipPairBufferInvalidate true when slot already invalid");

                 fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 2u)),
             "out-of-range invalidate reports OutOfRangeSlot reject reason");
    buffer.invalidateSlot(2u);
    expectTrue(!buffer.slotIsValid(2u), "slotIsValid rejects slot beyond pairSlotCount");

    const fuse::physics::broadphase::PairBufferInvalidatePreflight preflight =
        fuse::physics::broadphase::preflightPairBufferInvalidate(buffer, 1u);
    expectTrue(preflight.canInvalidate(), "invalidate preflight accepts valid slot");

void testCellPairGenRejectReasonGuards() {
                 fuse::physics::broadphase::cellPairGenRejectReason(emptyOccupants)),
    expectTrue(fuse::physics::broadphase::canSkipCellPairGeneration(emptyOccupants),
               "canSkipCellPairGeneration on empty occupants");

                 fuse::physics::broadphase::cellPairGenRejectReason(singleOccupant)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::SingleOccupant),
             "single occupant reports SingleOccupant cell-pair reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellPairGenRejectReasonName(
                               fuse::physics::broadphase::CellPairGenRejectReason::SingleOccupant),
                           "SingleOccupant") == 0,
               "SingleOccupant cell-pair reject reason has stable label");


    const std::vector<fuse::u32> singleton = {0u};
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(singleton)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::SingletonOccupant),
             "singleton occupant reports SingletonOccupant reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellPairGen(singleton),
               "canSkipCellPairGen true for singleton occupant");
    expectTrue(!fuse::physics::broadphase::shouldRunCellPairGen(singleton),
               "shouldRunCellPairGen false for singleton occupant");
    expectEq(fuse::physics::broadphase::countPairsForOccupants(singleton), 0u,
             "countPairsForOccupants returns zero for singleton");

    const std::vector<fuse::u32> pairOccupants = {0u, 1u};
    const fuse::physics::broadphase::CellPairGenPreflight pairPreflight =
        fuse::physics::broadphase::preflightCellPairGen(pairOccupants);
    expectTrue(pairPreflight.canGenerate(), "cell-pair-gen preflight accepts multiple occupants");
    expectEq(pairPreflight.estimatedPairCount, 1u, "cell-pair-gen preflight estimates one pair");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGen(pairOccupants),
               "shouldRunCellPairGen true for pair occupants");
    expectEq(fuse::physics::broadphase::countPairsForOccupants(pairOccupants), 1u,
             "countPairsForOccupants returns one for two unique occupants");

    const std::vector<fuse::u32> tripleOccupants = {0u, 1u, 0u};
    expectEq(fuse::physics::broadphase::countPairsForOccupants(tripleOccupants), 1u,
             "countPairsForOccupants dedupes duplicate occupants");
                               fuse::physics::broadphase::CellPairGenRejectReason::SingletonOccupant),
                           "SingletonOccupant") == 0,
               "SingletonOccupant cell-pair-gen reject reason has stable label");

             "empty occupants report EmptyOccupants cell-pair-gen reject reason");

    const std::vector<fuse::u32> singleOccupant = {0u, 0u};
    expectTrue(singlePreflight.singleOccupant, "single unique occupant marks singleOccupant");
    expectTrue(!singlePreflight.canGenerate(), "cell-pair-gen preflight rejects single occupant");
    expectEq(singlePreflight.uniqueBodyCount, 1u, "single occupant reports one unique body");

    const std::vector<fuse::u32> multiOccupant = {0u, 1u, 0u, 2u};
        fuse::physics::broadphase::preflightCellPairGen(multiOccupant);
    expectTrue(multiPreflight.canGenerate(), "cell-pair-gen preflight accepts multiple unique bodies");
    expectEq(multiPreflight.uniqueBodyCount, 3u, "multi occupant reports three unique bodies");
    expectEq(multiPreflight.estimatedPairCount, 3u, "three unique bodies estimate three pairs");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGen(multiOccupant),
               "SingleOccupant cell-pair-gen reject reason has stable label");

void testShouldRunBroadphasePairGenerationGuards() {


               "shouldRunBroadphasePairGeneration false on singleton scene");




    const fuse::physics::broadphase::RefineBroadphasePreflight emptyRefine =
        fuse::physics::broadphase::preflightRefineBroadphase(bodies, shapes, buffer);
    expectEq(emptyRefine.validPairCount, 0u, "empty refine preflight reports zero valid pairs");


    const fuse::physics::broadphase::RefineBroadphasePreflight slotRefine =



    expectEq(slotRefine.validPairCount, 2u, "refine preflight reports valid slot count");

    buffer.compact();
    const fuse::physics::broadphase::DedupeBroadphasePreflight dedupePreflight =
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectEq(dedupePreflight.pairCount, 2u, "dedupe preflight reports pair count");

    bodies.addBody({0.f, 0.f, 0.f}, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 2, {0.f, 1.f, 0.f});
    expectEq(mergePreflight.planeBodyCount, 1u, "merge preflight reports plane body count");
    expectTrue(mergePreflight.dynamicBodyCount >= 2u, "merge preflight reports dynamic body count");
    expectEq(mergePreflight.estimatedMergePairs,
             mergePreflight.planeBodyCount * mergePreflight.dynamicBodyCount,
             "merge preflight estimates plane-dynamic pair count");

void testBroadphaseMergeBufferPreflightGuards() {

    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer);
    expectTrue(!emptyPreflight.canMergeIntoBuffer(), "empty scene cannot merge into buffer");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMergeIntoBuffer(bodies, shapes, buffer),
               "canSkipBroadphaseMergeIntoBuffer on empty scene");


    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight mergeablePreflight =
    expectTrue(mergeablePreflight.canMergeIntoBuffer(), "mergeable scene can merge into open buffer");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMergeIntoBuffer(bodies, shapes, buffer),
               "shouldRunBroadphaseMergeIntoBuffer true for mergeable scene");

    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight fullPreflight =
    expectTrue(fullPreflight.bufferFull, "full buffer marks bufferFull in merge preflight");
    expectTrue(!fullPreflight.canMergeIntoBuffer(), "full buffer cannot accept merge pairs");

void testRefinableBroadphasePairCountGuards() {

    expectEq(fuse::physics::broadphase::countRefinableBroadphasePairs(bodies, shapes, buffer), 0u,
             "empty scene yields zero refinable pairs");
    expectTrue(!fuse::physics::broadphase::hasRefinableBroadphasePair(bodies, shapes, buffer),
               "hasRefinableBroadphasePair false on empty scene");

    bodies.addBody({20.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 2, {1.f, 0.f, 0.f});

    buffer.push(0u, 2u);
    expectEq(fuse::physics::broadphase::countRefinableBroadphasePairs(bodies, shapes, buffer), 1u,
             "countRefinableBroadphasePairs counts overlapping pair only");
    expectTrue(fuse::physics::broadphase::hasRefinableBroadphasePair(bodies, shapes, buffer),
               "hasRefinableBroadphasePair true when one pair survives refine");

                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::None),
               "multiple pairs reject for None");

             "sort preflight carries reject reason");

void testPairBufferShouldRunDedupeAndSortGuards() {
               "canSkipPairBufferDedupe true when shouldRunPairBufferDedupe false");
               "canSkipPairBufferSort true on empty buffer");

               "shouldRunPairBufferSort false on single pair");

               "canSkipPairBufferSort false when shouldRunPairBufferSort true");

    expectEq(withinBudget.budgetRemaining, 0u, "at-budget range reports zero budget remaining");
    expectEq(withinBudget.occupancyCount, 8u, "at-budget range reports occupancy count");

    const fuse::physics::broadphase::CellOccupancyPreflight headroom =
    expectEq(headroom.budgetRemaining, 4u, "within-budget range reports remaining slots");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {1, 0}};
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 6u);
    expectEq(planePreflight.budgetRemaining, 4u, "2D preflight reports budget remaining");

void testRefineBroadphasePreflightValidPairCount() {

    const fuse::physics::broadphase::RefineBroadphasePreflight emptyPreflight =
    expectEq(emptyPreflight.validPairCount, 0u, "empty refine preflight reports zero valid pairs");


    const fuse::physics::broadphase::RefineBroadphasePreflight validPreflight =
    expectEq(validPreflight.validPairCount, 1u, "valid refine preflight reports active pair count");
    expectTrue(validPreflight.canRefine(), "valid refine preflight can refine");

void testBroadphaseMergePreflightBodyCounts() {

    const fuse::physics::broadphase::BroadphaseMergePreflight emptyPreflight =
    expectEq(emptyPreflight.planeBodyCount, 0u, "empty scene reports zero plane bodies");
    expectEq(emptyPreflight.dynamicBodyCount, 0u, "empty scene reports zero dynamic bodies");

    const fuse::physics::broadphase::BroadphaseMergePreflight planeOnlyPreflight =
    expectEq(planeOnlyPreflight.planeBodyCount, 1u, "plane-only scene reports one plane body");
    expectEq(planeOnlyPreflight.dynamicBodyCount, 0u, "plane-only scene reports zero dynamic bodies");

    expectEq(mergePreflight.planeBodyCount, 1u, "merge scene reports plane body count");
    expectEq(mergePreflight.dynamicBodyCount, 1u, "merge scene reports dynamic body count");
    expectTrue(mergePreflight.canMerge(), "merge scene can merge with populated counts");

void testShouldRunPairBufferDedupeGuards() {



void testDedupeBroadphaseShouldRunIntegration() {
    expectTrue(fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "dedupe integration buffer should run dedupe");

    fuse::physics::broadphase::PairBufferSoA singlePair = buffer;
    singlePair.clear();
    singlePair.push(0u, 1u);
    expectTrue(!fuse::physics::broadphase::shouldRunDedupeBroadphase(singlePair),
               "single-pair buffer skips dedupe integration path");



void testPairBufferSortShouldRunIntegration() {
    expectTrue(!buffer.isSortedCanonical(), "unsorted multi-pair buffer is not canonical");
               "unsorted buffer should run sort");

    buffer.sortCanonical();
    expectTrue(buffer.isSortedCanonical(), "sortCanonical leaves canonical order via shouldRun gate");
               "multi-pair buffer remains sort-eligible after canonical ordering");









    expectTrue(fuse::physics::broadphase::shouldRunPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "shouldRunPairBufferWriteSlot true for valid slot write");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 2u, 0u, 1u)),
                   buffer, 2u, 0u, 1u,
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 2u, 0u, 1u),
               "canSkipPairBufferWriteSlot on out-of-range slot");

             "self-pair slot write reports InvalidPair reject reason");
                               fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),

    expectTrue(!buffer.slotIsValid(0u), "writeSlot rejects self-pair via preflight gate");
    expectTrue(buffer.slotIsValid(1u), "writeSlot accepts valid pair via preflight gate");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 1u, 0u, 2u);
    expectTrue(preflight.canWrite(), "slot-write preflight accepts valid pair");
             "slot-write preflight carries reject reason");

void testPairBufferPrepareSlotsPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferPrepareSlotsRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPrepareSlotsRejectReason::ZeroSlots),
             "zero slot count reports ZeroSlots prepare reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferPrepareSlotsRejectsForReason(
                   0u, fuse::physics::broadphase::PairBufferPrepareSlotsRejectReason::ZeroSlots),
               "zero slot count rejects for ZeroSlots");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferPrepareSlots(0u),
               "canSkipPairBufferPrepareSlots on zero slot count");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferPrepareSlots(0u),
               "shouldRunPairBufferPrepareSlots false on zero slot count");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPrepareSlotsRejectReasonName(
                               fuse::physics::broadphase::PairBufferPrepareSlotsRejectReason::ZeroSlots),
                           "ZeroSlots") == 0,
               "ZeroSlots prepare reject reason has stable label");

    const fuse::physics::broadphase::PairBufferPrepareSlotsPreflight zeroPreflight =
        fuse::physics::broadphase::preflightPairBufferPrepareSlots(0u);
    expectTrue(!zeroPreflight.canPrepare(), "prepare preflight rejects zero slot count");
    expectTrue(zeroPreflight.zeroSlots, "prepare preflight marks zero slots");

    const fuse::physics::broadphase::PairBufferPrepareSlotsPreflight validPreflight =
        fuse::physics::broadphase::preflightPairBufferPrepareSlots(4u);
    expectTrue(validPreflight.canPrepare(), "prepare preflight accepts non-zero slot count");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferPrepareSlots(4u),
               "shouldRunPairBufferPrepareSlots true for non-zero slot count");

    buffer.preparePairSlots(0u);
    expectTrue(buffer.canSkipSoAIteration(), "preparePairSlots(0) clears slot storage via preflight gate");

    const fuse::physics::broadphase::CellRange3 withinRange = {{0, 0, 0}, {3, 3, 3}};
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(withinRange, 8u),
               "canSkipCellSpanClamp true when span is within limit");
    expectTrue(!fuse::physics::broadphase::shouldRunCellSpanClamp(withinRange, 8u),
               "shouldRunCellSpanClamp false when span is within limit");
                 fuse::physics::broadphase::cellSpanClampRejectReason(withinRange, 8u)),
             "within-limit range reports None span-clamp reject reason");

    const fuse::physics::broadphase::CellRange3 wideRange = {{0, 0, 0}, {10, 10, 10}};
               "exceedsCellSpanPerAxis flags wide range");
               "shouldRunCellSpanClamp true when span exceeds limit");
                   wideRange, 8u, fuse::physics::broadphase::CellSpanClampRejectReason::ExceedsSpanPerAxis),
               "wide range rejects for ExceedsSpanPerAxis");
                               fuse::physics::broadphase::CellSpanClampRejectReason::ExceedsSpanPerAxis),
                           "ExceedsSpanPerAxis") == 0,
               "ExceedsSpanPerAxis span-clamp reject reason has stable label");

    const fuse::physics::broadphase::CellSpanClampPreflight preflight =
        fuse::physics::broadphase::preflightCellSpanClamp(wideRange, 8u);
    expectTrue(!preflight.withinSpanLimit(), "span-clamp preflight rejects wide range");
    expectTrue(preflight.exceedsSpanPerAxis, "span-clamp preflight marks exceedsSpanPerAxis");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {12, 1}};
    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(planeRange, 8u),
               "2D exceedsCellSpanPerAxis flags wide range");
                   planeRange, 8u, fuse::physics::broadphase::CellSpanClampRejectReason::ExceedsSpanPerAxis),
               "2D wide range rejects for ExceedsSpanPerAxis");

void testBroadphaseCellPairPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellPairRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroCellSlots),
             "zero cell slots reports ZeroCellSlots reject reason");
    expectTrue(fuse::physics::broadphase::broadphaseCellPairRejectsForReason(
                   0u, fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroCellSlots),
               "zero cell slots rejects for ZeroCellSlots");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseCellPairGeneration(0u),
               "canSkipBroadphaseCellPairGeneration on zero cell slots");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseCellPairGeneration(0u),
               "shouldRunBroadphaseCellPairGeneration false on zero cell slots");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseCellPairRejectReasonName(
                               fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroCellSlots),
                           "ZeroCellSlots") == 0,
               "ZeroCellSlots cell-pair reject reason has stable label");

    const fuse::physics::broadphase::BroadphaseCellPairPreflight zeroPreflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairGeneration(0u);
    expectTrue(!zeroPreflight.canDispatch(), "cell-pair preflight rejects zero cell slots");
    expectTrue(zeroPreflight.zeroCellSlots, "cell-pair preflight marks zeroCellSlots");

    const fuse::physics::broadphase::BroadphaseCellPairPreflight validPreflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairGeneration(4u);
    expectTrue(validPreflight.canDispatch(), "cell-pair preflight accepts non-zero cell slots");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseCellPairGeneration(4u),
               "shouldRunBroadphaseCellPairGeneration true for non-zero cell slots");


               "canSkipBroadphasePairGeneration true when shouldRunBroadphasePairGeneration false");


    expectTrue(!fuse::physics::broadphase::canSkipBroadphasePairGeneration(bodies, shapes),
               "canSkipBroadphasePairGeneration false when shouldRunBroadphasePairGeneration true");

    expectTrue(outOfRange.outOfRangeSlot, "write-slot preflight marks out-of-range slot");
    expectTrue(!outOfRange.canWrite(), "write-slot preflight rejects out-of-range slot");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 4u, 0u, 1u),
               "canSkipPairBufferWriteSlot true for out-of-range slot");

    expectTrue(invalidPair.invalidPair, "write-slot preflight marks self-pair invalid");
    expectTrue(!invalidPair.canWrite(), "write-slot preflight rejects self-pair");

    expectTrue(buffer.slotIsValid(0u), "writeSlot accepts valid pair via preflight gate");


               "valid slot write rejects for None");

               "InvalidPair write-slot reject reason has stable label");


    const fuse::physics::broadphase::CellRange3 inBudget = {{0, 0, 0}, {3, 3, 3}};
    const fuse::physics::broadphase::CellSpanPreflight withinSpan =
        fuse::physics::broadphase::preflightCellSpan(inBudget, 8u);
    expectTrue(!withinSpan.needsClamp(), "in-budget span preflight does not need clamp");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(inBudget, 8u),
               "canSkipCellSpanClamp true within span budget");
    expectTrue(!fuse::physics::broadphase::shouldRunCellSpanClamp(inBudget, 8u),
               "shouldRunCellSpanClamp false within span budget");

    const fuse::physics::broadphase::CellRange3 wideRange = {{-10, 0, 0}, {10, 0, 0}};
    const fuse::physics::broadphase::CellSpanPreflight overSpan =
    expectTrue(overSpan.exceedsMaxSpan, "wide range preflight marks exceedsMaxSpan");
    expectTrue(overSpan.needsClamp(), "wide range preflight needs clamp");

    const fuse::physics::broadphase::CellRange3 clamped =
        fuse::physics::broadphase::clampCellRange3(wideRange, 8u);
    expectTrue(clamped.maxCell.x - clamped.minCell.x <= 8, "clampCellRange3 limits span via preflight gate");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(clamped, 8u),
               "post-clamp range skips further clamp");

    const fuse::physics::broadphase::CellSpanPreflight emptySpan =
        fuse::physics::broadphase::preflightCellSpan(inverted, 4u);
    expectTrue(emptySpan.emptyRange, "inverted range preflight marks emptyRange");
    expectTrue(!emptySpan.needsClamp(), "empty range preflight does not need clamp");

    const fuse::physics::broadphase::CellRange2 planeRange = {{-5, 0}, {5, 0}};
               "2D exceedsCellSpanPerAxis flags wide span");
               "2D shouldRunCellSpanClamp true for wide span");

void testCellSpanRejectReasonGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {2, 2, 2}};
                 fuse::physics::broadphase::cellSpanRejectReason(validRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::None),
             "in-budget range reports None cell-span reject reason");
                   validRange, 8u, fuse::physics::broadphase::CellSpanRejectReason::None),
               "in-budget range rejects for None");

                 fuse::physics::broadphase::cellSpanRejectReason(wideRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::ExceedsMaxSpan),
             "wide range reports ExceedsMaxSpan cell-span reject reason");

             "inverted range reports EmptyRange cell-span reject reason");

                   planeRange, 4u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsMaxSpan),
               "2D wide range rejects for ExceedsMaxSpan");

void testMergePairsIntoBufferRejectsForReasonGuards() {
    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs;

    expectTrue(fuse::physics::broadphase::mergePairsIntoBufferRejectsForReason(
                   emptyPairs, buffer,
                   fuse::physics::broadphase::MergePairsIntoBufferRejectReason::EmptyPairs),
               "mergePairsIntoBufferRejectsForReason matches empty pair list");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}, {2u, 3u}};
                   pairs, buffer, fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None),
               "valid merge rejects for None");

                   pairs, buffer,
                   fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
               "mergePairsIntoBufferRejectsForReason matches full buffer");
    expectTrue(fuse::physics::broadphase::canSkipMergePairsIntoBuffer(pairs, buffer),
               "canSkipMergePairsIntoBuffer true when buffer is full");

void testBroadphaseDedupeAndClampPreflightIntegration() {
               "single-pair buffer skips SoA dedupe preflight");

               "multi-pair buffer runs SoA dedupe preflight");

               "overflow buffer runs clamp preflight");
    expectEq(clampBuffer.applyMaxCapacityClamp(), 1u, "applyMaxCapacityClamp honors clamp preflight gate");


    params.cellSize = 4.f;

    fuse::physics::broadphase::PairBufferSoA broadphaseBuffer;
    broadphaseBuffer.setMaxCapacity(1u);
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, broadphaseBuffer);
    expectEq(broadphaseBuffer.activeCount, 1u, "broadphase clamp preflight gate truncates overflow");
    expectTrue(broadphaseBuffer.droppedCount >= 1u, "broadphase clamp preflight records dropped pairs");


             "valid writeSlot reports None reject reason");
               "shouldRunPairBufferWriteSlot true for valid slot");


             "self-pair writeSlot reports InvalidPair reject reason");
               "InvalidPair writeSlot reject reason has stable label");

    buffer.writeSlot(1u, 1u, 1u);
    expectEq(buffer.compact(), 1u, "writeSlot preflight gate rejects self-pair slots");

        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 1u, 2u, 3u);
    expectTrue(!preflight.canWrite(), "writeSlot preflight rejects out-of-range slot after compact");

void testPairBufferAcceptPairsRejectReasonGuards() {

                 fuse::physics::broadphase::pairBufferAcceptPairsRejectReason(buffer, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::None),
             "empty buffer accepts two pairs");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferAcceptPairs(buffer, 2u),
               "shouldRunPairBufferAcceptPairs true when capacity allows");

                 fuse::physics::broadphase::pairBufferAcceptPairsRejectReason(buffer, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::AtCapacity),
             "empty buffer rejects three pairs");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferAcceptPairs(buffer, 3u),
               "canSkipPairBufferAcceptPairs true when batch exceeds capacity");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferAcceptPairsRejectReasonName(
                               fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::AtCapacity),
               "AtCapacity accept-pairs reject reason has stable label");

                 fuse::physics::broadphase::pairBufferAcceptPairsRejectReason(buffer, 1u)),
             "full buffer rejects another pair batch");
    expectTrue(buffer.cannotAcceptPairs(1u), "cannotAcceptPairs mirrors accept preflight");

    const fuse::physics::broadphase::PairBufferAcceptPairsPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferAcceptPairs(buffer, 1u);
    expectTrue(!preflight.canAccept(), "accept-pairs preflight rejects full buffer");
    expectTrue(preflight.atCapacity, "accept-pairs preflight marks at capacity");

void testCellSpanRejectReasonAndPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {3, 3, 3}};
    expectTrue(!fuse::physics::broadphase::exceedsCellSpanPerAxis(validRange, 8u),
               "valid range does not exceed span limit");
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellSpanRejectReason(validRange, 8u)),
             "valid range reports None span reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanClamp(validRange, 8u),
               "shouldRunCellSpanClamp true for clampable range");

    expectTrue(fuse::physics::broadphase::exceedsCellSpanPerAxis(validRange, 2u),
               "wide range exceeds per-axis span limit");
                   validRange, 2u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanLimit),
               "wide range rejects for ExceedsSpanLimit");
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanLimit),
                           "ExceedsSpanLimit") == 0,
               "ExceedsSpanLimit span reject reason has stable label");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellSpanRejectReason(inverted, 4u)),
               "canSkipCellSpanClamp true for empty range");

    const fuse::physics::broadphase::CellSpanPreflight preflight =
        fuse::physics::broadphase::preflightCellSpan(validRange, 2u);
    expectTrue(!preflight.canClamp(), "span preflight rejects over-limit range");
    expectTrue(preflight.exceedsSpanLimit, "span preflight marks exceedsSpanLimit");
    expectTrue(preflight.spanPerAxis.x >= 4, "span preflight reports per-axis span");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {5, 1}};
    const fuse::physics::broadphase::CellSpanPreflight planePreflight =
        fuse::physics::broadphase::preflightCellSpan2D(planeRange, 2u);
    expectTrue(!planePreflight.canClamp(), "2D span preflight rejects over-limit range");
                   planeRange, 2u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanLimit),
               "2D wide range rejects for ExceedsSpanLimit");

void testRefineBroadphasePreflightRejectsForReasonGuards() {

    expectTrue(fuse::physics::broadphase::refineBroadphasePreflightRejectsForReason(
                   fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyBuffer),
               "refine preflight rejects for EmptyBuffer on empty scene");


                   fuse::physics::broadphase::RefineBroadphaseRejectReason::None),
               "refine preflight rejects for None on valid scene");
               "shouldRunRefineBroadphase true when preflight reason is None");

void testDedupeBroadphasePreflightRejectsForReasonGuards() {

    expectTrue(fuse::physics::broadphase::dedupeBroadphasePreflightRejectsForReason(
                   buffer, fuse::physics::broadphase::DedupeBroadphaseRejectReason::EmptyBuffer),
               "dedupe preflight rejects for EmptyBuffer");
    expectTrue(fuse::physics::broadphase::canSkipDedupeBroadphase(buffer),
               "canSkipDedupeBroadphase on empty buffer");

                   buffer, fuse::physics::broadphase::DedupeBroadphaseRejectReason::SinglePair),
               "dedupe preflight rejects for SinglePair");

                   buffer, fuse::physics::broadphase::DedupeBroadphaseRejectReason::None),
               "dedupe preflight rejects for None with multiple pairs");
               "shouldRunDedupeBroadphase true when preflight reason is None");

void testMergeBroadphasePreflightRejectsForReasonGuards() {

    expectTrue(fuse::physics::broadphase::mergeBroadphasePreflightRejectsForReason(
               "merge preflight rejects for EmptyPlaneBodies on empty scene");

               "merge preflight rejects for EmptyDynamicBodies on plane-only scene");

                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
               "merge preflight rejects for None on mergeable scene");
               "canSkipBroadphaseMerge false when preflight reason is None");





void testShapeCellInsertPreflightGuards() {

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 99, {1.f, 0.f, 0.f});

    params.maxCellOccupancy = 0u;

    const fuse::physics::broadphase::ShapeCellInsertPreflight orphanPreflight =
        fuse::physics::broadphase::preflightShapeCellInsert(0u, bodies, shapes, params, false);
    expectTrue(orphanPreflight.outOfRangeBody, "orphan shape marks out-of-range body");
    expectTrue(!orphanPreflight.canInsert(), "orphan shape cannot insert into cells");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsert(0u, bodies, shapes, params, false),
               "canSkipShapeCellInsert true for orphan shape");

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {256.f, 0.f, 0.f});
    params.maxCellOccupancy = 8u;

    const fuse::physics::broadphase::ShapeCellInsertPreflight budgetPreflight =
    expectTrue(budgetPreflight.occupancyRejected, "huge shape marks occupancy rejected");
    expectEq(static_cast<fuse::u32>(budgetPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::OccupancyRejected),
             "huge shape reports OccupancyRejected reason");
    expectTrue(!fuse::physics::broadphase::shouldRunShapeCellInsert(0u, bodies, shapes, params, false),
               "shouldRunShapeCellInsert false when occupancy budget rejects");

void testBroadphaseCellPairGenPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellPairGenRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairGenRejectReason::EmptyCells),
             "zero cell slots reports EmptyCells reject reason");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseCellPairGen(0u),
               "canSkipBroadphaseCellPairGen true for zero slots");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseCellPairGen(0u),
               "shouldRunBroadphaseCellPairGen false for zero slots");

    const fuse::physics::broadphase::BroadphaseCellPairGenPreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairGen(4u);
    expectTrue(preflight.canGenerate(), "non-zero slots can generate cell pairs");
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairGenRejectReason::None),
             "cell-pair preflight carries reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseCellPairGenRejectReasonName(
                               fuse::physics::broadphase::BroadphaseCellPairGenRejectReason::EmptyCells),
                           "EmptyCells") == 0,
               "EmptyCells cell-pair reject reason has stable label");




void testPairBufferShouldRunDedupeGuards() {



             "in-range valid pair reports None write-slot reject reason");

             "out-of-range slot reports OutOfRangeSlot write-slot reject reason");

             "self-pair reports InvalidPair write-slot reject reason");
    expectTrue(preflight.invalidPair, "write-slot preflight marks invalid pair");
    expectTrue(!preflight.canWrite(), "write-slot preflight rejects self-pair");

    expectTrue(buffer.slotIsValid(0u), "writeSlot ignores invalid overwrite via preflight gate");

void testPairBufferInvalidateSlotRejectReasonGuards() {

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             "valid slot reports None invalidate-slot reject reason");
    expectTrue(validWrite.canWrite(), "write-slot preflight accepts valid pair in range");
               "shouldRunPairBufferWriteSlot true for valid pair");


                   buffer, 0u, 1u, 1u, fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
               "write-slot rejects for InvalidPair reason");

    const fuse::physics::broadphase::PairBufferWriteSlotPreflight outOfSlotWrite =
    expectTrue(outOfSlotWrite.outOfSlot, "write-slot preflight marks out-of-slot index");
               "canSkipPairBufferWriteSlot true for out-of-slot write");


    expectTrue(!buffer.slotIsValid(1u), "writeSlot rejects invalid pair via preflight gate");
    expectTrue(validWrite.canWrite(), "write-slot preflight accepts valid in-range pair");
             "valid write-slot preflight carries None reason");

    expectTrue(invalidPair.invalidPair, "write-slot preflight marks invalid pair");
               "write-slot rejectsForReason matches self-pair");

        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 2u, 0u, 1u);

    buffer.writeSlot(2u, 0u, 1u);
    expectTrue(!buffer.slotIsValid(2u), "writeSlot ignores out-of-range slot via preflight gate");


               "shouldRunPairBufferWriteSlot true for valid write");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 2u, 2u, 3u)),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 2u, 2u, 3u),


    expectTrue(preflight.canWrite(), "write-slot preflight accepts valid pair");


    expectTrue(validInvalidate.canInvalidate(), "invalidate-slot preflight accepts valid slot");




    expectTrue(buffer.slotIsValid(0u), "writeSlot rejects self-pair via preflight gate");


    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),


                   buffer, 0u, 1u, 1u,
               "write-slot rejects for InvalidPair");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u),
               "canSkipPairBufferWriteSlot true for invalid pair");

                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 3u, 0u, 1u)),

    expectTrue(preflight.canWrite(), "write-slot preflight accepts valid slot");

    buffer.writeSlot(3u, 0u, 1u);
    expectTrue(!buffer.slotIsValid(3u), "writeSlot rejects out-of-range slot via preflight gate");





                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 2u, 2u)),



    expectTrue(fuse::physics::broadphase::shouldRunPairBufferInvalidateSlot(buffer, 0u),
               "shouldRunPairBufferInvalidateSlot true for valid slot");

             "already-invalid slot reports AlreadyInvalid invalidate-slot reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "canSkipPairBufferInvalidateSlot true for already-invalid slot");

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 99u)),
             "far out-of-range slot reports OutOfRangeSlot invalidate-slot reject reason");
                               fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::
                                   OutOfRangeSlot),
               "OutOfRangeSlot invalidate-slot reject reason has stable label");
    expectTrue(validInvalidate.canInvalidate(), "invalidate-slot preflight accepts in-range slot");

        fuse::physics::broadphase::preflightPairBufferInvalidateSlot(buffer, 2u);
    expectTrue(!outOfRange.canInvalidate(), "invalidate-slot preflight rejects out-of-range slot");
    expectTrue(outOfRange.outOfRangeSlot, "invalidate-slot preflight marks out-of-range slot");

    expectTrue(buffer.slotIsValid(0u), "invalidateSlot ignores out-of-range slot via preflight gate");

    buffer.setMaxCapacity(4u);

        fuse::physics::broadphase::preflightPairBufferSlotReservation(buffer, 2u);
    expectTrue(validReservation.canReserve(), "slot reservation preflight accepts valid count");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferSlotReservation(buffer, 2u),

    expectTrue(!zeroSlots.canReserve(), "slot reservation preflight rejects zero slots");
    expectTrue(zeroSlots.zeroSlots, "slot reservation preflight marks zero slots");
               "canSkipPairBufferSlotReservation true for zero slots");

        fuse::physics::broadphase::preflightPairBufferSlotReservation(buffer, 8u);
    expectTrue(!exceedsCapacity.canReserve(), "slot reservation preflight rejects over-capacity count");
    expectTrue(exceedsCapacity.exceedsCapacity, "slot reservation preflight marks exceedsCapacity");

        {0, 0, 0},
        {127, 0, 0},
                 fuse::physics::broadphase::cellSpanRejectReason(wideRange, 64u)),
             "wide range reports ExceedsMaxSpan reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellSpanClamp(wideRange, 64u),
    expectTrue(!fuse::physics::broadphase::canSkipCellSpanClamp(wideRange, 64u),
               "canSkipCellSpanClamp false for over-span range");

    const fuse::physics::broadphase::CellSpanPreflight spanPreflight =
        fuse::physics::broadphase::preflightCellSpan(wideRange, 64u);
    expectTrue(!spanPreflight.canClamp(), "span preflight rejects over-span range");
    expectTrue(spanPreflight.exceedsMaxSpan, "span preflight marks exceedsMaxSpan");
    expectEq(spanPreflight.spanPerAxis.x, 128, "span preflight reports per-axis span");

        fuse::physics::broadphase::clampCellRange3(wideRange, 64u);
    expectTrue(fuse::physics::broadphase::cellSpanPerAxis(clamped).x <= 65,
               "clampCellRange3 shrinks over-span range toward center");

    const fuse::physics::broadphase::CellRange3 validRange = {
        {31, 0, 0},
        fuse::physics::broadphase::preflightCellSpan(validRange, 64u);
    expectTrue(validPreflight.canClamp(), "span preflight accepts in-span range");
    expectTrue(fuse::physics::broadphase::canSkipCellSpanClamp(validRange, 64u),
               "canSkipCellSpanClamp true for in-span range");

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),

             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "far out-of-range slot reports OutOfRangeSlot invalidate reject reason");

    const fuse::physics::broadphase::PairBufferInvalidateSlotPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferInvalidateSlot(buffer, 1u);
    expectTrue(!preflight.canInvalidate(), "invalidate preflight rejects empty slot");

             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::EmptyCell),
             "empty cell reports EmptyCell pair-gen reject reason");
               "canSkipCellPairGeneration true for empty cell");
    expectEq(fuse::physics::broadphase::estimateCellPairCount(emptyOccupants), 0u,
             "estimateCellPairCount returns zero for empty cell");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(singleOccupant)),
             "single occupant reports SingleOccupant pair-gen reject reason");
    expectTrue(fuse::physics::broadphase::cellPairGenRejectsForReason(
                   singleOccupant, fuse::physics::broadphase::CellPairGenRejectReason::SingleOccupant),
               "single occupant rejects for SingleOccupant");

    const std::vector<fuse::u32> duplicateOccupants = {1u, 1u, 1u};
    expectEq(fuse::physics::broadphase::countUniqueBodiesInCell(duplicateOccupants), 1u,
             "countUniqueBodiesInCell deduplicates occupants");
    expectTrue(fuse::physics::broadphase::canSkipCellPairGeneration(duplicateOccupants),
               "canSkipCellPairGeneration true for duplicate single-body cell");

    const std::vector<fuse::u32> multiOccupants = {0u, 1u, 1u, 2u};
    const fuse::physics::broadphase::CellPairGenPreflight preflight =
        fuse::physics::broadphase::preflightCellPairGeneration(multiOccupants);
    expectTrue(preflight.canGenerate(), "cell-pair preflight accepts multi-body cell");
    expectEq(preflight.uniqueBodyCount, 3u, "cell-pair preflight counts unique bodies");
    expectEq(preflight.pairCount, 3u, "cell-pair preflight estimates canonical pair count");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGeneration(multiOccupants),
               "shouldRunCellPairGeneration true for multi-body cell");

void testCellShapeInsertPreflightGuards() {

                 fuse::physics::broadphase::cellShapeInsertRejectReason(99u, 4u, validRange, 8u)),
                 fuse::physics::broadphase::CellShapeInsertRejectReason::OutOfRangeBody),
             "out-of-range body reports OutOfRangeBody insert reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellShapeInsert(99u, 4u, validRange, 8u),
               "canSkipCellShapeInsert true for out-of-range body");

    const fuse::physics::broadphase::CellRange3 emptyRange = {{1, 0, 0}, {0, 0, 0}};
                 fuse::physics::broadphase::cellShapeInsertRejectReason(0u, 4u, emptyRange, 8u)),
                 fuse::physics::broadphase::CellShapeInsertRejectReason::EmptyOccupancyRange),
             "empty range reports EmptyOccupancyRange insert reject reason");

                 fuse::physics::broadphase::cellShapeInsertRejectReason(0u, 4u, validRange, 7u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellShapeInsertRejectReason::ExceedsBudget),
             "over-budget range reports ExceedsBudget insert reject reason");
    const fuse::physics::broadphase::CellShapeInsertPreflight preflight =
        fuse::physics::broadphase::preflightCellShapeInsert(0u, 4u, validRange, 8u);
    expectTrue(preflight.canInsert(), "shape insert preflight accepts valid range within budget");
    expectTrue(fuse::physics::broadphase::shouldRunCellShapeInsert(0u, 4u, validRange, 8u),
               "shouldRunCellShapeInsert true for valid insert");

    expectTrue(fuse::physics::broadphase::cellShapeInsertRejectsForReason(
                   0u, 4u, planeRange, 4u,
                   fuse::physics::broadphase::CellShapeInsertRejectReason::ExceedsBudget),
               "2D insert rejects for ExceedsBudget over budget");
    expectTrue(alreadyInvalid.alreadyInvalid, "invalidate-slot preflight marks already-invalid slot");
    expectTrue(!alreadyInvalid.canInvalidate(), "invalidate-slot preflight rejects already-invalid slot");
                   buffer, 0u, fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
               "invalidate-slot rejects for AlreadyInvalid reason");

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 8u)),
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfSlot),
             "out-of-range slot reports OutOfSlot invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 8u),
               "canSkipPairBufferInvalidateSlot true for out-of-range slot");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::InsufficientOccupants),
             "single occupant reports InsufficientOccupants cell-pair reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellPairGen(0u),
               "canSkipCellPairGen true for zero occupants");

    const fuse::physics::broadphase::CellPairGenPreflight twoBodyPreflight =
        fuse::physics::broadphase::preflightCellPairGen(2u);
    expectTrue(twoBodyPreflight.canGenerate(), "two unique bodies can generate cell pairs");
    expectEq(twoBodyPreflight.pairCount, 1u, "two-body cell reports one pair");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGen(2u),
               "shouldRunCellPairGen true for two occupants");

    const std::vector<fuse::u32> duplicateOccupants = {0u, 0u, 1u};
    const fuse::physics::broadphase::CellPairGenPreflight duplicatePreflight =
        fuse::physics::broadphase::preflightCellPairGen(duplicateOccupants);
    expectTrue(duplicatePreflight.canGenerate(), "duplicate occupants dedupe to one pair");
    expectEq(duplicatePreflight.uniqueBodyCount, 2u, "duplicate occupants report two unique bodies");
    expectEq(fuse::physics::broadphase::estimateCellPairCount(duplicateOccupants),
             1u,
             "estimateCellPairCount dedupes duplicate occupants");

                   singleOccupant, fuse::physics::broadphase::CellPairGenRejectReason::InsufficientOccupants),
               "single occupant vector rejects for InsufficientOccupants");

void testCellCapacityInsertPreflightGuards() {

    const fuse::physics::broadphase::CellCapacityInsertPreflight withinBudget =
        fuse::physics::broadphase::preflightCellCapacityInsert(validRange, 8u, 0u, 4u);
    expectTrue(withinBudget.canInsert(), "cell-capacity insert preflight accepts valid range");
    expectTrue(fuse::physics::broadphase::shouldRunCellCapacityInsert(validRange, 8u, 0u, 4u),
               "shouldRunCellCapacityInsert true within budget");

    const fuse::physics::broadphase::CellCapacityInsertPreflight outOfRangeBody =
        fuse::physics::broadphase::preflightCellCapacityInsert(validRange, 8u, 5u, 4u);
    expectTrue(outOfRangeBody.outOfRangeBody, "cell-capacity insert preflight marks out-of-range body");
    expectTrue(!outOfRangeBody.canInsert(), "cell-capacity insert preflight rejects out-of-range body");
    expectTrue(fuse::physics::broadphase::cellCapacityInsertRejectsForReason(
                   validRange, 8u, 5u, 4u, fuse::physics::broadphase::CellCapacityInsertRejectReason::OutOfRangeBody),
               "cell-capacity insert rejects for OutOfRangeBody reason");

    const fuse::physics::broadphase::CellCapacityInsertPreflight overBudget =
        fuse::physics::broadphase::preflightCellCapacityInsert(validRange, 7u, 0u, 4u);
    expectTrue(overBudget.exceedsBudget, "cell-capacity insert preflight marks exceedsBudget");
    expectTrue(fuse::physics::broadphase::canSkipCellCapacityInsert(validRange, 7u, 0u, 4u),
               "canSkipCellCapacityInsert true when range exceeds budget");

                 fuse::physics::broadphase::cellCapacityInsertRejectReason(inverted, 8u, 0u, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityInsertRejectReason::EmptyRange),
             "empty range reports EmptyRange cell-capacity insert reject reason");

    const fuse::physics::broadphase::CellCapacityInsertPreflight planePreflight =
        fuse::physics::broadphase::preflightCellCapacityInsert(planeRange, 4u, 1u, 2u);
    expectTrue(!planePreflight.canInsert(), "2D cell-capacity insert preflight rejects over-budget range");
             "invalidated slot reports AlreadyInvalid invalidate reject reason");
               "invalidate rejects for AlreadyInvalid");


    const std::vector<fuse::u32> singleOccupant = {0u};

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(duplicateOccupants)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellPairGenRejectReason::None),
             "duplicate occupants with two unique bodies report None cell-pair reject reason");

        fuse::physics::broadphase::preflightCellPairGeneration(duplicateOccupants);
    expectTrue(preflight.canGenerate(), "cell-pair preflight accepts two unique bodies");
    expectEq(preflight.uniqueBodyCount, 2u, "cell-pair preflight counts unique bodies");
    expectEq(preflight.pairSlotCount, 1u, "cell-pair preflight counts canonical pair slots");
    expectEq(fuse::physics::broadphase::countCellPairSlots(duplicateOccupants), 1u,
             "countCellPairSlots matches preflight pair slot count");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGeneration(duplicateOccupants),
               "shouldRunCellPairGeneration true for two unique bodies");

                 fuse::physics::broadphase::cellCapacityInsertRejectReason(0u, 2u, validRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityInsertRejectReason::None),
             "valid insert reports None cell-capacity insert reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunCellCapacityInsert(0u, 2u, validRange, 8u),

                 fuse::physics::broadphase::cellCapacityInsertRejectReason(2u, 2u, validRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityInsertRejectReason::OutOfRangeBody),
             "out-of-range body reports OutOfRangeBody cell-capacity insert reject reason");
                   2u, 2u, validRange, 8u,
                   fuse::physics::broadphase::CellCapacityInsertRejectReason::OutOfRangeBody),
               "cell-capacity insert rejects for OutOfRangeBody");

                 fuse::physics::broadphase::cellCapacityInsertRejectReason(0u, 2u, inverted, 8u)),
             "inverted range reports EmptyRange cell-capacity insert reject reason");

                 fuse::physics::broadphase::cellCapacityInsertRejectReason(0u, 2u, validRange, 7u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityInsertRejectReason::ExceedsBudget),
             "over-budget range reports ExceedsBudget cell-capacity insert reject reason");
    expectTrue(fuse::physics::broadphase::canSkipCellCapacityInsert(0u, 2u, validRange, 7u),
               "canSkipCellCapacityInsert true over budget");

        fuse::physics::broadphase::preflightCellCapacityInsert(0u, 2u, planeRange, 4u);
    expectTrue(!planePreflight.canInsert(), "2D cell-capacity insert preflight rejects over budget");
    expectTrue(planePreflight.exceedsBudget, "2D cell-capacity insert preflight marks exceedsBudget");
    expectTrue(std::strcmp(fuse::physics::broadphase::cellCapacityInsertRejectReasonName(
                               fuse::physics::broadphase::CellCapacityInsertRejectReason::ExceedsBudget),
                           "ExceedsBudget") == 0,
               "ExceedsBudget cell-capacity insert reject reason has stable label");
               "canSkipCellPairGen true for empty occupants");
             "estimateCellPairCount returns zero for empty occupants");

    const std::vector<fuse::u32> singletonOccupants = {3u, 3u, 3u};
    const fuse::physics::broadphase::CellPairGenPreflight singletonPreflight =
        fuse::physics::broadphase::preflightCellPairGen(singletonOccupants);
    expectTrue(!singletonPreflight.canGenerate(), "cell-pair preflight rejects singleton occupants");
    expectTrue(singletonPreflight.insufficientOccupants, "cell-pair preflight marks insufficient occupants");
    expectEq(singletonPreflight.uniqueOccupantCount, 1u, "cell-pair preflight counts unique occupants");

    const std::vector<fuse::u32> pairOccupants = {0u, 1u, 0u};
    expectTrue(pairPreflight.canGenerate(), "cell-pair preflight accepts multi-body cell");
               "shouldRunCellPairGen true for pairable occupants");
    expectEq(pairPreflight.estimatedPairCount, 1u, "cell-pair preflight estimates one canonical pair");
    expectEq(fuse::physics::broadphase::estimateCellPairCount(pairOccupants), 1u,
             "estimateCellPairCount matches preflight estimate");

void testRefineMergePreflightCountGuards() {
             "cleared slot reports AlreadyInvalid invalidate reject reason");

    fuse::physics::broadphase::PairBufferSoA rangeBuffer;
    rangeBuffer.preparePairSlots(2u);
    rangeBuffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(rangeBuffer, 2u)),
             "boundary slot reports OutOfRangeSlot invalidate reject reason");
    rangeBuffer.invalidateSlot(2u);
    expectTrue(rangeBuffer.slotIsValid(1u), "invalidateSlot ignores out-of-range slot via preflight gate");

void testShapeCellInsertRejectReasonGuards() {


    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::shapeCellInsertRejectReason(
                 0u, bodies, shapes, params)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::OutOfRangeBody),
             "missing shape reports OutOfRangeBody insert reject reason");

    bodies.addBody({500.f, 0.f, 0.f}, 1.f);

             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::ExceedsOccupancy),
             "huge sphere reports ExceedsOccupancy insert reject reason");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsert(0u, bodies, shapes, params),
               "canSkipShapeCellInsert true for over-budget shape");
    expectTrue(std::strcmp(fuse::physics::broadphase::shapeCellInsertRejectReasonName(
                               fuse::physics::broadphase::ShapeCellInsertRejectReason::ExceedsOccupancy),
                           "ExceedsOccupancy") == 0,
               "ExceedsOccupancy shape insert reject reason has stable label");

    params.maxCellOccupancy = 64u;
    const fuse::physics::broadphase::ShapeCellInsertPreflight smallPreflight =
        fuse::physics::broadphase::preflightShapeCellInsert(1u, bodies, shapes, params);
    expectTrue(smallPreflight.canInsert(), "small sphere insert preflight can insert");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsert(1u, bodies, shapes, params),
               "shouldRunShapeCellInsert true for small sphere");

void testRefinePairSlotRejectReasonGuards() {


    expectTrue(mergePreflight.canMerge(), "merge preflight accepts plane plus dynamic scene");
    expectEq(mergePreflight.planeBodyCount, 1u, "merge preflight counts plane bodies");
    expectEq(mergePreflight.dynamicBodyCount, 1u, "merge preflight counts dynamic bodies");
    expectEq(mergePreflight.estimatedMergePairs, 1u, "merge preflight estimates merge pair count");

    expectEq(buffer.compact(), 1u, "compact gathers valid slot before refine preflight");

    const fuse::physics::broadphase::RefineBroadphasePreflight refinePreflight =
    expectTrue(refinePreflight.canRefine(), "refine preflight accepts buffer with valid pairs");
    expectEq(refinePreflight.validPairCount, 1u, "refine preflight reports valid pair count");

    expectTrue(!dedupePreflight.canDedupe(), "dedupe preflight skips single-pair buffer");
    expectEq(dedupePreflight.pairCount, 1u, "dedupe preflight reports active pair count");
               "canSkipCellPairGen on empty cell");
    expectEq(fuse::physics::broadphase::countPairsForCell(emptyOccupants), 0u,
             "countPairsForCell returns zero for empty cell");

             "single occupant reports InsufficientOccupants pair-gen reject reason");

    const std::vector<fuse::u32> duplicateBodies = {0u, 0u, 0u};
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellPairGenRejectReason(duplicateBodies)),
             "duplicate-only occupants report InsufficientOccupants pair-gen reject reason");

    const std::vector<fuse::u32> validOccupants = {0u, 1u, 1u};
        fuse::physics::broadphase::preflightCellPairGen(validOccupants);
    expectTrue(preflight.canGenerate(), "pair-gen preflight accepts multiple unique bodies");
    expectEq(preflight.pairCount, 1u, "pair-gen preflight reports pair count");
    expectTrue(fuse::physics::broadphase::shouldRunCellPairGen(validOccupants),
               "shouldRunCellPairGen true for valid occupants");
    expectEq(fuse::physics::broadphase::countPairsForCell(validOccupants), 1u,
             "countPairsForCell returns one pair for two unique bodies");
                               fuse::physics::broadphase::CellPairGenRejectReason::InsufficientOccupants),
                           "InsufficientOccupants") == 0,
               "InsufficientOccupants pair-gen reject reason has stable label");


                 fuse::physics::broadphase::shapeCellInsertRejectReason(0u, 2u, validRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::None),
             "valid body and range report None shape-insert reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsert(0u, 2u, validRange, 8u),
               "shouldRunShapeCellInsert true for valid insert");

                 fuse::physics::broadphase::shapeCellInsertRejectReason(2u, 2u, validRange, 8u)),
             "out-of-range body reports OutOfRangeBody shape-insert reject reason");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsert(2u, 2u, validRange, 8u),
               "canSkipShapeCellInsert true for out-of-range body");

                 fuse::physics::broadphase::shapeCellInsertRejectReason(0u, 2u, validRange, 7u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::ExceedsOccupancyBudget),
             "over-budget range reports ExceedsOccupancyBudget shape-insert reject reason");

    const fuse::physics::broadphase::ShapeCellInsertPreflight planePreflight =
        fuse::physics::broadphase::preflightShapeCellInsert(0u, 2u, planeRange, 4u);
    expectTrue(!planePreflight.canInsert(), "2D shape-insert preflight rejects over-budget range");
    expectTrue(planePreflight.exceedsOccupancyBudget, "2D shape-insert preflight marks exceedsOccupancyBudget");
                               fuse::physics::broadphase::ShapeCellInsertRejectReason::ExceedsOccupancyBudget),
                           "ExceedsOccupancyBudget") == 0,
               "ExceedsOccupancyBudget shape-insert reject reason has stable label");
    const std::vector<fuse::u32> duplicateSingle = {3u, 3u, 3u};
                 fuse::physics::broadphase::cellPairGenRejectReason(duplicateSingle)),
             "duplicate occupants dedupe to single occupant");

    const std::vector<fuse::u32> multiOccupants = {0u, 1u, 0u, 2u};
    expectTrue(preflight.canGenerate(), "multi-occupant cell preflight can generate pairs");
    expectEq(preflight.uniqueOccupantCount, 3u, "cell preflight counts unique occupants");
    expectEq(preflight.pairCount, 3u, "cell preflight estimates pair count");
               "shouldRunCellPairGeneration true for multi-occupant cell");
    expectEq(fuse::physics::broadphase::estimateCellPairCount(multiOccupants), 3u,
             "estimateCellPairCount matches n*(n-1)/2 for unique occupants");

void testCellCapacityInsertRejectReasonGuards() {

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellCapacityInsertRejectReason(
                 2u, 2u, validRange, 8u)),
               "out-of-range body rejects for OutOfRangeBody");

                 0u, 2u, validRange, 7u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellCapacityInsertRejectReason::ExceedsOccupancy),
             "over-budget range reports ExceedsOccupancy insert reject reason");
                               fuse::physics::broadphase::CellCapacityInsertRejectReason::ExceedsOccupancy),
               "ExceedsOccupancy insert reject reason has stable label");

    const fuse::physics::broadphase::CellCapacityInsertPreflight preflight =
        fuse::physics::broadphase::preflightCellCapacityInsert(0u, 2u, validRange, 8u);
    expectTrue(preflight.canInsert(), "insert preflight accepts valid body and range");

    expectTrue(fuse::physics::broadphase::canSkipCellCapacityInsert(0u, 2u, planeRange, 4u),
               "2D canSkipCellCapacityInsert true over budget");

void testPairBufferCanSkipCompactAndClamp() {
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer can skip compact-and-clamp");
               "canSkipPairBufferCompactAndClamp mirrors member on empty buffer");

    expectTrue(buffer.canSkipCompactAndClamp(), "synced within-capacity buffer can skip compact-and-clamp");

    expectTrue(!buffer.canSkipCompactAndClamp(),
               "prepared slots with stale activeCount cannot skip compact-and-clamp");

void testPairBufferCanSkipCompactAndClampGuard() {
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer canSkipCompactAndClamp");
               "canSkipPairBufferCompactAndClamp mirrors SoA helper on empty buffer");

    expectTrue(buffer.canSkipCompactAndClamp(),
               "finalized within-capacity buffer canSkipCompactAndClamp");


    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::refinePairSlotRejectReason(
                 0u, bodies, shapes, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairSlotRejectReason::None),
             "overlapping pair reports None refine slot reject reason");
    expectTrue(fuse::physics::broadphase::preflightRefinePairSlot(0u, bodies, shapes, buffer).passesRefine(),
               "overlapping pair refine slot preflight passes refine");

                 1u, bodies, shapes, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairSlotRejectReason::Separated),
             "separated pair reports Separated refine slot reject reason");
    expectTrue(fuse::physics::broadphase::preflightRefinePairSlot(1u, bodies, shapes, buffer).shouldInvalidate(),
               "separated pair refine slot preflight should invalidate");
    expectTrue(std::strcmp(fuse::physics::broadphase::refinePairSlotRejectReasonName(
                               fuse::physics::broadphase::RefinePairSlotRejectReason::Separated),
                           "Separated") == 0,
               "Separated refine slot reject reason has stable label");

                 4u, bodies, shapes, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairSlotRejectReason::OutOfRangeSlot),
             "out-of-range pair index reports OutOfRangeSlot refine reject reason");

void testMergePairsIntoBufferExtendedPreflightGuards() {
    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}, {2u, 3u}, {4u, 5u}};

    const fuse::physics::broadphase::MergePairsIntoBufferPreflight truncatePreflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, buffer);
    expectTrue(truncatePreflight.canMerge(), "merge preflight still allows merge when truncate is diagnostic only");
    expectTrue(truncatePreflight.wouldTruncate,
               "merge preflight marks wouldTruncate when pairs exceed remaining capacity");
    expectTrue(fuse::physics::broadphase::mergePairsIntoBufferWouldTruncate(pairs, buffer),
               "mergePairsIntoBufferWouldTruncate true for excess pair count");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {{0u, 1u}, {2u, 2u}, {3u, 4u}};
    expectEq(fuse::physics::broadphase::countInvalidMergePairs(mixedPairs), 1u,
             "countInvalidMergePairs counts self-pair in merge list");
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight mixedPreflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(mixedPairs, buffer);
    expectEq(mixedPreflight.invalidPairCount, 1u, "merge preflight reports invalid pair count");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 4u)),
             "already-invalid slot reports AlreadyInvalid invalidate reject reason");
                   buffer,
                   0u,
                   fuse::physics::broadphase::PairBufferInvalidateRejectReason::AlreadyInvalid),
               "canSkipPairBufferInvalidate true for already-invalid slot");

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 8u)),
             "out-of-range slot reports OutOfRangeSlot invalidate reject reason");
void testPairBufferWriteInvalidatePreflightGuards() {
             "unprepared buffer reports OutOfRangeSlot write reject reason");


    expectTrue(validWrite.canWrite(), "write preflight accepts valid in-range slot");

                 fuse::physics::broadphase::pairBufferInvalidateRejectReason(buffer, 99u)),
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidate(buffer, 99u),
               "canSkipPairBufferInvalidate on out-of-range slot");

    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateRejectReasonName(
                           "AlreadyInvalid") == 0,
               "AlreadyInvalid invalidate reject reason has stable label");

    expectTrue(buffer.slotIsValid(1u), "invalidateSlot ignores out-of-range slot via preflight gate");
    expectTrue(!buffer.slotIsValid(1u), "invalidateSlot clears valid slot via preflight gate");

void testCellOccupancyForParamsGuards() {

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::cellOccupancyRejectReasonForParams(validRange, params)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "params wrapper reports None for range within budget");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellOccupancyIteration(validRange, params),
               "shouldRunShapeCellOccupancyIteration true within params budget");
    expectTrue(!fuse::physics::broadphase::canSkipShapeCellOccupancyIteration(validRange, params),
               "canSkipShapeCellOccupancyIteration false within params budget");

    params.maxCellOccupancy = 7u;
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "params wrapper reports ExceedsBudget when over budget");

    const fuse::physics::broadphase::CellOccupancyPreflight preflight =
        fuse::physics::broadphase::preflightCellOccupancyForParams(validRange, params);
    expectTrue(!preflight.canIterate(), "params preflight rejects over-budget range");
    expectTrue(preflight.exceedsBudget, "params preflight marks exceedsBudget");

    params.maxCellOccupancy = 4u;
    expectTrue(fuse::physics::broadphase::canSkipShapeCellOccupancyIteration(planeRange, params),
               "2D canSkipShapeCellOccupancyIteration true over params budget");

void testDedupeBroadphasePairBufferPreflightParity() {
               "empty buffer skips broadphase dedupe");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "empty buffer skips SoA dedupe");

               "single pair skips broadphase dedupe");
               "single pair skips SoA dedupe");

               "multiple pairs run broadphase dedupe");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferDedupe(buffer),
               "multiple pairs run SoA dedupe");
                 fuse::physics::broadphase::dedupeBroadphaseRejectReason(buffer)),
                 fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             "dedupe reject reasons match between broadphase and SoA layers");

void testMergePairsIntoBufferPreflightFields() {

    const fuse::physics::broadphase::MergePairsIntoBufferPreflight preflight =
    expectTrue(preflight.canMerge(), "merge preflight accepts non-empty list into empty buffer");
    expectEq(preflight.pairCount, 3u, "merge preflight reports pair count");
    expectEq(preflight.remainingCapacity, UINT32_MAX, "empty buffer merge preflight has unlimited remaining capacity");

    const fuse::physics::broadphase::MergePairsIntoBufferPreflight fullPreflight =
    expectTrue(!fullPreflight.canMerge(), "merge preflight rejects when buffer is full");
    expectTrue(fullPreflight.bufferFull, "full merge preflight marks bufferFull");
    expectEq(fullPreflight.remainingCapacity, 0u, "full buffer merge preflight reports zero remaining capacity");
    const fuse::physics::broadphase::PairBufferInvalidateSlotPreflight validPreflight =
        fuse::physics::broadphase::preflightPairBufferInvalidateSlot(buffer, 0u);
    expectTrue(validPreflight.canInvalidate(), "invalidate-slot preflight accepts valid slot");

               "canSkipPairBufferInvalidateSlot on already-invalid slot");

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 4u)),

void testCellCapacityPreflightGuards() {
    const fuse::physics::broadphase::CellCapacityPreflight withinBudget =
        fuse::physics::broadphase::preflightCellCapacity(validRange, 8u);
    expectTrue(withinBudget.canInsert(), "cell-capacity preflight accepts range within budget");
    expectEq(withinBudget.budgetRemaining, 0u, "cell-capacity preflight reports zero remaining at exact budget");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsertion(validRange, 8u),
               "shouldRunShapeCellInsertion true within budget");
    expectTrue(!fuse::physics::broadphase::canSkipShapeCellInsertion(validRange, 8u),
               "canSkipShapeCellInsertion false within budget");

    const fuse::physics::broadphase::CellCapacityPreflight overBudget =
        fuse::physics::broadphase::preflightCellCapacity(validRange, 7u);
    expectTrue(!overBudget.canInsert(), "cell-capacity preflight rejects range over budget");
    expectTrue(overBudget.occupancy.exceedsBudget, "cell-capacity preflight marks exceedsBudget");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(validRange, 7u),
               "canSkipShapeCellInsertion true over budget");

    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsertion(planeRange, 8u),
               "2D shouldRunShapeCellInsertion true within budget");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(planeRange, 4u),
               "2D canSkipShapeCellInsertion true over budget");

void testBroadphaseMergeIntoBufferPreflightGuards() {

    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight emptyScenePreflight =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, pairs, buffer);
    expectTrue(!emptyScenePreflight.canMergeIntoBuffer(), "empty scene cannot merge into buffer");
    expectTrue(emptyScenePreflight.emptyMergeScene, "merge-into-buffer preflight marks empty merge scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMergeIntoBuffer(bodies, shapes, pairs, buffer),


    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight validPreflight =
    expectTrue(validPreflight.canMergeIntoBuffer(), "mergeable scene can merge pairs into empty buffer");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMergeIntoBuffer(bodies, shapes, pairs, buffer),
               "shouldRunBroadphaseMergeIntoBuffer true for valid merge");

    buffer.setMaxCapacity(1u);
    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight fullPreflight =
    expectTrue(!fullPreflight.canMergeIntoBuffer(), "full buffer cannot merge additional pairs");
    expectTrue(fullPreflight.bufferFull, "merge-into-buffer preflight marks buffer full");
    expectEq(static_cast<fuse::u32>(fullPreflight.bufferReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
             "merge-into-buffer preflight carries buffer reject reason");
        fuse::physics::broadphase::preflightPairBufferInvalidate(buffer, 8u);
    expectTrue(!preflight.canInvalidate(), "invalidate preflight rejects out-of-range slot");
    expectTrue(preflight.outOfRangeSlot, "invalidate preflight marks out-of-range slot");
}

void testCellOccupancyBudgetRemainingPreflight() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight withinBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 12u);
    expectEq(withinBudget.budgetRemaining, 4u, "preflight reports remaining occupancy budget");
    expectEq(withinBudget.occupancyCount, 8u, "preflight occupancy count unchanged with budgetRemaining");

    const fuse::physics::broadphase::CellOccupancyPreflight overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 7u);
    expectEq(overBudget.budgetRemaining, 0u, "over-budget preflight reports zero remaining budget");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight planePreflight =
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 10u);
    expectEq(planePreflight.budgetRemaining, 2u, "2D preflight reports remaining occupancy budget");

void testRefineAndDedupeBroadphasePreflightGuards() {

                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 2u)),

    buffer.writeSlot(1u, 2u, 3u);
    expectTrue(buffer.invalidateSlotWithPreflight(1u), "invalidateSlotWithPreflight clears valid slot");
    expectTrue(!buffer.slotIsValid(1u), "invalidateSlotWithPreflight clears slot validity");
    expectTrue(!buffer.invalidateSlotWithPreflight(1u),
               "invalidateSlotWithPreflight returns false on already-invalid slot");

void testPairBufferWouldSkipWriteInvalidateGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;

    fuse::physics::broadphase::PairBufferWriteSlotRejectReason writeReason =
        fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u, &writeReason),
               "wouldSkipPairBufferWriteSlot false for valid write");
    expectEq(static_cast<fuse::u32>(writeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
             "wouldSkipPairBufferWriteSlot reports None for valid write");

    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u, &writeReason),
               "wouldSkipPairBufferWriteSlot true for self-pair");
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
             "wouldSkipPairBufferWriteSlot reports InvalidPair for self-pair");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u) ==
                   fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "wouldSkipPairBufferWriteSlot agrees with canSkipPairBufferWriteSlot");

    fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason invalidateReason =
        fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 1u, &invalidateReason),
               "wouldSkipPairBufferInvalidateSlot true for out-of-range slot");
    expectEq(static_cast<fuse::u32>(invalidateReason),
             static_cast<fuse::u32>(
             "wouldSkipPairBufferInvalidateSlot reports OutOfRangeSlot");

    buffer.writeSlot(0u, 0u, 1u);
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u, &invalidateReason),
               "wouldSkipPairBufferInvalidateSlot false for valid slot");
    buffer.invalidateSlot(0u);
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u, &invalidateReason),
               "wouldSkipPairBufferInvalidateSlot true for already-invalid slot");
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
             "wouldSkipPairBufferInvalidateSlot reports AlreadyInvalid");

void testCellCapacityWouldSkipGuards() {
    fuse::physics::broadphase::CellOccupancyRejectReason occupancyReason =
        fuse::physics::broadphase::CellOccupancyRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 8u, &occupancyReason),
               "wouldSkipCellOccupancyIteration false within budget");
    expectEq(static_cast<fuse::u32>(occupancyReason),
             "wouldSkipCellOccupancyIteration reports None within budget");
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u, &occupancyReason),
               "wouldSkipCellOccupancyIteration true over budget");
             "wouldSkipCellOccupancyIteration reports ExceedsBudget");
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u) ==
                   fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "wouldSkipCellOccupancyIteration agrees with canSkipCellOccupancyIteration");

    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(planeRange, 4u),
               "2D wouldSkipCellOccupancyIteration true over budget");

    const fuse::physics::broadphase::CellRange3 overSpanRange = {{0, 0, 0}, {5, 0, 0}};
    fuse::physics::broadphase::CellSpanRejectReason spanReason =
        fuse::physics::broadphase::CellSpanRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipCellSpanClamp(overSpanRange, 4u, &spanReason),
               "wouldSkipCellSpanClamp false when span exceeds budget");
    expectEq(static_cast<fuse::u32>(spanReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpan),
             "wouldSkipCellSpanClamp reports ExceedsSpan when clamp needed");
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(overSpanRange, 4u) ==
                   fuse::physics::broadphase::canSkipCellSpanClamp(overSpanRange, 4u),
               "wouldSkipCellSpanClamp agrees with canSkipCellSpanClamp");

    const fuse::physics::broadphase::CellRange3 withinSpanRange = {{0, 0, 0}, {3, 0, 0}};
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(withinSpanRange, 4u, &spanReason),
               "wouldSkipCellSpanClamp true when span within limit");
             "wouldSkipCellSpanClamp reports None when span within limit");
    expectTrue(fuse::physics::broadphase::wouldSkipCellSpanClamp(overSpanRange, 0u),
               "wouldSkipCellSpanClamp true for unlimited span budget");

void testRefineDedupeMergeWouldSkipGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectTrue(fuse::physics::broadphase::canSkipRefineAndDedupeBroadphase(bodies, shapes, buffer),
               "canSkipRefineAndDedupeBroadphase on empty scene");
    expectTrue(!fuse::physics::broadphase::shouldRunRefineAndDedupeBroadphase(bodies, shapes, buffer),
               "shouldRunRefineAndDedupeBroadphase false on empty scene");
    fuse::physics::broadphase::RefineBroadphaseRejectReason refineReason =
        fuse::physics::broadphase::RefineBroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer, &refineReason),
               "wouldSkipRefineBroadphase true on empty scene");
    expectTrue(refineReason == fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyBuffer ||
                   refineReason == fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyInput,
               "wouldSkipRefineBroadphase reports empty-scene reject reason");
    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer) ==
                   fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "wouldSkipRefineBroadphase agrees with canSkipRefineBroadphase");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    const fuse::physics::broadphase::RefineAndDedupeBroadphasePreflight preflight =
        fuse::physics::broadphase::preflightRefineAndDedupeBroadphase(bodies, shapes, buffer);
    expectTrue(preflight.canRefine(), "combined preflight can refine valid scene");
    expectTrue(preflight.canDedupe(), "combined preflight can dedupe duplicate pairs");
    expectTrue(preflight.canRunEither(), "combined preflight can run either pass");
    expectTrue(fuse::physics::broadphase::shouldRunRefineAndDedupeBroadphase(bodies, shapes, buffer),
               "shouldRunRefineAndDedupeBroadphase true when dedupe needed");

void testBroadphaseMergeDeepenPreflightGuards() {

    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight planeOnly =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectEq(planeOnly.planeBodyCount, 1u, "merge preflight counts plane bodies");
    expectEq(planeOnly.dynamicBodyCount, 0u, "merge preflight counts zero dynamic bodies on plane-only scene");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight mergeable =
    expectEq(mergeable.planeBodyCount, 1u, "mergeable scene reports plane body count");
    expectEq(mergeable.dynamicBodyCount, 1u, "mergeable scene reports dynamic body count");
    expectTrue(mergeable.canMerge(), "mergeable scene preflight can merge");

void testMergePairsIntoBufferDeepenPreflightGuards() {

        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, buffer);
    expectEq(preflight.mergeablePairCount, 2u, "merge preflight counts mergeable pairs");
    expectEq(preflight.remainingCapacity, 1u, "merge preflight reports remaining capacity");
    expectTrue(preflight.canMerge(), "merge preflight can merge into non-full buffer");
    expectTrue(preflight.hasPartialCapacity(), "merge preflight marks partial capacity when pairs exceed remaining slots");

    expectTrue(fullPreflight.bufferFull, "full buffer merge preflight marks buffer full");
    expectEq(fullPreflight.mergeablePairCount, 0u, "full buffer merge preflight counts zero mergeable pairs");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferInvalidate(buffer, 0u),
               "shouldRunPairBufferInvalidate false on already-invalid slot");

void testCellSpanCapacityPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 wideRange = {
        {-10, -10, -10},
        {10, 10, 10},
    };
    expectTrue(fuse::physics::broadphase::exceedsCellSpanLimit(wideRange, 8u),
               "wide range exceeds cell span limit");
    expectTrue(fuse::physics::broadphase::cellSpanWithinLimit(wideRange, 0u),
               "zero span limit is unbounded stub");

    const fuse::physics::broadphase::CellSpanPreflight overSpan =
        fuse::physics::broadphase::preflightCellSpan(wideRange, 8u);
    expectTrue(!overSpan.withinLimit(), "cell span preflight rejects over-limit range");
    expectTrue(overSpan.exceedsSpanLimit, "cell span preflight marks exceedsSpanLimit");
    expectEq(static_cast<fuse::u32>(overSpan.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanLimit),
             "cell span preflight carries reject reason");

    const fuse::physics::broadphase::CellRange3 smallRange = {
        {0, 0, 0},
        {3, 3, 3},
    const fuse::physics::broadphase::CellSpanPreflight withinSpan =
        fuse::physics::broadphase::preflightCellSpan(smallRange, 8u);
    expectTrue(withinSpan.withinLimit(), "small range passes cell span preflight");
    expectTrue(!fuse::physics::broadphase::canSkipCellSpanCheck(smallRange, 8u),
               "canSkipCellSpanCheck false when range is within limit");

    const fuse::physics::broadphase::CellRange2 planeRange = {
        {0, 0},
        {2, 1},
    const fuse::physics::broadphase::CellSpanPreflight planePreflight =
        fuse::physics::broadphase::preflightCellSpan2D(planeRange, 4u);
    expectTrue(planePreflight.withinLimit(), "2D cell span preflight accepts within-limit range");
    expectEq(planePreflight.spanPerAxis.x, 3, "2D cell span preflight reports x span");

void testRefinePairAndDedupeDeepenGuards() {
    bodies.addBody({10.f, 0.f, 0.f}, 1.f);

                 fuse::physics::broadphase::refinePairRejectReason(0u, 1u, bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefinePairRejectReason::Separated),
             "distant pair reports Separated refine-pair reject reason");
    expectTrue(fuse::physics::broadphase::shouldInvalidatePairDuringRefine(0u, 1u, bodies, shapes),
               "shouldInvalidatePairDuringRefine true for separated pair");

    bodies.positions[1] = {1.5f, 0.f, 0.f};
    const fuse::physics::broadphase::RefinePairPreflight overlappingPreflight =
        fuse::physics::broadphase::preflightRefinePair(0u, 1u, bodies, shapes);
    expectTrue(overlappingPreflight.passesRefine(), "refine-pair preflight accepts overlapping pair");
    expectTrue(std::strcmp(fuse::physics::broadphase::refinePairRejectReasonName(
                               fuse::physics::broadphase::RefinePairRejectReason::Separated),
                           "Separated") == 0,
               "Separated refine-pair reject reason has stable label");

    expectTrue(!fuse::physics::broadphase::dedupeBroadphaseWouldChange(buffer),
               "dedupeWouldChange false on empty buffer");

    expectTrue(fuse::physics::broadphase::dedupeBroadphaseWouldChange(buffer),
               "dedupeWouldChange true when duplicate pairs exist");

void testMergePairIntoBufferPreflightGuards() {
    const fuse::physics::broadphase::CandidatePair validPair = {0u, 1u};
    const fuse::physics::broadphase::CandidatePair invalidPair = {2u, 2u};

    expectTrue(fuse::physics::broadphase::shouldRunMergePairIntoBuffer(validPair, buffer),
               "shouldRunMergePairIntoBuffer true for valid pair into empty buffer");
                 fuse::physics::broadphase::mergePairIntoBufferRejectReason(invalidPair, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairIntoBufferRejectReason::InvalidPair),
             "self-pair reports InvalidPair merge-pair reject reason");

    expectTrue(!fuse::physics::broadphase::shouldRunMergePairIntoBuffer(validPair, buffer),
               "shouldRunMergePairIntoBuffer false when buffer is full");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}, {2u, 3u}, {4u, 5u}};
    fuse::physics::broadphase::PairBufferSoA partialBuffer;
    partialBuffer.setMaxCapacity(2u);
    partialBuffer.push(0u, 1u);
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight batchPreflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, partialBuffer);
    expectEq(batchPreflight.mergeablePairCount, 1u, "batch merge preflight counts mergeable pairs");
    expectTrue(batchPreflight.partialMergeOnly, "batch merge preflight marks partial merge");
    expectEq(fuse::physics::broadphase::countMergeablePairsIntoBuffer(pairs, partialBuffer),
             1u,
             "countMergeablePairsIntoBuffer matches batch preflight");
    expectTrue(!fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer, &refineReason),
               "wouldSkipRefineBroadphase false for valid scene");
    expectEq(static_cast<fuse::u32>(refineReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::None),
             "wouldSkipRefineBroadphase reports None for valid scene");

    fuse::physics::broadphase::DedupeBroadphaseRejectReason dedupeReason =
        fuse::physics::broadphase::DedupeBroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer, &dedupeReason),
               "wouldSkipDedupeBroadphase true on single pair");
    expectEq(static_cast<fuse::u32>(dedupeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::SinglePair),
             "wouldSkipDedupeBroadphase reports SinglePair");
    buffer.push(2u, 3u);
    expectTrue(!fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer, &dedupeReason),
               "wouldSkipDedupeBroadphase false for multiple pairs");

    fuse::physics::broadphase::BroadphaseMergeRejectReason mergeReason =
        fuse::physics::broadphase::BroadphaseMergeRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphaseMerge(bodies, shapes, &mergeReason),
               "wouldSkipBroadphaseMerge true without plane bodies");
    expectEq(static_cast<fuse::u32>(mergeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "wouldSkipBroadphaseMerge reports EmptyPlaneBodies");

    fuse::physics::broadphase::PairBufferSoA mergeBuffer;
    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{0u, 1u}};
    fuse::physics::broadphase::MergePairsIntoBufferRejectReason mergeIntoReason =
        fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, mergeBuffer, &mergeIntoReason),
               "wouldSkipMergePairsIntoBuffer false for valid merge");
    mergeBuffer.setMaxCapacity(1u);
    mergeBuffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::wouldSkipMergePairsIntoBuffer(pairs, mergeBuffer, &mergeIntoReason),
               "wouldSkipMergePairsIntoBuffer true when buffer is full");
    expectEq(static_cast<fuse::u32>(mergeIntoReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
             "wouldSkipMergePairsIntoBuffer reports BufferFull");

    fuse::physics::broadphase::BroadphaseRejectReason broadphaseReason =
        fuse::physics::broadphase::BroadphaseRejectReason::None;
    fuse::physics::RigidBodySoA emptyBodies;
    fuse::physics::CollisionShapeSoA emptyShapes;
    expectTrue(fuse::physics::broadphase::wouldSkipBroadphase(emptyBodies, emptyShapes, &broadphaseReason),
               "wouldSkipBroadphase true on empty scene");
    expectEq(static_cast<fuse::u32>(broadphaseReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::EmptyInput),
             "wouldSkipBroadphase reports EmptyInput");
    expectTrue(!fuse::physics::broadphase::wouldSkipBroadphase(bodies, shapes, &broadphaseReason),
               "wouldSkipBroadphase false on populated scene");
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
    testPairBufferCompactionClampRejectReasonGuards();
    testPairBufferSkipPredicateGuards();
    testCellOccupancyPreflightReasonField();
    testShouldRunRefineBroadphaseGuards();
    testCellSpanClampPreflightGuards();
    testPairBufferWriteSlotAndPreparePreflights();
    testPairBufferMergeAndDuplicateGuards();
    testRefineDedupeBroadphasePreflightGuards();
    testBroadphaseMergeRejectReasonGuards();
    testBroadphaseMergePreflightGuards();
    testCellOccupancyPreflightReasonField();
    testBroadphaseMergeRejectReasonGuards();
    testRefineBroadphaseShouldRunGuard();
    testPairBufferPushRejectReasonGuards();
    testPairBufferCompactionRejectReasonGuards();
    testPairBufferClampRejectReasonGuards();
    testPairBufferDedupeRejectReasonGuards();
    testCellOccupancyIterationSkipGuards();
    testRefineBroadphaseShouldRunGuards();
    testBroadphaseMergeRejectReasonGuards();
    testPairBufferWriteSlotPreflightGuards();
    testPairBufferAcceptPreflightGuards();
    testCellRangeSpanClampPreflightGuards();
    testShouldRunBroadphasePairGenerationGuards();
    testBroadphaseCellPairBuildPreflightGuards();
    testPairBufferSortRejectReasonGuards();
    testPairBufferCompactAndClampPreflightGuards();
    testShouldRunBroadphaseGuards();
    testMergePairsIntoBufferPreflightGuards();
    testPairBufferShouldRunDedupeGuards();
    testPairBufferCanSkipCompactAndClamp();
    testPairBufferPushShouldRunGuards();
    testPairBufferPushSkipGuards();
    testPairBufferWriteSlotRejectReasonGuards();
    testPairBufferInvalidateSlotRejectReasonGuards();
    testCellPairGenRejectReasonGuards();
    testShapeCellInsertRejectReasonGuards();
    testBroadphaseCellPairGenRejectReasonGuards();
    testPairBufferToVectorRejectReasonGuards();
    testCellSpanRejectReasonAndPreflight();
    testShapeCellCapacityRejectReasonAndPreflight();
    testBroadphaseMergePairsIntoBufferPreflightGuards();
    testCellCapacityPreflightAndWouldSkipGuards();
    testRefineDedupeMergeWouldSkipGuards();
    testPairBufferWouldSkipWriteSlotGuards();
    testWouldSkipCellCapacityGuards();
    testWouldSkipRefineDedupeMergeGuards();
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
    testShouldRunRefineBroadphaseGuards();
    testPairBufferRejectReasonGuards();
    testCellOccupancyCanSkipIterationGuards();
    testMergeBroadphaseRejectReasonGuards();
    testPairBufferSortAndCompactionSkipGuards();
    testPairBufferSortAndDedupeSkipGuards();
    testPairBufferWriteSlotPreflightGuards();
    testPairBufferMergePreflightGuards();
    testPairBufferPushBodyCountPreflightGuards();
    testRefineBroadphaseEmptyInputRejectReason();
    testRefineBroadphaseNoValidPairsRejectReason();
    testBroadphasePlaneDynamicMergeIntegration();
    testBroadphaseDedupeRemovesDuplicates();
    testPairBufferCompactionClampRejectReasonGuards();
    testRefineAndMergeRejectReasonGuards();
    testCanSkipCellOccupancyIterationGuards();
    testPairBufferRejectReasonAndSkipGuards();
    testPairBufferSkipHelperGuards();
    testPairBufferDedupeShouldRunGuards();
    testBroadphaseShouldRunGuards();
    testCellSpanPreflightGuards();
    testBroadphaseMergeBufferPreflightGuards();
    testShouldRunPairBufferDedupeGuards();
    testBroadphaseMergePreflightHasBodiesFields();
    testPairBufferPushSkipGuards();
    testRefineDedupeBroadphasePreflightGuards();
    testBroadphaseMergeLaunchPreflightGuards();
    testBroadphaseMergeRejectReasonGuards();
    testShouldRunPairBufferDedupeAndSortGuards();
    testPairSlotPreflightGuards();
    testPairBufferSlotWritePreflightGuards();
    testPairBufferSlotReservationPreflightGuards();
    testPairBufferSortSkipGuards();
    testRefineDedupeMergePreflightCounts();
    testPairBufferSortRejectReasonGuards();
    testPairBufferCompactAndClampPreflightGuards();
    testRefinableBroadphasePairCountGuards();
    testPairBufferShouldRunDedupeAndSortGuards();
    testCellOccupancyPreflightBudgetRemaining();
    testRefineBroadphasePreflightValidPairCount();
    testBroadphaseMergePreflightBodyCounts();
    testPairBufferCompactAndClampRejectReasonGuards();
    testMergePairsIntoBufferPreflightGuards();
    testMergePairsIntoBufferRejectReasonGuards();
    testPairBufferSortAndDedupeShouldRunGuards();
    testShouldRunBroadphaseGuard();
    testShouldRunBroadphaseGuards();
    testBroadphaseCellPairPreflightGuards();
    testPerShapeCellBudgetGuards();
    testPairBufferShouldRunDedupeGuards();
    testDedupeBroadphaseShouldRunIntegration();
    testBroadphaseCountValidPairsGuards();
    testCellOccupancyPreflightCanSkipGuards();
    testBroadphaseMergePreflightCounts();
    testCellSpanClampPreflightGuards();
    testCellSpanClampRejectReasonGuards();
    testPairBufferSortShouldRunIntegration();
    testPairBufferSlotWriteRejectReasonGuards();
    testPairBufferMergeIntoRejectReasonGuards();
    testBroadphaseMergeIntoBufferPreflightGuards();
    testPairBufferCompactClampRejectReasonGuards();
    testRefineBroadphaseActivePairCountPreflight();
    testBroadphaseMergeStatsPreflight();
    testPairBufferMergeRejectReasonGuards();
    testCellSpanRejectReasonGuards();
    testBroadphaseMergeBufferRejectReasonGuards();
    testPairBufferAcceptPairsRejectReasonGuards();
    testMergePairsIntoBufferPartialCapacityPreflight();
    testBroadphaseShapeInsertPreflightGuards();
    testRefineBroadphaseAllSlotsInvalidGuard();
    testPairBufferSortAlreadySortedGuard();
    testMergePairsAllInvalidPreflightGuards();
    testBroadphasePairSlotRejectReasonGuards();
    testPairBufferClampShouldRunWiring();
    testPairBufferPrepareSlotsPreflightGuards();
    testShouldRunBroadphasePairGenerationGuards();
    testMergePairsIntoBufferRejectsForReasonGuards();
    testBroadphaseDedupeAndClampPreflightIntegration();
    testCellSpanRejectReasonAndPreflightGuards();
    testRefineBroadphasePreflightRejectsForReasonGuards();
    testDedupeBroadphasePreflightRejectsForReasonGuards();
    testMergeBroadphasePreflightRejectsForReasonGuards();
    testShapeCellInsertPreflightGuards();
    testBroadphaseCellSlotRejectReasonGuards();
    testBroadphaseCellPairGenPreflightGuards();
    testCellPairGenPreflightGuards();
    testCellShapeInsertPreflightGuards();
    testPairBufferInvalidateSlotPreflightGuards();
    testCellCapacityInsertPreflightGuards();
    testRefineMergePreflightCountGuards();
    testPairBufferSlotInvalidatePreflightGuards();
    testPairBufferWriteRejectReasonGuards();
    testPairBufferInvalidateRejectReasonGuards();
    testCellCapacityInsertRejectReasonGuards();
    testPairBufferCanSkipCompactAndClampGuard();
    testCellShapeInsertRejectReasonGuards();
    testBroadphaseCellCapacityInsertIntegration();
    testShapeCellOccupancyPreflightGuards();
    testMergePairPushPreflightGuards();
    testBroadphaseMergeBodyCountPreflight();
    testRefinePairRejectReasonGuards();
    testMergePairPushRejectReasonGuards();
    testShapeCellOccupancyParamsPreflightGuards();
    testMergePairIntoBufferPreflightGuards();
    testShapeCellCapacityPreflightGuards();
    testRefineDedupePreflightPairCountGuards();
    testBroadphaseMergePreflightBodyCountGuards();
    testMergePairsIntoBufferInsufficientCapacityPreflight();
    testCellOccupancyPreflightRemainingBudgetGuards();
    testRefineAndDedupePreflightCountGuards();
    testMergePairsIntoBufferCapacityPreflightGuards();
    testRefineInvalidateSlotPreflightGuards();
    testCellSpanCapacityGuards();
    testCellSpanCapacityRejectReasonGuards();
    testDedupeBroadphasePairBufferLayerGuards();
    testShapeCellInsertionPreflightGuards();
    testRefinePairSlotPreflightGuards();
    testMergeBroadphasePushPreflightGuards();
    testRefinePairSlotRejectReasonGuards();
    testMergePairsIntoBufferExtendedPreflightGuards();
    testCellOccupancyForParamsGuards();
    testDedupeBroadphasePairBufferPreflightParity();
    testMergePairsIntoBufferPreflightFields();
    testCellCapacityPreflightGuards();
    testCellOccupancyPreflightShapeInsertGuards();
    testCellOccupancyBudgetRemainingPreflight();
    testRefineAndDedupeBroadphasePreflightGuards();
    testBroadphaseMergeDeepenPreflightGuards();
    testMergePairsIntoBufferDeepenPreflightGuards();
    testPairBufferWriteInvalidatePreflightGuards();
    testCellSpanCapacityPreflightGuards();
    testRefinePairAndDedupeDeepenGuards();
    testPairBufferInvalidateSlotRejectReasonGuards();
    testDedupePairBufferSoAWithPreflightGuards();
    testMergePairsIntoBufferPreflightCapacityFields();
    testShapeCellHashInsertPreflightGuards();
    testCellCapacityRejectReasonAndPreflight();
    testBroadphaseMergePlaneDynamicWithPreflightGuards();
    testShapeCellInsertionRejectReasonAndPreflight();
    testRefineDedupeMergeWithPreflightReturnGuards();
    testPairBufferWouldSkipWriteAndPushGuards();
    testCellPairGenRejectReasonGuards();
    testShapeCellInsertRejectReasonGuards();
    testBroadphaseWouldSkipGuards();
    testWouldSkipBroadphaseGuards();
    testWouldSkipPairBufferWriteInvalidateGuards();
    testWouldSkipCellCapacityGuards();
    testWouldSkipRefineDedupeMergeGuards();
    testPairBufferWouldSkipWriteInvalidateGuards();
    testPairBufferWouldSkipWriteAndInvalidateGuards();

    if (g_failures == 0) {
        std::printf("fuse_physics_broadphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_broadphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
