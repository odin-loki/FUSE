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
    expectEq(buffer.countValidSlots(), 0u, "countValidSlots early-outs when empty");
    expectEq(buffer.applyMaxCapacityClamp(), 0u, "applyMaxCapacityClamp early-outs when empty");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs when empty");

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
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::candidatePairRejectReason(0u, 2u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::OutOfRangeBody),
             "out-of-range pair reports OutOfRangeBody reject reason");
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::candidatePairRejectReason(0u, 1u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::None),
             "in-range pair reports None reject reason");
}

void testCandidatePairRejectReasonName() {
    expectTrue(std::strcmp(fuse::physics::broadphase::candidatePairRejectReasonName(
                               fuse::physics::broadphase::CandidatePairRejectReason::None),
                           "None") == 0,
               "None reject reason has stable label");
    expectTrue(std::strcmp(fuse::physics::broadphase::candidatePairRejectReasonName(
                               fuse::physics::broadphase::CandidatePairRejectReason::SelfPair),
                           "SelfPair") == 0,
               "SelfPair reject reason has stable label");
    expectTrue(std::strcmp(fuse::physics::broadphase::candidatePairRejectReasonName(
                               fuse::physics::broadphase::CandidatePairRejectReason::OutOfRangeBody),
                           "OutOfRangeBody") == 0,
               "OutOfRangeBody reject reason has stable label");
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
}

void testPairBufferCapacityGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(2u);
    expectTrue(!buffer.isFull(), "empty buffer is not full");
    expectEq(buffer.remainingCapacity(), 2u, "empty buffer reports full remaining capacity");

    expectTrue(buffer.push(0u, 1u), "push accepts pair under capacity");
    expectTrue(!buffer.isFull(), "partial buffer is not full");
    expectEq(buffer.remainingCapacity(), 1u, "partial buffer reports one remaining slot");

    expectTrue(buffer.push(2u, 3u), "push accepts second pair at capacity");
    expectTrue(buffer.isFull(), "buffer at max capacity reports full");
    expectEq(buffer.remainingCapacity(), 0u, "full buffer reports zero remaining capacity");
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
}

void testPairBufferPreparePairSlotsZeroGuard() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(0u);
    expectTrue(buffer.canSkipSoAIteration(), "preparePairSlots(0) clears slot storage");
    expectTrue(buffer.isEmpty(), "preparePairSlots(0) leaves empty buffer");
    expectEq(buffer.compact(), 0u, "compact on zero slots returns zero");
    expectEq(buffer.countValidSlots(), 0u, "countValidSlots on zero slots returns zero");
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
}

void testBroadphaseNormalizedParamsGuard() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 0.f;
    params.tableSize = 0u;
    params.bodyCount = bodies.count();

    const auto pairs = fuse::physics::broadphase::runBroadphase(bodies, shapes, params);
    expectTrue(!pairs.empty(), "normalized zero params still find overlapping pair");
}

void testPairBufferCompactionEarlyOuts() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(buffer.countValidSlots(), 2u, "countValidSlots counts prepared valid slots");
    expectTrue(buffer.canSkipCompaction(), "all-valid slots skip compaction work");
    expectEq(buffer.compact(), 2u, "compact early-out preserves active count");
    expectEq(buffer.activeCount, 2u, "compact early-out leaves pairs intact");
}

void testEmptyBroadphaseInputGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseInput(bodies, shapes),
               "empty bodies and shapes is empty broadphase input");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphasePairGeneration(bodies, shapes),
               "canSkipBroadphasePairGeneration on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseInput(bodies, shapes),
               "bodies without shapes is empty broadphase input");
    expectTrue(fuse::physics::broadphase::isSingletonBroadphaseInput(bodies, shapes),
               "single body without matching shape count is singleton input");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase when shapes are missing");

    bodies.clear();
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseInput(bodies, shapes),
               "shapes without bodies is empty broadphase input");
    expectTrue(fuse::physics::broadphase::isSingletonBroadphaseInput(bodies, shapes),
               "orphan shape is singleton broadphase input");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    expectTrue(fuse::physics::broadphase::isSingletonBroadphaseInput(bodies, shapes),
               "one body and one shape is singleton broadphase input");
    expectTrue(fuse::physics::broadphase::canSkipBroadphasePairGeneration(bodies, shapes),
               "singleton scene skips pair generation");

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    expectTrue(fuse::physics::broadphase::isSingletonBroadphaseInput(bodies, shapes),
               "two bodies with one shape is singleton by shape count");

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectTrue(!fuse::physics::broadphase::isSingletonBroadphaseInput(bodies, shapes),
               "two bodies and two shapes is not singleton");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "populated scene does not skip broadphase");
}

void testCandidatePairRejectsForReasonGuards() {
    expectTrue(fuse::physics::broadphase::candidatePairRejectsForReason(
                   1u, 1u, 0u, fuse::physics::broadphase::CandidatePairRejectReason::SelfPair),
               "candidatePairRejectsForReason matches self-pair");
    expectTrue(fuse::physics::broadphase::candidatePairRejectsForReason(
                   0u, 2u, 2u, fuse::physics::broadphase::CandidatePairRejectReason::OutOfRangeBody),
               "candidatePairRejectsForReason matches out-of-range");
    expectTrue(!fuse::physics::broadphase::candidatePairRejectsForReason(
                   0u, 1u, 2u, fuse::physics::broadphase::CandidatePairRejectReason::SelfPair),
               "valid pair does not reject for SelfPair");

    const fuse::physics::broadphase::CandidatePair pair{0u, 1u};
    expectTrue(!fuse::physics::broadphase::candidatePairRejectsForReason(
                   pair, 2u, fuse::physics::broadphase::CandidatePairRejectReason::OutOfRangeBody),
               "candidatePair overload accepts in-range pair");
}

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

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::cellOccupancyWithinBudget(inverted, 1u),
               "empty range is within any positive budget");
    expectEq(fuse::physics::broadphase::occupancyBudgetRemaining(inverted, 4u), 4u,
             "empty range leaves full occupancy budget");
}

void testEstimatePairCountForUniqueBodies() {
    expectEq(fuse::physics::broadphase::estimatePairCountForUniqueBodies(0u), 0u,
             "zero bodies yields zero pairs");
    expectEq(fuse::physics::broadphase::estimatePairCountForUniqueBodies(1u), 0u,
             "single body yields zero pairs");
    expectEq(fuse::physics::broadphase::estimatePairCountForUniqueBodies(3u), 3u,
             "three unique bodies yield three pairs");
    expectEq(fuse::physics::broadphase::estimatePairCountForUniqueBodies(4u), 6u,
             "four unique bodies yield six pairs");
}

void testPairBufferCanAcceptPairsGuard() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(2u);
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
}

void testBroadphaseCanSkipIntegration() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 128;

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "integration scene is skippable before population");
    expectTrue(buffer.isEmpty(), "skippable broadphase leaves empty pair buffer");
}

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

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectEq(static_cast<fuse::u32>(
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

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::CellOccupancyPreflight planePreflight =
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 4u);
    expectTrue(!planePreflight.canIterate(), "2D preflight rejects over-budget range");
    expectEq(planePreflight.occupancyCount, 8u, "2D preflight reports occupancy count");
}

void testBroadphasePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const fuse::physics::broadphase::BroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphase(bodies, shapes);
    expectTrue(emptyPreflight.emptyInput, "preflight marks empty scene");
    expectTrue(!emptyPreflight.singletonInput, "preflight does not mark empty scene singleton");
    expectTrue(!emptyPreflight.canRun(), "preflight cannot run on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphasePreflight singletonPreflight =
        fuse::physics::broadphase::preflightBroadphase(bodies, shapes);
    expectTrue(!singletonPreflight.emptyInput, "preflight does not mark singleton scene empty");
    expectTrue(singletonPreflight.singletonInput, "preflight marks singleton scene");
    expectTrue(!singletonPreflight.canRun(), "preflight cannot run on singleton scene");

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphasePreflight populatedPreflight =
        fuse::physics::broadphase::preflightBroadphase(bodies, shapes);
    expectTrue(!populatedPreflight.emptyInput, "preflight does not mark populated scene empty");
    expectTrue(!populatedPreflight.singletonInput, "preflight does not mark populated scene singleton");
    expectTrue(populatedPreflight.canRun(), "preflight can run on populated scene");
}

void testRefineBroadphasePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    fuse::physics::broadphase::PairBufferSoA buffer;
    const fuse::physics::broadphase::RefineBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightRefineBroadphase(bodies, shapes, buffer);
    expectTrue(emptyPreflight.emptyBuffer, "refine preflight marks empty buffer");
    expectTrue(emptyPreflight.noValidPairs, "refine preflight marks no valid pairs");
    expectTrue(emptyPreflight.emptyInput, "refine preflight marks empty input");
    expectTrue(!emptyPreflight.canRefine(), "refine preflight cannot refine empty scene");
    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    const fuse::physics::broadphase::RefineBroadphasePreflight validPreflight =
        fuse::physics::broadphase::preflightRefineBroadphase(bodies, shapes, buffer);
    expectTrue(validPreflight.canRefine(), "refine preflight accepts valid scene");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase false when refine is viable");
}

void testDedupeBroadphasePreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    const fuse::physics::broadphase::DedupeBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "dedupe preflight marks empty buffer");
    expectTrue(!emptyPreflight.canDedupe(), "dedupe preflight skips empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase false on empty buffer");

    buffer.push(0u, 1u);
    const fuse::physics::broadphase::DedupeBroadphasePreflight singlePreflight =
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectTrue(singlePreflight.singlePair, "dedupe preflight marks single pair");
    expectTrue(!singlePreflight.canDedupe(), "dedupe preflight skips single pair");

    buffer.push(2u, 3u);
    const fuse::physics::broadphase::DedupeBroadphasePreflight multiPreflight =
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectTrue(multiPreflight.canDedupe(), "dedupe preflight accepts multiple pairs");
    expectTrue(fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase true for multiple pairs");
}

void testPairBufferPreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(1u);

    const fuse::physics::broadphase::PairBufferPushPreflight validPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u);
    expectTrue(validPush.canPush(), "push preflight accepts valid pair under capacity");

    const fuse::physics::broadphase::PairBufferPushPreflight invalidPush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 2u);
    expectTrue(invalidPush.invalidPair, "push preflight marks self-pair invalid");
    expectTrue(!invalidPush.canPush(), "push preflight rejects self-pair");

    buffer.push(0u, 1u);
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
        fuse::physics::broadphase::preflightPairBufferCompaction(slotBuffer);
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
}

void testBroadphaseBoxShapeCellRange() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({1.2f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Box, 0, {1.f, 1.f, 1.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Box, 1, {1.f, 1.f, 1.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 128;
    params.bodyCount = bodies.count();

    const auto pairs = fuse::physics::broadphase::runBroadphase(bodies, shapes, params);
    expectTrue(!pairs.empty(), "box shapes emit candidate pairs via AABB cell range");
}

void testBroadphaseSingletonEarlyOut() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});

    fuse::physics::broadphase::SpatialHashParams params;
    params.cellSize = 2.f;
    params.tableSize = 128;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(fuse::physics::broadphase::canSkipBroadphasePairGeneration(bodies, shapes),
               "singleton scene is skippable for pair generation");
    expectTrue(buffer.isEmpty(), "singleton broadphase leaves empty pair buffer");
}

void testBroadphaseCellOccupancyBudgetIntegration() {
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
    params.maxCellOccupancy = 8u;
    params.bodyCount = bodies.count();

    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    expectTrue(buffer.isEmpty(), "occupancy budget skips flooding shape insertion");
}

void testPairBufferReserveForUniqueBodies() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.reserveForUniqueBodies(4u);
    expectTrue(buffer.bodyA.capacity() >= 6u, "reserveForUniqueBodies sizes for n*(n-1)/2 pairs");
    expectTrue(buffer.canSkipMaxCapacityClamp(), "fresh buffer skips max-capacity clamp");
}

void testPairBufferCanSkipMaxCapacityClamp() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(buffer.canSkipMaxCapacityClamp(), "empty buffer skips max-capacity clamp");

    buffer.setMaxCapacity(2u);
    buffer.push(0u, 1u);
    expectTrue(buffer.canSkipMaxCapacityClamp(), "under-capacity buffer skips post clamp");
    buffer.push(2u, 3u);
    expectTrue(buffer.canSkipMaxCapacityClamp(), "at-capacity buffer skips post clamp");

    fuse::physics::broadphase::PairBufferSoA overflowBuffer;
    overflowBuffer.push(0u, 1u);
    overflowBuffer.push(2u, 3u);
    overflowBuffer.push(4u, 5u);
    overflowBuffer.setMaxCapacity(2u);
    expectTrue(!overflowBuffer.canSkipMaxCapacityClamp(), "overflow buffer needs post clamp");
}

void testBroadphaseRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectEq(static_cast<fuse::u32>(
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

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::SingletonInput),
             "singleton scene reports SingletonInput reject reason");

    bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::broadphaseRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::None),
             "populated scene reports None reject reason");

    const fuse::physics::broadphase::BroadphasePreflight preflight =
        fuse::physics::broadphase::preflightBroadphase(bodies, shapes);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseRejectReason::None),
             "preflightBroadphase carries reject reason");
    expectTrue(preflight.canRun(), "populated preflight can run");
}

void testRefineBroadphaseRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectEq(static_cast<fuse::u32>(
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

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::refineBroadphaseRejectReason(bodies, shapes, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::None),
             "valid refine scene reports None reject reason");

    const fuse::physics::broadphase::RefineBroadphasePreflight preflight =
        fuse::physics::broadphase::preflightRefineBroadphase(bodies, shapes, buffer);
    expectTrue(preflight.canRefine(), "refine preflight accepts valid scene with reason None");
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::None),
             "refine preflight carries reject reason");
}

void testDedupeBroadphaseRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::dedupeBroadphaseRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer dedupe reject reason");
    expectTrue(fuse::physics::broadphase::dedupeBroadphaseRejectsForReason(
                   buffer, fuse::physics::broadphase::DedupeBroadphaseRejectReason::EmptyBuffer),
               "dedupeBroadphaseRejectsForReason matches empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipDedupeBroadphase(buffer),
               "canSkipDedupeBroadphase on empty buffer");
    expectTrue(!fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase false when canSkipDedupeBroadphase true");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::dedupeBroadphaseRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::SinglePair),
             "single pair reports SinglePair dedupe reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::dedupeBroadphaseRejectReasonName(
                               fuse::physics::broadphase::DedupeBroadphaseRejectReason::SinglePair),
                           "SinglePair") == 0,
               "SinglePair dedupe reject reason has stable label");

    buffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::dedupeBroadphaseRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::None),
             "multiple pairs report None dedupe reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunDedupeBroadphase(buffer),
               "shouldRunDedupeBroadphase true for multiple pairs");
    expectTrue(!fuse::physics::broadphase::canSkipDedupeBroadphase(buffer),
               "canSkipDedupeBroadphase false for multiple pairs");

    const fuse::physics::broadphase::DedupeBroadphasePreflight preflight =
        fuse::physics::broadphase::preflightDedupeBroadphase(buffer);
    expectTrue(preflight.canDedupe(), "dedupe preflight accepts multiple pairs with reason None");
}

void testCellOccupancyRejectsForReasonGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::cellOccupancyRejectsForReason(
                   validRange, 8u, fuse::physics::broadphase::CellOccupancyRejectReason::None),
               "valid range rejects for None");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::cellOccupancyRejectsForReason(
                   inverted, 4u, fuse::physics::broadphase::CellOccupancyRejectReason::EmptyRange),
               "inverted range rejects for EmptyRange");
    expectTrue(fuse::physics::broadphase::cellOccupancyRejectsForReason(
                   validRange, 7u, fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
               "over-budget range rejects for ExceedsBudget");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    expectTrue(fuse::physics::broadphase::cellOccupancyRejectsForReason(
                   planeRange, 4u, fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
               "2D over-budget range rejects for ExceedsBudget");
}

void testPairBufferDedupeAndSortPreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    const fuse::physics::broadphase::PairBufferDedupePreflight emptyDedupe =
        fuse::physics::broadphase::preflightPairBufferDedupe(buffer);
    expectTrue(!emptyDedupe.canDedupe(), "empty buffer dedupe preflight cannot dedupe");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe on empty buffer");

    const fuse::physics::broadphase::PairBufferSortPreflight emptySort =
        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectTrue(!emptySort.needsSort(), "empty buffer sort preflight does not need sort");
    expectTrue(buffer.isSortedCanonical(), "empty buffer is canonically sorted");

    buffer.push(0u, 1u);
    const fuse::physics::broadphase::PairBufferDedupePreflight singleDedupe =
        fuse::physics::broadphase::preflightPairBufferDedupe(buffer);
    expectTrue(!singleDedupe.canDedupe(), "single-pair dedupe preflight cannot dedupe");
    expectTrue(singleDedupe.singlePair, "single-pair dedupe preflight marks single pair");

    const fuse::physics::broadphase::PairBufferSortPreflight singleSort =
        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectTrue(!singleSort.needsSort(), "single-pair sort preflight does not need sort");

    buffer.push(2u, 3u);
    const fuse::physics::broadphase::PairBufferDedupePreflight multiDedupe =
        fuse::physics::broadphase::preflightPairBufferDedupe(buffer);
    expectTrue(multiDedupe.canDedupe(), "multi-pair dedupe preflight can dedupe");
    expectTrue(!fuse::physics::broadphase::canSkipPairBufferDedupe(buffer),
               "canSkipPairBufferDedupe false for multiple pairs");

    const fuse::physics::broadphase::PairBufferSortPreflight multiSort =
        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectTrue(multiSort.needsSort(), "multi-pair sort preflight needs sort");

    buffer.sortCanonical();
    expectTrue(buffer.isSortedCanonical(), "sortCanonical leaves canonical order via preflight gate");
}

void testBroadphaseMergePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const fuse::physics::broadphase::BroadphaseMergePreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectTrue(!emptyPreflight.canMerge(), "empty scene cannot merge plane-dynamic pairs");
    expectTrue(emptyPreflight.emptyPlaneBodies, "empty scene has no plane bodies");
    expectTrue(emptyPreflight.emptyDynamicBodies, "empty scene has no dynamic bodies");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight planeOnlyPreflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectTrue(!planeOnlyPreflight.canMerge(), "plane-only scene cannot merge");
    expectTrue(!planeOnlyPreflight.emptyPlaneBodies, "plane-only scene has plane bodies");
    expectTrue(planeOnlyPreflight.emptyDynamicBodies, "plane-only scene has no dynamic bodies");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    const fuse::physics::broadphase::BroadphaseMergePreflight mergePreflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectTrue(mergePreflight.canMerge(), "plane plus dynamic scene can merge");
    expectTrue(!mergePreflight.emptyPlaneBodies, "merge scene has plane bodies");
    expectTrue(!mergePreflight.emptyDynamicBodies, "merge scene has dynamic bodies");
}

void testPairBufferPushRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::None),
             "valid push reports None reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferPushRejectsForReason(
                   buffer, 0u, 1u, fuse::physics::broadphase::PairBufferPushRejectReason::None),
               "valid push rejects for None");

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
             "self-pair reports InvalidPair reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPushRejectReasonName(
                               fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
                           "AtCapacity") == 0,
               "AtCapacity push reject reason has stable label");

    buffer.setMaxCapacity(1u);
    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 2u, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
             "full buffer reports AtCapacity reject reason");

    const fuse::physics::broadphase::PairBufferPushPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 2u, 3u);
    expectTrue(!preflight.canPush(), "push preflight rejects at-capacity pair");
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
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferCompaction(buffer),
               "shouldRunPairBufferCompaction false on empty buffer");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
             "all-valid slots report AllValid compaction reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferCompactionRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
               "all-valid slots reject for AllValid");

    buffer.invalidateSlot(1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactionRejectReason::None),
             "invalid slots report None compaction reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferCompaction(buffer),
               "shouldRunPairBufferCompaction true when invalid slots exist");
}

void testPairBufferClampRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferClamp(buffer),
               "canSkipPairBufferClamp on empty buffer");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
             "within-capacity buffer reports WithinCapacity clamp reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunPairBufferClamp(buffer),
               "shouldRunPairBufferClamp false when within capacity");

    buffer.push(2u, 3u);
    buffer.push(4u, 5u);
    buffer.setMaxCapacity(2u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferClampRejectReason::None),
             "overflow buffer reports None clamp reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferClamp(buffer),
               "shouldRunPairBufferClamp true when overflow exists");
}

void testPairBufferDedupeRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer SoA dedupe reject reason");

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
             "single pair reports SinglePair SoA dedupe reject reason");

    buffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::None),
             "multiple pairs report None SoA dedupe reject reason");
    const fuse::physics::broadphase::PairBufferDedupePreflight preflight =
        fuse::physics::broadphase::preflightPairBufferDedupe(buffer);
    expectTrue(preflight.canDedupe(), "SoA dedupe preflight accepts multiple pairs with reason None");
}

void testCellOccupancyIterationSkipGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::shouldRunCellOccupancyIteration(validRange, 8u),
               "shouldRunCellOccupancyIteration true within budget");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 8u),
               "canSkipCellOccupancyIteration false within budget");

    const fuse::physics::broadphase::CellOccupancyPreflight preflight =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 8u);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "cell occupancy preflight carries reject reason");
    expectTrue(preflight.canIterate(), "cell occupancy preflight can iterate within budget");

    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "canSkipCellOccupancyIteration true over budget");
    expectTrue(!fuse::physics::broadphase::shouldRunCellOccupancyIteration(validRange, 7u),
               "shouldRunCellOccupancyIteration false over budget");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(planeRange, 4u),
               "2D canSkipCellOccupancyIteration true over budget");
}

void testRefineBroadphaseShouldRunGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectTrue(!fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase true when shouldRunRefineBroadphase false");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    expectTrue(fuse::physics::broadphase::shouldRunRefineBroadphase(bodies, shapes, buffer),
               "shouldRunRefineBroadphase true for valid scene");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "canSkipRefineBroadphase false when shouldRunRefineBroadphase true");
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

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(buffer)),
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

    const fuse::physics::broadphase::PairBufferSortPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferSort(buffer);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::None),
             "sort preflight carries reject reason");
}

void testPairBufferCompactAndClampPreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::EmptyBuffer),
             "empty buffer reports EmptyBuffer compact-and-clamp reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferCompactAndClamp(buffer),
               "canSkipPairBufferCompactAndClamp on empty buffer");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs via preflight on empty buffer");

    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWork),
             "synced within-capacity buffer reports NoWork compact-and-clamp reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferCompactAndClampRejectsForReason(
                   buffer,
                   fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWork),
               "synced buffer rejects for NoWork");
    expectEq(buffer.compactAndClamp(), 2u, "compactAndClamp no-op returns synced active count");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    buffer.writeSlot(1u, 2u, 3u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::None),
             "prepared slots with stale activeCount report None compact-and-clamp reject reason");

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
}

void testShouldRunBroadphaseGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectTrue(!fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase false on empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase true when shouldRunBroadphase false");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::shouldRunBroadphase(bodies, shapes),
               "shouldRunBroadphase true on populated scene");
    expectTrue(!fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase false when shouldRunBroadphase true");
}

void testMergePairsIntoBufferPreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs;

    expectEq(static_cast<fuse::u32>(
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
    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::mergePairsIntoBufferRejectReason(pairs, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
             "full buffer reports BufferFull merge-into-buffer reject reason");
    expectTrue(!fuse::physics::broadphase::shouldRunMergePairsIntoBuffer(pairs, buffer),
               "shouldRunMergePairsIntoBuffer false when buffer is full");
}

void testPairBufferWriteSlotRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
             "valid write-slot reports None reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "shouldRunPairBufferWriteSlot true for valid slot");

    expectEq(static_cast<fuse::u32>(
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

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 0u, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
             "self-pair write-slot reports InvalidPair reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u),
               "canSkipPairBufferWriteSlot true for invalid pair");

    buffer.writeSlot(0u, 0u, 1u);
    expectTrue(buffer.slotIsValid(0u), "writeSlot accepts valid pair via preflight gate");
    buffer.writeSlot(1u, 1u, 1u);
    expectTrue(!buffer.slotIsValid(1u), "writeSlot rejects self-pair via preflight gate");
}

void testPairBufferToVectorRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
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

    buffer.push(0u, 1u);
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferToVectorRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferToVectorRejectReason::None),
             "non-empty buffer reports None toVector reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunPairBufferToVector(buffer),
               "shouldRunPairBufferToVector true for non-empty buffer");
    expectEq(buffer.toVector().size(), 1u, "toVector exports pair after preflight gate");
}

void testCellSpanRejectReasonAndPreflight() {
    const fuse::physics::broadphase::CellRange3 withinRange = {{0, 0, 0}, {3, 3, 3}};
    expectTrue(fuse::physics::broadphase::cellSpanWithinPerAxisLimit(withinRange, 4u),
               "4-cell span is within limit of four");
    expectTrue(!fuse::physics::broadphase::exceedsCellSpanPerAxis(withinRange, 4u),
               "4-cell span does not exceed limit of four");
    expectEq(static_cast<fuse::u32>(
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

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectEq(static_cast<fuse::u32>(
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
}

void testRefineDedupeMergeWithPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    expectTrue(!fuse::physics::broadphase::refineBroadphasePairsParallelWithPreflight(bodies, shapes, buffer),
               "refine with preflight skips empty scene");
    expectTrue(buffer.isEmpty(), "refine with preflight leaves empty buffer unchanged");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 2, {1.f, 0.f, 0.f});

    buffer.push(0u, 1u);
    buffer.push(0u, 2u);
    expectTrue(fuse::physics::broadphase::refineBroadphasePairsParallelWithPreflight(bodies, shapes, buffer),
               "refine with preflight runs on valid scene");
    expectEq(buffer.activeCount, 1u, "refine with preflight removes separated pair");
    expectTrue(buffer.containsCanonicalPair(0u, 1u), "refine with preflight keeps overlapping pair");

    fuse::physics::broadphase::PairBufferSoA dedupeBuffer;
    dedupeBuffer.push(0u, 1u);
    fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer);
    expectEq(dedupeBuffer.activeCount, 1u, "dedupe with preflight is no-op on single pair");

    dedupeBuffer.push(0u, 1u);
    dedupeBuffer.push(2u, 3u);
    dedupeBuffer.push(0u, 1u);
    fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer);
    expectEq(dedupeBuffer.activeCount, 2u, "dedupe with preflight removes duplicate pairs");

    fuse::physics::broadphase::PairBufferSoA mergeBuffer;
    const std::vector<fuse::physics::broadphase::CandidatePair> mergePairs = {{0u, 1u}, {2u, 3u}};
    fuse::physics::broadphase::mergePairsIntoBufferWithPreflight(mergePairs, mergeBuffer);
    expectEq(mergeBuffer.activeCount, 2u, "merge with preflight pushes valid pairs");

    mergeBuffer.setMaxCapacity(2u);
    expectTrue(mergeBuffer.isFull(), "merge buffer at capacity after setMaxCapacity");
    expectTrue(fuse::physics::broadphase::mergePairsIntoBufferRejectsForReason(
                   mergePairs, mergeBuffer,
                   fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
               "full buffer rejects for BufferFull after merge");
    expectTrue(!fuse::physics::broadphase::shouldRunMergePairsIntoBuffer(mergePairs, mergeBuffer),
               "shouldRunMergePairsIntoBuffer false when buffer is full");
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

void testPairBufferInvalidateSlotRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
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
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot invalidate reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateSlotRejectsForReason(
                   buffer, 2u,
                   fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
               "out-of-range slot rejects for OutOfRangeSlot");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
                           "AlreadyInvalid") == 0,
               "AlreadyInvalid invalidate reject reason has stable label");

    expectTrue(buffer.invalidateSlotWithPreflight(0u), "invalidateSlotWithPreflight clears valid slot");
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlotWithPreflight leaves slot invalid");
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
             "already-invalid slot reports AlreadyInvalid invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "canSkipPairBufferInvalidateSlot true for already-invalid slot");
    expectTrue(!buffer.invalidateSlotWithPreflight(0u), "invalidateSlotWithPreflight is no-op on invalid slot");
    expectTrue(!buffer.invalidateSlotWithPreflight(99u), "invalidateSlotWithPreflight ignores out-of-range slot");
}

void testShapeCellInsertionPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    const fuse::physics::broadphase::ShapeCellInsertionPreflight validPreflight =
        fuse::physics::broadphase::preflightShapeCellInsertion(validRange, 8u);
    expectTrue(validPreflight.canInsert(), "shape cell-insertion preflight accepts within-budget range");
    expectEq(validPreflight.occupancyCount, 8u, "shape cell-insertion preflight reports occupancy count");
    expectEq(validPreflight.budgetRemaining, 0u, "shape cell-insertion preflight reports zero budget remaining at limit");
    expectTrue(fuse::physics::broadphase::shouldRunShapeCellInsertion(validRange, 8u),
               "shouldRunShapeCellInsertion true within budget");

    const fuse::physics::broadphase::CellRange3 overBudgetRange = {{0, 0, 0}, {2, 1, 1}};
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::shapeCellInsertionRejectReason(overBudgetRange, 5u)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::ShapeCellInsertionRejectReason::ExceedsOccupancyBudget),
             "over-budget range reports ExceedsOccupancyBudget shape cell-insertion reject reason");
    expectTrue(fuse::physics::broadphase::shapeCellInsertionRejectsForReason(
                   overBudgetRange, 5u,
                   fuse::physics::broadphase::ShapeCellInsertionRejectReason::ExceedsOccupancyBudget),
               "over-budget range rejects for ExceedsOccupancyBudget");
    expectTrue(fuse::physics::broadphase::canSkipShapeCellInsertion(overBudgetRange, 5u),
               "canSkipShapeCellInsertion true for over-budget range");
    expectTrue(std::strcmp(fuse::physics::broadphase::shapeCellInsertionRejectReasonName(
                               fuse::physics::broadphase::ShapeCellInsertionRejectReason::EmptyRange),
                           "EmptyRange") == 0,
               "EmptyRange shape cell-insertion reject reason has stable label");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const fuse::physics::broadphase::ShapeCellInsertionPreflight planePreflight =
        fuse::physics::broadphase::preflightShapeCellInsertion(planeRange, 4u);
    expectTrue(!planePreflight.canInsert(), "2D shape cell-insertion preflight rejects over-budget range");
    expectEq(planePreflight.occupancyCount, 8u, "2D shape cell-insertion preflight reports occupancy count");
}

void testDedupePairBufferSoAWithPreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::dedupePairBufferSoAWithPreflight(buffer);
    expectTrue(buffer.isEmpty(), "SoA dedupe with preflight is no-op on empty buffer");

    buffer.push(0u, 1u);
    fuse::physics::broadphase::dedupePairBufferSoAWithPreflight(buffer);
    expectEq(buffer.activeCount, 1u, "SoA dedupe with preflight is no-op on single pair");

    buffer.push(0u, 1u);
    buffer.push(2u, 3u);
    buffer.push(0u, 1u);
    fuse::physics::broadphase::dedupePairBufferSoAWithPreflight(buffer);
    expectEq(buffer.activeCount, 2u, "SoA dedupe with preflight removes duplicate pairs");
    expectTrue(buffer.isSortedCanonical(), "SoA dedupe with preflight leaves canonical order");
}

void testMergePairsIntoBufferPreflightCapacityFields() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(4u);
    buffer.push(0u, 1u);
    buffer.push(2u, 3u);

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{4u, 5u}, {6u, 7u}};
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight preflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, buffer);
    expectTrue(preflight.canMerge(), "merge preflight accepts pairs with remaining capacity");
    expectEq(preflight.incomingPairCount, 2u, "merge preflight reports incoming pair count");
    expectEq(preflight.remainingCapacity, 2u, "merge preflight reports remaining capacity");

    buffer.push(8u, 9u);
    buffer.push(10u, 11u);
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight fullPreflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, buffer);
    expectTrue(!fullPreflight.canMerge(), "merge preflight rejects when buffer is full");
    expectEq(fullPreflight.remainingCapacity, 0u, "merge preflight reports zero remaining capacity on full buffer");
}

void testBroadphaseMergeRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::mergeBroadphaseRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
             "empty scene reports EmptyPlaneBodies merge reject reason");
    expectTrue(fuse::physics::broadphase::mergeBroadphaseRejectsForReason(
                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyPlaneBodies),
               "mergeBroadphaseRejectsForReason matches empty scene");
    expectTrue(fuse::physics::broadphase::canSkipBroadphaseMerge(bodies, shapes),
               "canSkipBroadphaseMerge on empty scene");
    expectTrue(!fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge false on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::mergeBroadphaseRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
             "plane-only scene reports EmptyDynamicBodies merge reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::mergeBroadphaseRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
                           "EmptyDynamicBodies") == 0,
               "EmptyDynamicBodies merge reject reason has stable label");

    bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {0.5f, 0.f, 0.f});
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::mergeBroadphaseRejectReason(bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "plane plus dynamic scene reports None merge reject reason");
    expectTrue(fuse::physics::broadphase::shouldRunBroadphaseMerge(bodies, shapes),
               "shouldRunBroadphaseMerge true for mergeable scene");

    const fuse::physics::broadphase::BroadphaseMergePreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMerge(bodies, shapes);
    expectEq(static_cast<fuse::u32>(preflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
             "merge preflight carries reject reason");
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
    testBroadphaseCanSkipIntegration();
    testCellOccupancyRejectReasonAndPreflight();
    testBroadphasePreflightGuards();
    testRefineBroadphasePreflightGuards();
    testDedupeBroadphasePreflightGuards();
    testPairBufferPreflightGuards();
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
    testPairBufferToVectorRejectReasonGuards();
    testCellSpanRejectReasonAndPreflight();
    testRefineDedupeMergeWithPreflightGuards();
    testPairBufferInvalidateSlotRejectReasonGuards();
    testShapeCellInsertionPreflightGuards();
    testDedupePairBufferSoAWithPreflightGuards();
    testMergePairsIntoBufferPreflightCapacityFields();

    if (g_failures == 0) {
        std::printf("fuse_physics_broadphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_broadphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
