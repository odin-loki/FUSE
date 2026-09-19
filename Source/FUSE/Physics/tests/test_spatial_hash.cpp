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

void testPairBufferInvalidateSlotRejectReasonGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None),
             "in-range slot reports None invalidate reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateSlotRejectsForReason(
                   buffer, 0u, fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None),
               "in-range slot rejects for None");

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 4u)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot invalidate reject reason");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
                           "OutOfRangeSlot") == 0,
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
}

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
}

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

    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::shapeCellInsertRejectReason(
                 0u, bodies, shapes, params, false)),
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
}

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
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 0u),
               "wouldSkipPairBufferInvalidateSlot false for valid slot");

    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 2u)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "out-of-range slot reports OutOfRangeSlot invalidate reject reason");
    expectTrue(fuse::physics::broadphase::pairBufferInvalidateSlotRejectsForReason(
                   buffer, 2u,
                   fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
               "out-of-range slot rejects for OutOfRangeSlot");
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferInvalidateSlotRejectReasonName(
                               fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
                           "AlreadyInvalid") == 0,
               "AlreadyInvalid invalidate reject reason has stable label");

    fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason skipReason =
        fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferInvalidateSlot(buffer, 2u, &skipReason),
               "wouldSkipPairBufferInvalidateSlot true for out-of-range slot");
    expectEq(static_cast<fuse::u32>(skipReason),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::OutOfRangeSlot),
             "wouldSkipPairBufferInvalidateSlot reports OutOfRangeSlot");

    buffer.invalidateSlot(0u);
    expectEq(static_cast<fuse::u32>(
                 fuse::physics::broadphase::pairBufferInvalidateSlotRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(
                 fuse::physics::broadphase::PairBufferInvalidateSlotRejectReason::AlreadyInvalid),
             "already-invalid slot reports AlreadyInvalid invalidate reject reason");
    expectTrue(fuse::physics::broadphase::canSkipPairBufferInvalidateSlot(buffer, 0u),
               "canSkipPairBufferInvalidateSlot true for already-invalid slot");

    buffer.writeSlot(1u, 2u, 3u);
    buffer.invalidateSlot(2u);
    expectTrue(buffer.slotIsValid(1u), "invalidateSlot ignores out-of-range slot via preflight gate");
    buffer.writeSlot(0u, 0u, 1u);
    buffer.invalidateSlot(0u);
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears in-range slot via preflight gate");
}

void testPairBufferWouldSkipWriteSlotGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::broadphase::PairBufferWriteSlotRejectReason reason =
        fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u, &reason),
               "wouldSkipPairBufferWriteSlot false for valid write");
    expectEq(static_cast<fuse::u32>(reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
             "wouldSkipPairBufferWriteSlot reports None for valid write");

    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 1u, 1u, &reason),
               "wouldSkipPairBufferWriteSlot true for self-pair");
    expectEq(static_cast<fuse::u32>(reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidPair),
             "wouldSkipPairBufferWriteSlot reports InvalidPair for self-pair");
    expectTrue(fuse::physics::broadphase::wouldSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u) ==
                   fuse::physics::broadphase::canSkipPairBufferWriteSlot(buffer, 0u, 0u, 1u),
               "wouldSkipPairBufferWriteSlot agrees with canSkipPairBufferWriteSlot for valid write");
}

void testCellCapacityWouldSkipGuards() {
    const fuse::physics::broadphase::CellRange3 validRange = {{0, 0, 0}, {1, 1, 1}};
    fuse::physics::broadphase::CellOccupancyRejectReason occupancyReason =
        fuse::physics::broadphase::CellOccupancyRejectReason::None;
    expectTrue(!fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 8u, &occupancyReason),
               "wouldSkipCellOccupancyIteration false within budget");
    expectEq(static_cast<fuse::u32>(occupancyReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::None),
             "wouldSkipCellOccupancyIteration reports None within budget");

    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u, &occupancyReason),
               "wouldSkipCellOccupancyIteration true over budget");
    expectEq(static_cast<fuse::u32>(occupancyReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellOccupancyRejectReason::ExceedsBudget),
             "wouldSkipCellOccupancyIteration reports ExceedsBudget over budget");
    expectTrue(fuse::physics::broadphase::wouldSkipCellOccupancyIteration(validRange, 7u) ==
                   fuse::physics::broadphase::canSkipCellOccupancyIteration(validRange, 7u),
               "wouldSkipCellOccupancyIteration agrees with canSkipCellOccupancyIteration over budget");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
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
}

void testRefineDedupeMergeWouldSkipGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    fuse::physics::broadphase::RefineBroadphaseRejectReason refineReason =
        fuse::physics::broadphase::RefineBroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer, &refineReason),
               "wouldSkipRefineBroadphase true on empty scene");
    expectEq(static_cast<fuse::u32>(refineReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyBuffer),
             "wouldSkipRefineBroadphase reports EmptyBuffer on empty scene");
    expectTrue(fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer) ==
                   fuse::physics::broadphase::canSkipRefineBroadphase(bodies, shapes, buffer),
               "wouldSkipRefineBroadphase agrees with canSkipRefineBroadphase on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);
    expectTrue(!fuse::physics::broadphase::wouldSkipRefineBroadphase(bodies, shapes, buffer),
               "wouldSkipRefineBroadphase false for valid refine scene");

    fuse::physics::broadphase::DedupeBroadphaseRejectReason dedupeReason =
        fuse::physics::broadphase::DedupeBroadphaseRejectReason::None;
    expectTrue(fuse::physics::broadphase::wouldSkipDedupeBroadphase(buffer, &dedupeReason),
               "wouldSkipDedupeBroadphase true on single pair");
    expectEq(static_cast<fuse::u32>(dedupeReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::SinglePair),
             "wouldSkipDedupeBroadphase reports SinglePair");

    buffer.push(0u, 1u);
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

    shapes.addShape(fuse::physics::CollisionShapeType::Plane, 0, {0.f, 1.f, 0.f});
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
    expectEq(static_cast<fuse::u32>(mergeIntoReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferFull),
             "wouldSkipMergePairsIntoBuffer reports BufferFull");
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
    expectTrue(!fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer),
               "dedupe with preflight returns false on single pair");
    expectEq(dedupeBuffer.activeCount, 1u, "dedupe with preflight is no-op on single pair");

    dedupeBuffer.push(0u, 1u);
    dedupeBuffer.push(2u, 3u);
    dedupeBuffer.push(0u, 1u);
    expectTrue(fuse::physics::broadphase::dedupeBroadphasePairBufferWithPreflight(dedupeBuffer),
               "dedupe with preflight returns true for multiple pairs");
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
    testPairBufferInvalidateSlotRejectReasonGuards();
    testCellPairGenRejectReasonGuards();
    testShapeCellInsertRejectReasonGuards();
    testBroadphaseCellPairGenRejectReasonGuards();
    testPairBufferToVectorRejectReasonGuards();
    testCellSpanRejectReasonAndPreflight();
    testRefineDedupeMergeWithPreflightGuards();
    testPairBufferInvalidateSlotRejectReasonGuards();
    testPairBufferWouldSkipWriteSlotGuards();
    testCellCapacityWouldSkipGuards();
    testRefineDedupeMergeWouldSkipGuards();

    if (g_failures == 0) {
        std::printf("fuse_physics_broadphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_broadphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}

// --- deepen additive from deepen-b4-broadphase-guards-fcb2 ---
void testCandidateRejectReasonGuards() {
        fuse::physics::broadphase::candidatePairRejectReason(1u, 1u) ==
            fuse::physics::broadphase::CandidateRejectReason::SelfPair,
        fuse::physics::broadphase::candidatePairRejectReason(0u, 2u, 2u) ==
            fuse::physics::broadphase::CandidateRejectReason::OutOfRangeBody,
        fuse::physics::broadphase::candidatePairRejectReason(0u, 1u, 2u) ==
            fuse::physics::broadphase::CandidateRejectReason::None,
        fuse::physics::broadphase::candidatePairRejectReason({0u, 1u}, bodies, shapes) ==
            fuse::physics::broadphase::CandidateRejectReason::AabbSeparated,
        fuse::physics::broadphase::candidatePairRejectReason({0u, 0u}, bodies, shapes) ==
    expectTrue(std::strcmp(fuse::physics::broadphase::candidateRejectReasonLabel(
                               fuse::physics::broadphase::CandidateRejectReason::BufferFull),
void testPairBufferRejectReasonTracking() {
    expectTrue(buffer.lastRejectReason == fuse::physics::broadphase::CandidateRejectReason::SelfPair,
    expectTrue(buffer.lastRejectReason == fuse::physics::broadphase::CandidateRejectReason::BufferFull,
    testCandidateRejectReasonGuards();
    testPairBufferRejectReasonTracking();

// --- deepen additive from deepen-b4-broadphase-guards-90dc ---
        fuse::physics::broadphase::candidatePairRejectReason(2u, 2u) ==
            fuse::physics::broadphase::CandidatePairRejectReason::SelfPair,
            fuse::physics::broadphase::CandidatePairRejectReason::None,
            fuse::physics::broadphase::CandidatePairRejectReason::OutOfRangeBody,
        fuse::physics::broadphase::candidatePairRejectReason(selfPair, 4u) ==

// --- deepen additive from deepen-b4-broadphase-guards-cd2f ---
    expectTrue(!buffer.wouldRejectPush(0u, 1u), "wouldRejectPush accepts valid pair under capacity");
    expectTrue(buffer.wouldRejectPush(4u, 5u), "wouldRejectPush rejects when full");
void testPairBufferWriteSlotBodyCountGuard() {

// --- deepen additive from deepen-b4-broadphase-guards-b3ad ---
                               fuse::physics::broadphase::CandidatePairRejectReason::AabbSeparated),
                               fuse::physics::broadphase::CandidatePairRejectReason::BufferFull),
void testRejectedCandidatePairGuards() {
    expectTrue(fuse::physics::broadphase::isRejectedCandidatePair(1u, 1u),
    expectTrue(!fuse::physics::broadphase::isRejectedCandidatePair(0u, 1u, 2u),
    expectTrue(fuse::physics::broadphase::isRejectedCandidatePair(0u, 2u, 2u),
void testCanSkipBroadphaseEmptySetGuard() {
void testCellSpanExceedsClampGuards() {
void testPairBufferLastRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(buffer.lastRejectReason),
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::BufferFull),
void testPairBufferRefineAndInvalidSlotGuards() {
void testCandidatePairAabbRejectReason() {
                 fuse::physics::broadphase::candidatePairRejectReason(overlapping, bodies, shapes)),
                 fuse::physics::broadphase::candidatePairRejectReason(separated, bodies, shapes)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CandidatePairRejectReason::AabbSeparated),
    testPairBufferLastRejectReasonGuards();
    testCandidatePairAabbRejectReason();

// --- deepen additive from deepen-b4-broadphase-guards-bcce ---
void testPairBufferSlotModeDedupeGuard() {
void testPairBufferWouldRejectAdditionalPairs() {
    expectTrue(!buffer.wouldRejectAdditionalPairs(2u), "empty buffer accepts two pairs");
    expectTrue(buffer.wouldRejectAdditionalPairs(3u), "empty buffer rejects three pairs");
    expectTrue(!buffer.wouldRejectAdditionalPairs(0u), "zero additional pairs never rejected");
    expectTrue(buffer.wouldRejectAdditionalPairs(2u), "partial buffer rejects two more pairs");
    expectTrue(!buffer.wouldRejectAdditionalPairs(1u), "partial buffer accepts one more pair");
void testCanSkipBroadphaseRefineGuard() {
void testMaxCellSpanAxisGuards() {

// --- deepen additive from deepen-b4-broadphase-preflights-82c3 ---
void testShapeCellOccupancyPreflight() {
    const auto planePreflight =
    expectTrue(planePreflight.exceedsBudget, "2D preflight flags range above span budget");
    expectTrue(!planePreflight.can_insert(), "over-budget 2D range cannot insert");
    const auto emptyPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(emptyPreflight.skipped, "empty scene preflight is skipped");
    expectTrue(!emptyPreflight.can_run(), "empty scene preflight cannot run");
    const auto validPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(!validPreflight.skipped, "populated scene preflight is not skipped");
    expectTrue(validPreflight.can_run(), "populated scene preflight can run");
    expectEq(validPreflight.bodyCount, 1u, "preflight reports body count");
    expectEq(validPreflight.shapeCount, 1u, "preflight reports shape count");
void testBroadphaseRefinePreflightGuards() {
    const auto emptyPreflight =
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty input and buffer");
    expectTrue(!emptyPreflight.can_refine(), "empty refine preflight cannot refine");
    const auto validPreflight =
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid buffer");
    expectTrue(validPreflight.can_refine(), "refine preflight can refine valid buffer");
    expectEq(validPreflight.pairCount, 1u, "refine preflight reports pair count");
void testPairBufferDedupePreflights() {
    const auto emptyPreflight = buffer.preflight_dedupe();
    expectTrue(emptyPreflight.skipped, "empty buffer skips dedupe preflight");
    expectTrue(!emptyPreflight.needs_dedupe(), "empty buffer does not need dedupe");
    const auto singlePreflight = buffer.preflight_dedupe();
    expectTrue(singlePreflight.skipped, "single-pair buffer skips dedupe preflight");
    const auto duplicatePreflight = buffer.preflight_dedupe();
    expectTrue(!duplicatePreflight.skipped, "duplicate buffer runs dedupe preflight");
    expectEq(duplicatePreflight.duplicateCount, 1u, "preflight counts duplicate pairs");
    expectTrue(duplicatePreflight.needs_dedupe(), "duplicate buffer needs dedupe");
    testShapeCellOccupancyPreflight();
    testBroadphaseRefinePreflightGuards();
    testPairBufferDedupePreflights();

// --- deepen additive from deepen-b4-broadphase-preflights-ec03 ---
void testBroadphaseInputPreflight() {
    const fuse::physics::broadphase::BroadphaseInputPreflight emptyPreflight =
    expectTrue(emptyPreflight.skipped, "empty input preflight is skipped");
    expectTrue(emptyPreflight.emptyBodies, "empty input preflight marks empty bodies");
    expectTrue(emptyPreflight.emptyShapes, "empty input preflight marks empty shapes");
    expectTrue(!emptyPreflight.can_run(), "empty input preflight cannot run");
    expectTrue(fuse::physics::broadphase::should_skip_broadphase(bodies, shapes),
               "should_skip_broadphase on empty scene");
    const fuse::physics::broadphase::BroadphaseInputPreflight missingShapes =
    const fuse::physics::broadphase::BroadphaseInputPreflight validPreflight =
    expectTrue(!validPreflight.skipped, "populated input preflight is not skipped");
    expectTrue(validPreflight.can_run(), "populated input preflight can run");
    expectTrue(!fuse::physics::broadphase::should_skip_broadphase(bodies, shapes),
               "should_skip_broadphase on populated scene");
void testCellOccupancyPreflight() {
    const fuse::physics::broadphase::CellOccupancyPreflight emptyRange =
    expectTrue(planePreflight.exceedsBudget, "2D occupancy preflight flags exceed");
void testRefineBroadphasePreflight() {
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty buffer and input");
    expectTrue(fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on empty scene");
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid pair buffer");
    expectTrue(validPreflight.can_refine(), "valid refine preflight can refine");
    expectTrue(!fuse::physics::broadphase::should_skip_refine_broadphase(bodies, shapes, buffer),
               "should_skip_refine_broadphase on valid pair buffer");
void testPairBufferDedupePreflight() {
    const fuse::physics::broadphase::PairBufferDedupePreflight emptyPreflight =
    expectTrue(emptyPreflight.skipped, "dedupe preflight skips empty buffer");
    expectTrue(fuse::physics::broadphase::should_skip_dedupe_pair_buffer(buffer),
               "should_skip_dedupe_pair_buffer on empty buffer");
    const fuse::physics::broadphase::PairBufferDedupePreflight singlePreflight =
    expectTrue(singlePreflight.skipped, "dedupe preflight skips single pair");
    expectTrue(!singlePreflight.needs_dedupe(), "single pair does not need dedupe");
    const fuse::physics::broadphase::PairBufferDedupePreflight multiPreflight =
    expectTrue(!multiPreflight.skipped, "multi-pair dedupe preflight is not skipped");
    expectTrue(multiPreflight.needs_dedupe(), "multi-pair buffer needs dedupe");
    expectTrue(multiPreflight.can_dedupe(), "multi-pair buffer can dedupe");
    expectTrue(!fuse::physics::broadphase::should_skip_dedupe_pair_buffer(buffer),
               "should_skip_dedupe_pair_buffer on multi-pair buffer");
void testPairBufferClampPreflight() {
    const fuse::physics::broadphase::PairBufferClampPreflight emptyPreflight =
    expectTrue(emptyPreflight.skipped, "clamp preflight skips empty buffer");
    expectTrue(!emptyPreflight.needs_clamp(), "empty buffer does not need clamp");
    const fuse::physics::broadphase::PairBufferClampPreflight overflowPreflight =
    expectTrue(!overflowPreflight.skipped, "overflow clamp preflight is not skipped");
    expectTrue(overflowPreflight.needs_clamp(), "overflow buffer needs clamp");
    expectTrue(overflowPreflight.can_clamp(), "overflow buffer can clamp");
    expectEq(overflowPreflight.excessCount, 1u, "clamp preflight counts excess pairs");
void testPairBufferDedupeAndClampGuards() {
void testEmptyBroadphaseOutputGuard() {
    testBroadphaseInputPreflight();
    testCellOccupancyPreflight();
    testRefineBroadphasePreflight();
    testPairBufferDedupePreflight();
    testPairBufferClampPreflight();

// --- deepen additive from b4-broadphase-deepen-guards-1b87 ---
void testCellOccupancyPreflightGuards() {
        fuse::physics::broadphase::preflightCellOccupancy(unitRange, 8u);
        fuse::physics::broadphase::preflightCellOccupancy(unitRange, 7u);
        fuse::physics::broadphase::preflightCellOccupancy(inverted, 4u);
    expectTrue(emptyPreflight.skipped, "empty range preflight is skipped");
    expectEq(emptyPreflight.cellCount, 0u, "empty range reports zero cells");
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 8u);
    expectEq(planePreflight.cellCount, 8u, "2D preflight reports occupancy count");
void testPerShapeCellBudgetGuard() {
void testPairSlotPreflightGuards() {
    const auto zeroSlots = fuse::physics::broadphase::preflightPairSlots(0u, buffer);
    const auto withinCapacity = fuse::physics::broadphase::preflightPairSlots(3u, buffer);
    const auto exceedsCapacity = fuse::physics::broadphase::preflightPairSlots(8u, buffer);
        fuse::physics::broadphase::preflightRefineBroadphasePairs(buffer, bodies, shapes);
void testPairBufferDedupeAndCompactGuards() {
    testCellOccupancyPreflightGuards();
    testPairSlotPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-preflights-a60a ---
void testCellCapacityPreflightGuards() {
    expectTrue(planePreflight.exceedsOccupancyBudget, "2D preflight flags occupancy overflow");
void testBroadphaseInputPreflightGuards() {
    const auto emptyPreflight = fuse::physics::broadphase::preflight_broadphase_input(bodies, shapes);
    expectTrue(emptyPreflight.skipped, "empty scene is skipped by input preflight");
    expectTrue(emptyPreflight.emptyBodies, "empty scene has no bodies");
    expectTrue(emptyPreflight.emptyShapes, "empty scene has no shapes");
    expectTrue(!emptyPreflight.can_run(), "empty scene cannot run broadphase");
    const auto readyPreflight =
    expectTrue(!readyPreflight.skipped, "populated scene is not skipped");
    expectTrue(readyPreflight.can_run(), "populated scene can run broadphase");
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
    const auto droppedPreflight = fuse::physics::broadphase::preflight_pair_buffer(buffer);
    expectTrue(droppedPreflight.hasDropped, "rejected push sets dropped preflight");
    const auto emptyBufferPreflight =
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
void testBroadphaseDedupePreflightGuards() {
    const auto emptyPreflight = fuse::physics::broadphase::preflight_broadphase_dedupe(buffer);
    const auto singlePreflight = fuse::physics::broadphase::preflight_broadphase_dedupe(buffer);
    expectTrue(!singlePreflight.skipped, "single-pair buffer is not skipped");
    expectTrue(singlePreflight.noOp, "single-pair dedupe is a no-op");
    expectTrue(!singlePreflight.needs_dedupe(), "single-pair buffer does not need dedupe");
    const auto multiPreflight = fuse::physics::broadphase::preflight_broadphase_dedupe(buffer);
    expectTrue(!multiPreflight.noOp, "multi-pair buffer may need dedupe");
    expectTrue(multiPreflight.needs_dedupe(), "multi-pair buffer needs dedupe preflight");
void testPairBufferInvalidSlotGuards() {
void testEmptyCellBucketGuards() {
    testCellCapacityPreflightGuards();
    testBroadphaseInputPreflightGuards();
    testBroadphaseDedupePreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-abce ---
    expectTrue(planePreflight.exceedsBudget, "2D preflight flags budget overflow");
void testBroadphaseDispatchPreflightGuards() {
    expectTrue(emptyPreflight.emptyInput, "dispatch preflight marks empty scene");
    expectTrue(!emptyPreflight.can_dispatch(), "empty scene cannot dispatch broadphase");
    const auto populatedPreflight =
    expectTrue(!populatedPreflight.emptyInput, "populated scene is not empty input");
    expectTrue(populatedPreflight.can_dispatch(), "populated scene can dispatch broadphase");
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty scene and buffer");
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid scene");
    expectEq(validPreflight.validPairCount, 1u, "refine preflight counts valid pairs");
    testBroadphaseDispatchPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-preflights-76e1 ---
    expectTrue(!emptyPreflight.can_dispatch(), "empty scene preflight cannot dispatch");
    expectEq(emptyPreflight.bodyCount, 0u, "empty scene preflight reports zero bodies");
    expectEq(emptyPreflight.shapeCount, 0u, "empty scene preflight reports zero shapes");
    const auto populatedPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(!populatedPreflight.skipped, "populated scene preflight is not skipped");
    expectTrue(populatedPreflight.can_dispatch(), "populated scene preflight can dispatch");
    expectEq(populatedPreflight.bodyCount, 1u, "populated preflight reports body count");
    expectEq(populatedPreflight.shapeCount, 1u, "populated preflight reports shape count");
    expectTrue(emptyPreflight.emptyRange, "inverted range preflight is empty");
    expectTrue(emptyPreflight.skipped, "inverted range preflight is skipped");
    expectTrue(!emptyPreflight.can_iterate(), "inverted range preflight cannot iterate");
    expectTrue(planePreflight.exceedsBudget, "2D over-budget preflight flags exceed");
    expectTrue(!planePreflight.can_iterate(), "2D over-budget preflight cannot iterate");
    expectTrue(fuse::physics::broadphase::should_skip_refine_broadphase_pairs(bodies, shapes, buffer),
               "should_skip_refine on empty buffer");
    const auto refinePreflight =
    expectTrue(!refinePreflight.skipped, "refine preflight does not skip valid pair");
    expectTrue(refinePreflight.can_refine(), "refine preflight can refine valid pair");
    expectEq(refinePreflight.pairCount, 1u, "refine preflight reports pair count");
    expectEq(refinePreflight.bodyCount, 2u, "refine preflight reports body count");
    expectEq(refinePreflight.shapeCount, 2u, "refine preflight reports shape count");
    expectTrue(!fuse::physics::broadphase::should_skip_refine_broadphase_pairs(bodies, shapes, buffer),
               "should_skip_refine on valid pair buffer");
void testPairBufferDedupePreflightGuards() {
    const auto emptyPreflight = fuse::physics::broadphase::preflight_pair_buffer_dedupe(buffer);
    expectTrue(!emptyPreflight.can_dedupe(), "empty buffer cannot dedupe");
    expectTrue(fuse::physics::broadphase::should_skip_pair_buffer_dedupe(buffer),
               "should_skip_pair_buffer_dedupe on empty buffer");
    const auto singlePreflight = fuse::physics::broadphase::preflight_pair_buffer_dedupe(buffer);
    expectTrue(singlePreflight.skipped, "dedupe preflight skips single-pair buffer");
    expectTrue(!singlePreflight.can_dedupe(), "single-pair buffer cannot dedupe");
               "should_skip_pair_buffer_dedupe on single-pair buffer");
    const auto multiPreflight = fuse::physics::broadphase::preflight_pair_buffer_dedupe(buffer);
    expectTrue(!multiPreflight.skipped, "dedupe preflight does not skip multi-pair buffer");
    expectEq(multiPreflight.activeCount, 2u, "dedupe preflight reports active count");
    expectTrue(!fuse::physics::broadphase::should_skip_pair_buffer_dedupe(buffer),
               "should_skip_pair_buffer_dedupe on multi-pair buffer");
void testPairBufferCanSkipRefineGuard() {
    testPairBufferDedupePreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-e914 ---
    const auto smallPreflight = fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 8u);
    expectEq(smallPreflight.cellCount, 8u, "preflight counts cells in small 3D range");
    expectTrue(!smallPreflight.isEmptyRange, "preflight marks non-empty range");
    expectTrue(!smallPreflight.exceedsBudget, "preflight within budget does not exceed");
    expectTrue(smallPreflight.can_populate_cells(), "preflight allows cell population within budget");
    const auto overBudgetPreflight = fuse::physics::broadphase::preflight_cell_occupancy(smallRange, 4u);
    expectTrue(overBudgetPreflight.exceedsBudget, "preflight flags budget overflow");
    expectTrue(!overBudgetPreflight.can_populate_cells(), "preflight blocks population when over budget");
    const auto emptyPreflight = fuse::physics::broadphase::preflight_cell_occupancy(inverted, 4u);
    expectTrue(emptyPreflight.isEmptyRange, "preflight marks inverted range empty");
    expectEq(emptyPreflight.cellCount, 0u, "preflight empty range has zero cells");
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_population(inverted, 4u),
               "should_skip_shape_cell_population on empty range");
    const auto planePreflight = fuse::physics::broadphase::preflight_cell_occupancy(planeRange, 8u);
    expectEq(planePreflight.cellCount, 8u, "preflight counts cells in 2D range");
    const auto emptyPreflight = fuse::physics::broadphase::preflight_dedupe_pairs(buffer);
    expectEq(emptyPreflight.validPairCount, 0u, "empty buffer has zero valid pairs");
    const auto singlePreflight = fuse::physics::broadphase::preflight_dedupe_pairs(buffer);
    expectEq(singlePreflight.validPairCount, 1u, "single-pair buffer counts one valid pair");
    const auto duplicatePreflight = fuse::physics::broadphase::preflight_dedupe_pairs(buffer);
    expectTrue(!duplicatePreflight.skipped, "dedupe preflight runs on duplicate pairs");
    expectTrue(duplicatePreflight.can_dedupe(), "duplicate buffer can dedupe");
    expectEq(duplicatePreflight.validPairCount, 2u, "duplicate buffer counts both slots before dedupe");
    const auto emptyScenePreflight =
    expectTrue(emptyScenePreflight.skipped, "refine preflight skips empty scene");
    expectTrue(emptyScenePreflight.emptyInput, "refine preflight marks empty input");
               "should_skip_refine on empty scene");
    expectTrue(emptyBufferPreflight.skipped, "refine preflight skips empty buffer with valid input");
    expectTrue(emptyBufferPreflight.emptyBuffer, "refine preflight marks empty buffer");
    expectTrue(!validPreflight.skipped, "refine preflight allows populated buffer");
    expectTrue(validPreflight.can_refine(), "valid buffer can refine");
               "should_skip_refine false when buffer has pairs");

// --- deepen additive from deepen-b4-broadphase-guards-f048 ---
void testPairBufferCompactAndClampZeroGuard() {
    expectTrue(!emptyPreflight.can_insert(), "preflight rejects empty range");
    expectTrue(emptyPreflight.emptyRange, "preflight marks empty range");
void testShouldSkipShapeCellInsertionGuards() {
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insertion(inverted, 4u),
               "should_skip rejects empty 3D range");
    expectTrue(!fuse::physics::broadphase::should_skip_shape_cell_insertion(unitRange, 4u),
               "should_skip allows clamped in-budget 3D range");
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insertion(planeRange, 2u),
               "should_skip rejects over-budget 2D range");
    expectTrue(fuse::physics::broadphase::should_skip_broadphase_refine(bodies, shapes, buffer),
               "should_skip matches empty scene refine preflight");
    const auto validPreflight = fuse::physics::broadphase::preflight_broadphase_refine(bodies, shapes, buffer);
    expectTrue(!validPreflight.skipped, "refine preflight does not skip valid scene and buffer");
    expectTrue(validPreflight.can_refine(), "refine preflight can refine valid input");
    expectTrue(!fuse::physics::broadphase::should_skip_broadphase_refine(bodies, shapes, buffer),
               "should_skip allows valid refine input");

// --- deepen additive from deepen-b4-broadphase-preflights-4247 ---
    expectTrue(emptyPreflight.emptyBodies, "empty scene reports empty bodies");
    expectTrue(emptyPreflight.emptyShapes, "empty scene reports empty shapes");
    expectTrue(!emptyPreflight.can_build(), "empty scene cannot build broadphase");
    expectTrue(fuse::physics::broadphase::should_skip_broadphase_build(bodies, shapes),
               "should_skip_broadphase_build on empty scene");
    expectTrue(validPreflight.can_build(), "populated scene can build broadphase");
    expectTrue(!fuse::physics::broadphase::should_skip_broadphase_build(bodies, shapes),
               "should_skip_broadphase_build on populated scene");
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insert(inverted),
               "should_skip_shape_cell_insert on empty range");
    expectTrue(planePreflight.exceedsBudget, "2D preflight flags over-budget occupancy");
    const fuse::physics::broadphase::PairBufferPreflight emptyPreflight =
    expectTrue(emptyPreflight.skipped, "empty buffer preflight is skipped");
    expectTrue(emptyPreflight.skipDedupe, "empty buffer skips dedupe");
    expectTrue(emptyPreflight.skipCompaction, "empty buffer skips compaction");
    expectTrue(fuse::physics::broadphase::should_skip_pair_buffer_compaction(buffer),
               "should_skip_pair_buffer_compaction on empty buffer");
    const fuse::physics::broadphase::PairBufferPreflight partialPreflight =
    expectTrue(!partialPreflight.skipped, "partial buffer preflight is not skipped");
    expectTrue(partialPreflight.can_push(1u), "partial buffer can push one more pair");
    expectTrue(!partialPreflight.full, "partial buffer is not full");
    expectEq(partialPreflight.remainingCapacity, 1u, "preflight reports remaining capacity");
    const fuse::physics::broadphase::PairBufferPreflight fullPreflight =
    expectTrue(fullPreflight.full, "full buffer preflight reports full");
    expectTrue(!fullPreflight.can_push(1u), "full buffer preflight rejects push");
    expectTrue(!fullPreflight.skipDedupe, "two-pair buffer does not skip dedupe");
    const fuse::physics::broadphase::PairBufferPreflight singlePreflight =
    expectTrue(singlePreflight.skipDedupe, "single canonical pair skips dedupe");
    const fuse::physics::broadphase::PairBufferPreflight clampPreflight =
    expectTrue(clampPreflight.needsClamp, "overflow buffer preflight needs clamp");
    expectTrue(emptyPreflight.emptyInput, "refine preflight reports empty input");
    expectTrue(emptyPreflight.emptyBuffer, "refine preflight reports empty buffer");
    expectTrue(!validPreflight.emptyInput, "refine preflight has populated input");
    expectTrue(!validPreflight.emptyBuffer, "refine preflight has valid buffer");
               "should_skip_refine_broadphase on valid scene");
void testPairBufferCompactionPreflightGuards() {
    const fuse::physics::broadphase::PairBufferPreflight compactPreflight =
    expectTrue(compactPreflight.skipCompaction, "all-valid slots skip compaction in preflight");
               "should_skip_pair_buffer_compaction on all-valid slots");
    const fuse::physics::broadphase::PairBufferPreflight needsCompactPreflight =
    expectTrue(!needsCompactPreflight.skipCompaction, "invalid slot needs compaction");
    expectTrue(!fuse::physics::broadphase::should_skip_pair_buffer_compaction(buffer),
               "should_skip_pair_buffer_compaction false when invalid slots exist");
    testPairBufferCompactionPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-preflight-guards-3caf ---
    expectTrue(emptyPreflight.skipped, "preflight skips empty scene");
    expectTrue(!emptyPreflight.can_dispatch(), "preflight cannot dispatch empty scene");
    const auto singletonPreflight = fuse::physics::broadphase::preflight_broadphase(bodies, shapes);
    expectTrue(singletonPreflight.skipped, "preflight skips singleton scene");
    expectTrue(!validPreflight.skipped, "preflight does not skip populated scene");
    expectTrue(validPreflight.can_dispatch(), "preflight can dispatch populated scene");
    expectTrue(emptyPreflight.skipped, "refine preflight skips empty buffer");
    expectTrue(!emptyPreflight.can_refine(), "refine preflight cannot refine empty buffer");
               "should_skip_refine_broadphase on empty buffer");
    const auto singletonPreflight =
    expectTrue(singletonPreflight.skippedBroadphase, "refine preflight skips singleton broadphase");
    expectTrue(singletonPreflight.skipped, "refine preflight skips singleton scene");
    const auto emptyPreflight = fuse::physics::broadphase::preflight_dedupe_broadphase(buffer);
    expectTrue(!emptyPreflight.can_dedupe(), "dedupe preflight cannot dedupe empty buffer");
    expectTrue(fuse::physics::broadphase::should_skip_dedupe_broadphase(buffer),
               "should_skip_dedupe_broadphase on empty buffer");
    const auto singlePreflight = fuse::physics::broadphase::preflight_dedupe_broadphase(buffer);
    const auto sortedPreflight = fuse::physics::broadphase::preflight_dedupe_broadphase(buffer);
    expectTrue(sortedPreflight.skipped, "dedupe preflight skips sorted unique pairs");
    const auto duplicatePreflight = fuse::physics::broadphase::preflight_dedupe_broadphase(duplicateBuffer);
    expectTrue(!duplicatePreflight.skipped, "dedupe preflight does not skip duplicate pairs");
    expectTrue(duplicatePreflight.can_dedupe(), "dedupe preflight can dedupe duplicate pairs");
    expectTrue(!fuse::physics::broadphase::should_skip_shape_cell_insertion(smallRange, 8u),
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insertion(smallRange, 7u),
    expectTrue(emptyPreflight.skipped, "preflight skips empty range");
    expectEq(emptyPreflight.budgetRemaining, 4u, "empty range leaves full budget");
    const auto planePreflight = fuse::physics::broadphase::preflight_cell_occupancy_2d(planeRange, 4u);
    expectTrue(planePreflight.exceedsBudget, "2D preflight flags over-budget range");
    expectTrue(fuse::physics::broadphase::should_skip_shape_cell_insertion_2d(planeRange, 4u),

// --- deepen additive from deepen-b4-broadphase-guards-4311 ---
void testCellOccupancyPreflightReasonGuards() {
                 fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseMergeRejectReasonName(
    expectTrue(fuse::physics::broadphase::broadphaseMergeRejectsForReason(
                 fuse::physics::broadphase::pairBufferPushRejectReason(buffer, 1u, 1u)),
                   buffer, 1u, 1u, fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
                   buffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::EmptyBuffer),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(sparseBuffer)),
    expectTrue(fuse::physics::broadphase::preflightPairBufferCompaction(sparseBuffer).needsCompaction(),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(overflowBuffer)),
    expectTrue(fuse::physics::broadphase::preflightPairBufferClamp(overflowBuffer).needsClamp(),
void testPairBufferDedupeSortRejectReasonGuards() {
void testShouldRunBroadphaseAndRefineGuards() {
    testCellOccupancyPreflightReasonGuards();
    testPairBufferDedupeSortRejectReasonGuards();

// --- deepen additive from deepen-b4-broadphase-guards-f2c7 ---
void testCellOccupancyPreflightReasonField() {
                   buffer, 2u, 2u, fuse::physics::broadphase::PairBufferPushRejectReason::InvalidPair),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactionRejectReasonName(
    const fuse::physics::broadphase::PairBufferCompactionPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferCompaction(buffer);
    expectTrue(fuse::physics::broadphase::pairBufferClampRejectsForReason(
                   buffer, fuse::physics::broadphase::PairBufferClampRejectReason::EmptyBuffer),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferClampRejectReasonName(
    const fuse::physics::broadphase::PairBufferClampPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferClamp(overflowBuffer);
    testCellOccupancyPreflightReasonField();

// --- deepen additive from deepen-b4-broadphase-guards-8d3f ---
void testShouldRunRefineBroadphaseGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseMergeRejectReason(bodies, shapes)),
                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::EmptyDynamicBodies),
void testPairBufferRejectReasonGuards() {
                   buffer, 2u, 3u, fuse::physics::broadphase::PairBufferPushRejectReason::AtCapacity),
    const fuse::physics::broadphase::PairBufferPushPreflight pushPreflight =
    expectEq(static_cast<fuse::u32>(pushPreflight.reason),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactionRejectReason(slotBuffer)),
                   slotBuffer, fuse::physics::broadphase::PairBufferCompactionRejectReason::AllValid),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(clampBuffer)),
    expectTrue(fuse::physics::broadphase::pairBufferDedupeRejectsForReason(
                   dedupeBuffer, fuse::physics::broadphase::PairBufferDedupeRejectReason::EmptyBuffer),
    testPairBufferRejectReasonGuards();

// --- deepen additive from deepen-b4-broadphase-guards-bd20 ---
void testPairBufferSoADedupePassGuards() {
                   overflowBuffer, fuse::physics::broadphase::PairBufferClampRejectReason::None),

// --- deepen additive from deepen-b4-broadphase-guards-c0a6 ---
             "preflightBroadphaseMerge carries merge reject reason");
void testCellOccupancyCanSkipIterationGuards() {
             "preflightCellOccupancy carries occupancy reject reason");
    expectTrue(!pushPreflight.canPush(), "push preflight rejects at-capacity pair");
    expectTrue(fuse::physics::broadphase::preflightPairBufferCompaction(slotBuffer).needsCompaction(),
    expectTrue(fuse::physics::broadphase::preflightPairBufferClamp(clampBuffer).needsClamp(),
                   buffer, fuse::physics::broadphase::PairBufferDedupeRejectReason::SinglePair),
                 fuse::physics::broadphase::pairBufferDedupeRejectReason(dedupeBuffer)),
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight emptyPreflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(buffer);
    expectTrue(emptyPreflight.canSkipAll(), "empty buffer skips compact-and-clamp");
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight compactionOnly =
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight bothNeeded =

// --- deepen additive from deepen-b4-broadphase-guards-9b1a ---
void testMergeBroadphaseRejectReasonGuards() {
             static_cast<fuse::u32>(fuse::physics::broadphase::MergeBroadphaseRejectReason::EmptyPlaneBodies),
                   fuse::physics::broadphase::MergeBroadphaseRejectReason::EmptyPlaneBodies),
                               fuse::physics::broadphase::MergeBroadphaseRejectReason::EmptyDynamicBodies),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergeBroadphaseRejectReason::EmptyDynamicBodies),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergeBroadphaseRejectReason::None),
void testPairBufferSortAndCompactionSkipGuards() {
    expectTrue(emptyPreflight.canSkip(), "empty buffer compact-and-clamp preflight can skip");
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight compactionPreflight =
    expectTrue(compactionPreflight.needsCompaction, "slot buffer needs compaction");
    expectTrue(!compactionPreflight.needsClamp, "slot buffer without max capacity skips clamp");
    const fuse::physics::broadphase::PairBufferClampPreflight overflowClampPreflight =
    expectTrue(overflowClampPreflight.needsClamp(), "overflow buffer needs post-compact clamp");
    const fuse::physics::broadphase::CellOccupancyPreflight emptyRangePreflight =
    expectEq(static_cast<fuse::u32>(emptyRangePreflight.reason),
    testMergeBroadphaseRejectReasonGuards();

// --- deepen additive from deepen-b4-broadphase-guards-a79d ---
             "preflightBroadphaseMerge carries reject reason");
void testPairBufferCompactionClampRejectReasonGuards() {
    expectTrue(compactionPreflight.needsCompaction(), "compaction preflight requests invalid slot work");
    expectEq(static_cast<fuse::u32>(compactionPreflight.reason),
                   clampBuffer, fuse::physics::broadphase::PairBufferClampRejectReason::WithinCapacity),
void testPairBufferSkipPredicateGuards() {
    const fuse::physics::broadphase::CellOccupancyPreflight validPreflight =
    expectEq(static_cast<fuse::u32>(validPreflight.reason),
    expectTrue(validPreflight.canIterate(), "valid range preflight can iterate");
    expectTrue(!fuse::physics::broadphase::canSkipCellOccupancyIteration(validPreflight),
    const fuse::physics::broadphase::CellOccupancyPreflight emptyPreflight =
    expectEq(static_cast<fuse::u32>(emptyPreflight.reason),
    expectTrue(fuse::physics::broadphase::canSkipCellOccupancyIteration(emptyPreflight),
    const fuse::physics::broadphase::CellOccupancyPreflight overBudgetPreflight =
    expectEq(static_cast<fuse::u32>(overBudgetPreflight.reason),
    expectTrue(overBudgetPreflight.exceedsBudget, "over-budget preflight marks exceedsBudget");
    testPairBufferCompactionClampRejectReasonGuards();

// --- deepen additive from deepen-b4-broadphase-guards-7162 ---
void testCellSpanClampPreflightGuards() {
                 fuse::physics::broadphase::cellSpanRejectReason(wideRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanClamp),
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanClamp),
        fuse::physics::broadphase::preflightCellSpanClamp(smallRange, 8u);
    expectTrue(spanPreflight.canIterate(), "small span preflight accepts range");
    expectTrue(!spanPreflight.exceedsSpanClamp, "small span preflight does not exceed clamp");
    expectEq(spanPreflight.spanPerAxis.x, 4, "span preflight reports per-axis span");
                   inverted, 4u, fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
void testPairBufferWriteSlotAndPreparePreflights() {
    const fuse::physics::broadphase::PairBufferWriteSlotPreflight validWrite =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 0u, 1u);
    const fuse::physics::broadphase::PairBufferWriteSlotPreflight invalidPair =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 1u, 2u, 2u);
    const fuse::physics::broadphase::PairBufferWriteSlotPreflight outOfRange =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 4u, 0u, 2u);
    const fuse::physics::broadphase::PairBufferPrepareSlotsPreflight zeroSlots =
        fuse::physics::broadphase::preflightPairBufferPrepareSlots(0u);
void testPairBufferMergeAndDuplicateGuards() {
    const fuse::physics::broadphase::PairBufferMergePreflight atCapacity =
        fuse::physics::broadphase::preflightPairBufferMerge(buffer, 2u);
    const fuse::physics::broadphase::PairBufferMergePreflight partialAccept =
        fuse::physics::broadphase::preflightPairBufferMerge(openBuffer, 3u);
void testRefineDedupeBroadphasePreflightGuards() {
    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight emptyPreflight =
        fuse::physics::broadphase::preflightRefineDedupeBroadphase(bodies, shapes, buffer);
    expectTrue(!emptyPreflight.canRefine(), "combined preflight cannot refine empty scene");
    expectTrue(!emptyPreflight.needsDedupe(), "combined preflight does not need dedupe when empty");
    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight duplicatePreflight =
    expectTrue(duplicatePreflight.canRefine(), "combined preflight can refine valid scene");
    expectTrue(duplicatePreflight.hasDuplicatePairs, "combined preflight detects duplicate pairs");
    expectTrue(duplicatePreflight.needsDedupe(), "combined preflight needs dedupe when duplicates exist");
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeRejectReason::NoPlaneBodies),
                               fuse::physics::broadphase::BroadphaseMergeRejectReason::NoDynamicBodies),
                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::NoDynamicBodies),
    expectTrue(mergePreflight.canMerge(), "merge preflight accepts plane plus dynamic scene");
    expectEq(mergePreflight.estimatedMergePairs, 1u, "merge preflight estimates dynamic-plane pair count");
    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight mergeIntoBuffer =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer, 1u);
    testCellSpanClampPreflightGuards();
    testPairBufferWriteSlotAndPreparePreflights();
    testRefineDedupeBroadphasePreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-603e ---
    const fuse::physics::broadphase::PairBufferCompactionPreflight needsWork =
void testPairBufferSortAndDedupeSkipGuards() {

// --- deepen additive from b4-broadphase-deepen-preflights-5a84 ---
    expectEq(static_cast<fuse::u32>(planeOnlyPreflight.reason),
    expectEq(static_cast<fuse::u32>(mergePreflight.reason),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferClampRejectReason(withinBuffer)),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferDedupeRejectReason(dedupeBuffer)),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(dedupeBuffer)),
    expectTrue(fuse::physics::broadphase::pairBufferSortRejectsForReason(
                   dedupeBuffer, fuse::physics::broadphase::PairBufferSortRejectReason::None),

// --- deepen additive from deepen-b4-broadphase-guards-9072 ---
void testPairBufferWriteSlotPreflightGuards() {
    const fuse::physics::broadphase::PairBufferWriteSlotPreflight invalidSlot =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 4u, 0u, 1u);
    const fuse::physics::broadphase::PairBufferWriteSlotPreflight selfPair =
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 0u, 5u, 2u);
void testPairBufferMergePreflightGuards() {
    const fuse::physics::broadphase::PairBufferMergePreflight emptyIncoming =
        fuse::physics::broadphase::preflightPairBufferMerge(buffer, 0u);
    const fuse::physics::broadphase::PairBufferMergePreflight partialMerge =
        fuse::physics::broadphase::preflightPairBufferMerge(buffer, 1u);
void testPairBufferPushBodyCountPreflightGuards() {
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 1u, 2u);
    const fuse::physics::broadphase::PairBufferPushPreflight outOfRangePush =
        fuse::physics::broadphase::preflightPairBufferPush(buffer, 0u, 2u, 2u);
void testRefineBroadphaseEmptyInputRejectReason() {
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyInput),
                               fuse::physics::broadphase::RefineBroadphaseRejectReason::EmptyInput),
void testRefineBroadphaseNoValidPairsRejectReason() {
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::NoValidPairs),
    expectTrue(mergePreflight.canMerge(), "sphere-over-plane scene passes merge preflight");
    testPairBufferWriteSlotPreflightGuards();
    testPairBufferMergePreflightGuards();
    testPairBufferPushBodyCountPreflightGuards();
    testRefineBroadphaseEmptyInputRejectReason();
    testRefineBroadphaseNoValidPairsRejectReason();

// --- deepen additive from deepen-b4-broadphase-guards-f83b ---
void testRefineAndMergeRejectReasonGuards() {
    testRefineAndMergeRejectReasonGuards();

// --- deepen additive from b4-broadphase-deepen-guards-f2a3 ---
void testRefineBroadphaseShouldRunGuard() {
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferDedupeRejectReasonName(

// --- deepen additive from deepen-b4-broadphase-guards-cbb3 ---
void testCanSkipCellOccupancyIterationGuards() {
void testPairBufferRejectReasonAndSkipGuards() {
                 fuse::physics::broadphase::pairBufferCompactionRejectReason(compactionBuffer)),
                 fuse::physics::broadphase::pairBufferSortRejectReason(sortBuffer)),
    expectTrue(!emptyPreflight.needsWork(), "empty buffer compact-and-clamp preflight needs no work");
    expectTrue(compactionPreflight.compaction.needsCompaction(),
    expectTrue(!compactionPreflight.clamp.needsClamp(),
    expectTrue(compactionPreflight.needsWork(), "invalid slot buffer needs compact-and-clamp work");
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight clampPreflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(overflowBuffer);
    expectTrue(!clampPreflight.compaction.needsCompaction(),
    expectTrue(clampPreflight.clamp.needsClamp(),
    expectTrue(clampPreflight.needsWork(), "overflow buffer needs compact-and-clamp work");
    testPairBufferRejectReasonAndSkipGuards();

// --- deepen additive from deepen-b4-broadphase-guards-14d5 ---
    expectTrue(emptyPreflight.emptyBuffer, "empty buffer compact+clamp preflight marks empty");
    expectTrue(!emptyPreflight.needsWork(), "empty buffer compact+clamp preflight needs no work");
    expectTrue(compactionPreflight.needsCompaction, "invalid slot requests compaction work");
    expectTrue(compactionPreflight.needsWork(), "invalid slot compact+clamp preflight needs work");
    expectTrue(clampPreflight.needsClamp, "overflow buffer compact+clamp preflight needs clamp");
void testPairBufferSkipHelperGuards() {

// --- deepen additive from b4-broadphase-deepen-guards-727e ---
                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::EmptyBuffer),
void testShouldRunPairBufferDedupeAndSortGuards() {
    const fuse::physics::broadphase::PairSlotPreflight zeroSlots =
    const fuse::physics::broadphase::PairSlotPreflight withinCapacity =
        fuse::physics::broadphase::preflightPairSlots(2u, buffer);
    const fuse::physics::broadphase::PairSlotPreflight exceedsCapacity =
        fuse::physics::broadphase::preflightPairSlots(4u, buffer);
                 fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWorkNeeded),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactAndClampRejectReasonName(

// --- deepen additive from deepen-b4-broadphase-guards-03cf ---
                                       PairBufferCompactAndClampRejectReason::AlreadyCompactAndWithinCapacity),
                 fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(slotBuffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::None),
void testPairBufferDedupeShouldRunGuards() {
void testBroadphaseShouldRunGuards() {

// --- deepen additive from deepen-b4-broadphase-guards-b64e ---
void testPairBufferSlotWritePreflightGuards() {
    const fuse::physics::broadphase::PairBufferSlotWritePreflight validWrite =
        fuse::physics::broadphase::preflightPairBufferSlotWrite(buffer, 0u, 0u, 1u);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSlotWriteRejectReason::None),
    const fuse::physics::broadphase::PairBufferSlotWritePreflight outOfRange =
        fuse::physics::broadphase::preflightPairBufferSlotWrite(buffer, 3u, 0u, 1u);
    expectTrue(fuse::physics::broadphase::pairBufferSlotWriteRejectsForReason(
                   fuse::physics::broadphase::PairBufferSlotWriteRejectReason::OutOfRangeSlot),
    const fuse::physics::broadphase::PairBufferSlotWritePreflight invalidPair =
        fuse::physics::broadphase::preflightPairBufferSlotWrite(buffer, 1u, 2u, 2u);
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSlotWriteRejectReasonName(
                               fuse::physics::broadphase::PairBufferSlotWriteRejectReason::InvalidPair),
void testPairBufferSlotReservationPreflightGuards() {
    const fuse::physics::broadphase::PairBufferSlotReservationPreflight zeroSlots =
        fuse::physics::broadphase::preflightPairBufferSlotReservation(buffer, 0u);
    const fuse::physics::broadphase::PairBufferSlotReservationPreflight validReservation =
        fuse::physics::broadphase::preflightPairBufferSlotReservation(buffer, 4u);
    const fuse::physics::broadphase::PairBufferSlotReservationPreflight exceedsCapacity =
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferSlotReservationRejectReasonName(
                               fuse::physics::broadphase::PairBufferSlotReservationRejectReason::ExceedsCapacity),
void testPairBufferSortSkipGuards() {
void testCellSpanPreflightGuards() {
    const fuse::physics::broadphase::CellSpanPreflight validPreflight =
        fuse::physics::broadphase::preflightCellSpan(validRange, 4u);
    expectTrue(validPreflight.canClamp(), "cell-span preflight accepts clampable range");
    const fuse::physics::broadphase::CellSpanPreflight widePreflight =
        fuse::physics::broadphase::preflightCellSpan(wideRange, 4u);
    expectTrue(widePreflight.exceedsMaxSpan, "cell-span preflight marks exceeds max span");
    expectTrue(!widePreflight.canClamp(), "cell-span preflight rejects over-span range");
                   wideRange, 4u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsMaxSpan),
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsMaxSpan),
    const fuse::physics::broadphase::CellSpanPreflight2D planePreflight =
        fuse::physics::broadphase::preflightCellSpan(planeRange, 4u);
    expectTrue(planePreflight.exceedsMaxSpan, "2D cell-span preflight marks exceeds max span");
void testRefineDedupeMergePreflightCounts() {
    const fuse::physics::broadphase::RefineBroadphasePreflight emptyRefine =
    const fuse::physics::broadphase::RefineBroadphasePreflight slotRefine =
    const fuse::physics::broadphase::DedupeBroadphasePreflight dedupePreflight =
    expectEq(dedupePreflight.pairCount, 2u, "dedupe preflight reports pair count");
    expectEq(mergePreflight.planeBodyCount, 1u, "merge preflight reports plane body count");
    expectTrue(mergePreflight.dynamicBodyCount >= 2u, "merge preflight reports dynamic body count");
             mergePreflight.planeBodyCount * mergePreflight.dynamicBodyCount,
    testPairBufferSlotWritePreflightGuards();
    testPairBufferSlotReservationPreflightGuards();
    testCellSpanPreflightGuards();
    testRefineDedupeMergePreflightCounts();

// --- deepen additive from deepen-b4-broadphase-guards-c372 ---
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 1u, 1u);
void testBroadphaseMergeBufferPreflightGuards() {
    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight emptyPreflight =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer);
    expectTrue(!emptyPreflight.canMergeIntoBuffer(), "empty scene cannot merge into buffer");
    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight mergeablePreflight =
    expectTrue(mergeablePreflight.canMergeIntoBuffer(), "mergeable scene can merge into open buffer");
    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight fullPreflight =
    expectTrue(fullPreflight.bufferFull, "full buffer marks bufferFull in merge preflight");
    expectTrue(!fullPreflight.canMergeIntoBuffer(), "full buffer cannot accept merge pairs");
void testRefinableBroadphasePairCountGuards() {
    testBroadphaseMergeBufferPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-1f79 ---
void testPairBufferShouldRunDedupeAndSortGuards() {
void testCellOccupancyPreflightBudgetRemaining() {
    const fuse::physics::broadphase::CellOccupancyPreflight headroom =
        fuse::physics::broadphase::preflightCellOccupancy(validRange, 12u);
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 6u);
    expectEq(planePreflight.budgetRemaining, 4u, "2D preflight reports budget remaining");
void testRefineBroadphasePreflightValidPairCount() {
    expectEq(emptyPreflight.validPairCount, 0u, "empty refine preflight reports zero valid pairs");
    expectEq(validPreflight.validPairCount, 1u, "valid refine preflight reports active pair count");
    expectTrue(validPreflight.canRefine(), "valid refine preflight can refine");
void testBroadphaseMergePreflightBodyCounts() {
    expectEq(emptyPreflight.planeBodyCount, 0u, "empty scene reports zero plane bodies");
    expectEq(emptyPreflight.dynamicBodyCount, 0u, "empty scene reports zero dynamic bodies");
    expectEq(planeOnlyPreflight.planeBodyCount, 1u, "plane-only scene reports one plane body");
    expectEq(planeOnlyPreflight.dynamicBodyCount, 0u, "plane-only scene reports zero dynamic bodies");
    expectEq(mergePreflight.planeBodyCount, 1u, "merge scene reports plane body count");
    expectEq(mergePreflight.dynamicBodyCount, 1u, "merge scene reports dynamic body count");
    expectTrue(mergePreflight.canMerge(), "merge scene can merge with populated counts");
    testCellOccupancyPreflightBudgetRemaining();
    testRefineBroadphasePreflightValidPairCount();
    testBroadphaseMergePreflightBodyCounts();

// --- deepen additive from deepen-b4-broadphase-guards-1d11 ---
void testPairBufferCompactAndClampRejectReasonGuards() {
void testShouldRunPairBufferDedupeGuards() {
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight emptyPreflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(emptyPairs, buffer);
    expectTrue(!emptyPreflight.canMerge(), "empty pairs cannot merge into buffer");
    expectTrue(emptyPreflight.emptyPairs, "merge preflight marks empty pairs");
    expectTrue(validPreflight.canMerge(), "valid pairs can merge into empty buffer");
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight fullPreflight =
    expectTrue(!fullPreflight.canMerge(), "merge preflight rejects full buffer");
    expectTrue(fullPreflight.bufferFull, "merge preflight marks buffer full");
void testMergePairsIntoBufferRejectReasonGuards() {
    testPairBufferCompactAndClampRejectReasonGuards();
    testMergePairsIntoBufferRejectReasonGuards();

// --- deepen additive from deepen-b4-broadphase-guards-b316 ---
                   buffer, fuse::physics::broadphase::PairBufferSortRejectReason::SinglePair),
                 fuse::physics::broadphase::cellSpanRejectReason(validRange, 4u)),
                 fuse::physics::broadphase::cellSpanRejectReason(compactRange, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::WithinSpanLimit),
                 fuse::physics::broadphase::cellSpanRejectReason(compactRange, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::Unbounded),
                               fuse::physics::broadphase::CellSpanRejectReason::WithinSpanLimit),
    const fuse::physics::broadphase::CellSpanPreflight preflight =
    expectTrue(emptyPreflight.sceneNotMergeable, "empty scene merge buffer preflight marks scene not mergeable");
    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight mergePreflight =
    expectTrue(mergePreflight.canMergeIntoBuffer(), "mergeable scene with room can merge into buffer");
    expectTrue(fullPreflight.bufferAtCapacity, "full buffer merge preflight marks at capacity");
    expectEq(static_cast<fuse::u32>(fullPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::BufferAtCapacity),
    expectTrue(fuse::physics::broadphase::mergeBroadphaseBufferRejectsForReason(
                   fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::BufferAtCapacity),
    expectTrue(std::strcmp(fuse::physics::broadphase::mergeBroadphaseBufferRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::SceneNotMergeable),

// --- deepen additive from b4-broadphase-deepen-a14d ---
void testPairBufferSortAndDedupeShouldRunGuards() {
                 fuse::physics::broadphase::MergePairsIntoBufferRejectReason::BufferAtCapacity),
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight preflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, mergeBuffer);
void testShouldRunBroadphaseGuard() {

// --- deepen additive from deepen-b4-broadphase-guards-1f94 ---
void testBroadphaseMergePreflightHasBodiesFields() {
    expectTrue(!emptyPreflight.hasPlaneBodies, "empty scene has no plane bodies");
    expectTrue(!emptyPreflight.hasDynamicBodies, "empty scene has no dynamic bodies");
    expectTrue(planeOnlyPreflight.hasPlaneBodies, "plane-only scene marks hasPlaneBodies");
    expectTrue(!planeOnlyPreflight.hasDynamicBodies, "plane-only scene has no dynamic bodies");
    expectTrue(mergePreflight.hasPlaneBodies, "merge scene marks hasPlaneBodies");
    expectTrue(mergePreflight.hasDynamicBodies, "merge scene marks hasDynamicBodies");
    expectTrue(mergePreflight.canMerge(), "merge scene can merge with positive body flags");
    testBroadphaseMergePreflightHasBodiesFields();

// --- deepen additive from deepen-b4-broadphase-guards-b86e ---
                   buffer, 4u, 0u, 1u, fuse::physics::broadphase::PairBufferWriteSlotRejectReason::OutOfRangeSlot),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactAndClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWork),
    const fuse::physics::broadphase::PairBufferCompactAndClampPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferCompactAndClamp(workBuffer);
void testBroadphaseCellPairPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellPairRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroSlots),
    expectTrue(fuse::physics::broadphase::broadphaseCellPairRejectsForReason(
                   0u, fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroSlots),
    const fuse::physics::broadphase::BroadphaseCellPairPreflight zeroPreflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairs(0u);
    expectTrue(zeroPreflight.zeroSlots, "cell-pair preflight marks zero slots");
    expectTrue(!zeroPreflight.canGenerate(), "cell-pair preflight cannot generate zero slots");
    const fuse::physics::broadphase::BroadphaseCellPairPreflight validPreflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairs(4u);
    expectTrue(validPreflight.canGenerate(), "cell-pair preflight accepts non-zero slots");
    expectEq(validPreflight.totalCellSlots, 4u, "cell-pair preflight reports slot count");
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseCellPairRejectReasonName(
                               fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroSlots),
    testBroadphaseCellPairPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-3ca3 ---
                   buffer, 0u, 0u, 1u, fuse::physics::broadphase::PairBufferWriteSlotRejectReason::None),
                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 4u, 0u, 1u)),
    const fuse::physics::broadphase::PairBufferWriteSlotPreflight preflight =

// --- deepen additive from b4-broadphase-deepen-guards-5209 ---
void testPerShapeCellBudgetGuards() {

// --- deepen additive from deepen-b4-broadphase-guards-f56d ---
                   wideRange, 8u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsMaxSpan),
    const fuse::physics::broadphase::CellSpanPreflight clampPreflight =
        fuse::physics::broadphase::preflightCellSpan(wideRange, 8u);
    expectTrue(clampPreflight.needsClamp(), "span preflight requests clamp for wide range");
    expectTrue(clampPreflight.exceedsMaxSpan, "span preflight marks exceedsMaxSpan");

// --- deepen additive from b4-broadphase-deepen-guards-c4fe ---
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::EmptyBuffer),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactAndClampRejectReason::NoWorkNeeded),

// --- deepen additive from deepen-b4-broadphase-guards-c567 ---
void testBroadphaseCountValidPairsGuards() {
void testCellOccupancyPreflightCanSkipGuards() {
void testBroadphaseMergePreflightCounts() {
    const fuse::physics::broadphase::BroadphaseMergePreflight planeOnly =
    const fuse::physics::broadphase::BroadphaseMergePreflight mergeable =
    testCellOccupancyPreflightCanSkipGuards();
    testBroadphaseMergePreflightCounts();

// --- deepen additive from b4-broadphase-deepen-guards-ca26 ---
void testPairBufferPushSkipGuards() {
    expectTrue(!emptyPreflight.canDedupe(), "combined preflight cannot dedupe empty buffer");
    expectTrue(!emptyPreflight.canRefineDedupe(), "combined preflight cannot refine+dedupe empty scene");
    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight singlePreflight =
    expectTrue(singlePreflight.canRefine(), "combined preflight can refine valid scene");
    expectTrue(!singlePreflight.canDedupe(), "combined preflight cannot dedupe single pair");
    expectTrue(!singlePreflight.canRefineDedupe(), "combined preflight cannot refine+dedupe single pair");
    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight multiPreflight =
    expectTrue(multiPreflight.canRefineDedupe(), "combined preflight can refine+dedupe multiple pairs");
void testBroadphaseMergeLaunchPreflightGuards() {
    const fuse::physics::broadphase::BroadphaseMergeLaunchPreflight emptyLaunch =
        fuse::physics::broadphase::preflightBroadphaseMergeLaunch(bodies, shapes);
    const fuse::physics::broadphase::BroadphaseMergeLaunchPreflight planeOnlyLaunch =
    const fuse::physics::broadphase::BroadphaseMergeLaunchPreflight launchPreflight =
    expectTrue(launchPreflight.canRunBroadphase(), "populated scene can run broadphase");
    expectTrue(launchPreflight.canMerge(), "plane plus dynamic scene can merge");
    expectTrue(launchPreflight.canLaunchMerge(), "merge launch preflight can launch mergeable scene");
    testBroadphaseMergeLaunchPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-345c ---
               "shouldRunBroadphase mirrors preflightBroadphase.canRun");
    const fuse::physics::broadphase::CellSpanClampPreflight unlimitedPreflight =
        fuse::physics::broadphase::preflightCellSpanClamp(unitRange, 0u);
    expectTrue(unlimitedPreflight.unlimitedSpan, "preflight marks unlimited span clamp");
    const fuse::physics::broadphase::CellSpanClampPreflight emptyPreflight =
        fuse::physics::broadphase::preflightCellSpanClamp(inverted, 4u);
    expectTrue(emptyPreflight.emptyRange, "preflight marks empty range for span clamp");
void testCellSpanClampRejectReasonGuards() {
                 fuse::physics::broadphase::cellSpanClampRejectReason(wideRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::None),
    expectTrue(fuse::physics::broadphase::cellSpanClampRejectsForReason(
                   wideRange, 8u, fuse::physics::broadphase::CellSpanClampRejectReason::None),
                 fuse::physics::broadphase::cellSpanClampRejectReason(unitRange, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::WithinSpanLimit),
    expectTrue(std::strcmp(fuse::physics::broadphase::cellSpanClampRejectReasonName(
                               fuse::physics::broadphase::CellSpanClampRejectReason::UnlimitedSpan),
                 fuse::physics::broadphase::cellSpanClampRejectReason(unitRange, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::UnlimitedSpan),
    testCellSpanClampRejectReasonGuards();

// --- deepen additive from deepen-b4-broadphase-guards-15cc ---
                 fuse::physics::broadphase::cellSpanClampRejectReason(validRange, 8u)),
                 fuse::physics::broadphase::cellSpanClampRejectReason(validRange, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::UnboundedSpan),
                               fuse::physics::broadphase::CellSpanClampRejectReason::EmptyRange),
                   inverted, 4u, fuse::physics::broadphase::CellSpanClampRejectReason::EmptyRange),
    const fuse::physics::broadphase::CellSpanClampPreflight preflight =
        fuse::physics::broadphase::preflightCellSpanClamp(validRange, 8u);
void testPairBufferSlotWriteRejectReasonGuards() {
                 fuse::physics::broadphase::pairBufferSlotWriteRejectReason(buffer, 0u, 1u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSlotWriteRejectReason::InvalidPair),
                 fuse::physics::broadphase::pairBufferSlotWriteRejectReason(buffer, 4u, 0u, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSlotWriteRejectReason::OutOfRangeSlot),
    const fuse::physics::broadphase::PairBufferSlotWritePreflight validPreflight =
    expectTrue(validPreflight.canWrite(), "slot-write preflight accepts valid pair");
void testPairBufferMergeIntoRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferMergeIntoRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferMergeIntoRejectReason::EmptyInput),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferMergeIntoRejectReason(buffer, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferMergeIntoRejectReason::BufferFull),
    const fuse::physics::broadphase::PairBufferMergeIntoPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferMergeInto(openBuffer, 2u);
void testBroadphaseMergeIntoBufferPreflightGuards() {
                 fuse::physics::broadphase::mergeIntoBufferBroadphaseRejectReason(bodies, shapes, buffer)),
                 fuse::physics::broadphase::BroadphaseMergeIntoBufferRejectReason::SceneNotMergeable),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeIntoBufferRejectReason::BufferFull),
    expectTrue(std::strcmp(fuse::physics::broadphase::mergeIntoBufferBroadphaseRejectReasonName(
                               fuse::physics::broadphase::BroadphaseMergeIntoBufferRejectReason::BufferFull),
    const fuse::physics::broadphase::BroadphaseMergeIntoBufferPreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseMergeIntoBuffer(bodies, shapes, openBuffer);
    testPairBufferSlotWriteRejectReasonGuards();
    testPairBufferMergeIntoRejectReasonGuards();
    testBroadphaseMergeIntoBufferPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-e578 ---
void testPairBufferCompactClampRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferCompactClampRejectReason(buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactClampRejectReason::EmptyBuffer),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferCompactClampRejectReason::NoWork),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferCompactClampRejectReasonName(
                               fuse::physics::broadphase::PairBufferCompactClampRejectReason::NoWork),
    const fuse::physics::broadphase::PairBufferCompactClampPreflight workPreflight =
        fuse::physics::broadphase::preflightPairBufferCompactClamp(workBuffer);
    expectTrue(workPreflight.canRun(), "compact-clamp preflight accepts compaction work");
    expectTrue(workPreflight.needsCompaction, "compact-clamp preflight marks compaction needed");
    expectEq(emptyPreflight.budgetRemaining, 4u, "empty range leaves full budget in preflight");
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 10u);
    expectEq(planePreflight.budgetRemaining, 2u, "2D preflight reports occupancy budget remaining");
void testRefineBroadphaseActivePairCountPreflight() {
    expectEq(emptyPreflight.activePairCount, 0u, "refine preflight reports zero active pairs on empty buffer");
    expectEq(validPreflight.activePairCount, 2u, "refine preflight reports active pair count");
    expectTrue(validPreflight.canRefine(), "refine preflight accepts valid scene with pair count");
void testBroadphaseMergeStatsPreflight() {
    expectEq(emptyPreflight.stats.planeBodyCount, 0u, "empty scene has zero plane bodies");
    expectEq(emptyPreflight.stats.dynamicBodyCount, 0u, "empty scene has zero dynamic bodies");
    expectEq(planeOnlyPreflight.stats.planeBodyCount, 1u, "plane-only scene reports one plane body");
    expectEq(planeOnlyPreflight.stats.dynamicBodyCount, 0u, "plane-only scene reports zero dynamic bodies");
    expectEq(mergePreflight.stats.planeBodyCount, 1u, "merge scene reports plane body count");
    expectEq(mergePreflight.stats.dynamicBodyCount, 1u, "merge scene reports dynamic body count");
    expectTrue(mergePreflight.canMerge(), "merge preflight accepts scene with plane and dynamic counts");
    testPairBufferCompactClampRejectReasonGuards();
    testRefineBroadphaseActivePairCountPreflight();
    testBroadphaseMergeStatsPreflight();

// --- deepen additive from deepen-b4-broadphase-guards-ce99 ---
    const fuse::physics::broadphase::CellOccupancyPreflight atBudget =
    const fuse::physics::broadphase::PairBufferWriteSlotPreflight invalidWrite =

// --- deepen additive from deepen-b4-broadphase-guards-a65f ---
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidSlot),
                               fuse::physics::broadphase::PairBufferWriteSlotRejectReason::InvalidSlot),
void testPairBufferMergeRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferMergeRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferMergeRejectReason::EmptyPairs),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferMergeRejectReason(buffer, 1u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferMergeRejectReason::AtCapacity),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferMergeRejectReasonName(
                               fuse::physics::broadphase::PairBufferMergeRejectReason::AtCapacity),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferMergeRejectReason(openBuffer, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferMergeRejectReason::None),
    const fuse::physics::broadphase::PairBufferMergePreflight preflight =
        fuse::physics::broadphase::preflightPairBufferMerge(openBuffer, 2u);
void testCellSpanRejectReasonGuards() {
                 fuse::physics::broadphase::cellSpanRejectReason(validRange, 2u)),
                   wideRange, 4u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpan),
        fuse::physics::broadphase::preflightCellSpan(validRange, 2u);
void testBroadphaseMergeBufferRejectReasonGuards() {
                 fuse::physics::broadphase::mergeBroadphaseBufferRejectReason(bodies, shapes, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::SceneRejected),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseMergeBufferRejectReason::None),
    const fuse::physics::broadphase::BroadphaseMergeBufferPreflight preflight =
    expectEq(static_cast<fuse::u32>(emptyPreflight.refineReason),
    expectEq(static_cast<fuse::u32>(emptyPreflight.dedupeReason),
    const fuse::physics::broadphase::RefineDedupeBroadphasePreflight validPreflight =
    expectTrue(validPreflight.canRefine(), "combined preflight can refine valid scene");
    expectTrue(validPreflight.canDedupe(), "combined preflight can dedupe multiple pairs");
    testPairBufferMergeRejectReasonGuards();
    testCellSpanRejectReasonGuards();
    testBroadphaseMergeBufferRejectReasonGuards();

// --- deepen additive from deepen-b4-broadphase-guards-ff66 ---
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 1u, 2u, 3u);
void testPairBufferAcceptPairsRejectReasonGuards() {
                 fuse::physics::broadphase::pairBufferAcceptPairsRejectReason(buffer, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::None),
    const fuse::physics::broadphase::PairBufferAcceptPairsPreflight emptyAccept =
        fuse::physics::broadphase::preflightPairBufferAcceptPairs(buffer, 2u);
                 fuse::physics::broadphase::pairBufferAcceptPairsRejectReason(buffer, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::ExceedsCapacity),
    expectTrue(fuse::physics::broadphase::pairBufferAcceptPairsRejectsForReason(
                   buffer, 2u, fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::ExceedsCapacity),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferAcceptPairsRejectReasonName(
                               fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::ExceedsCapacity),
                 fuse::physics::broadphase::cellSpanClampRejectReason(inverted, 4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::EmptyRange),
        fuse::physics::broadphase::preflightCellSpanClamp(validRange, 4u);
void testMergePairsIntoBufferPartialCapacityPreflight() {
    const fuse::physics::broadphase::MergePairsIntoBufferPreflight partialPreflight =
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(pairs, partialBuffer);
    expectTrue(partialPreflight.canMerge(), "partial-capacity buffer can merge some pairs");
    expectEq(partialPreflight.mergeablePairCount, 1u, "partial-capacity preflight counts mergeable pairs");
    expectTrue(partialPreflight.partialCapacity, "partial-capacity preflight marks partial merge");
    expectEq(partialPreflight.requestedPairCount, 2u, "partial-capacity preflight records requested count");
    testPairBufferAcceptPairsRejectReasonGuards();
    testMergePairsIntoBufferPartialCapacityPreflight();

// --- deepen additive from deepen-b4-broadphase-guards-5597 ---
    const fuse::physics::broadphase::DedupeBroadphasePreflight uniquePreflight =
    expectTrue(uniquePreflight.alreadyUnique, "dedupe preflight marks already-unique pairs");
    expectTrue(!uniquePreflight.canDedupe(), "dedupe preflight skips already-unique pairs");
    expectTrue(multiPreflight.canDedupe(), "dedupe preflight accepts duplicate pairs");
             static_cast<fuse::u32>(fuse::physics::broadphase::DedupeBroadphaseRejectReason::AlreadyUnique),
    const fuse::physics::broadphase::PairBufferDedupePreflight uniqueDedupe =
        fuse::physics::broadphase::preflightPairBufferSort(unsortedBuffer);
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferDedupeRejectReason::AlreadyUnique),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferSortRejectReason::AlreadySorted),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferSortRejectReason(unsortedBuffer)),
                   validRange, 3u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanBudget),
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanBudget),
        fuse::physics::broadphase::preflightCellSpan(validRange, 8u);
void testBroadphaseShapeInsertPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseShapeInsertRejectReason(
                 fuse::physics::broadphase::BroadphaseShapeInsertRejectReason::OutOfRangeBody),
                 fuse::physics::broadphase::BroadphaseShapeInsertRejectReason::ExceedsOccupancyBudget),
    const fuse::physics::broadphase::BroadphaseShapeInsertPreflight validPreflight =
        fuse::physics::broadphase::preflightBroadphaseShapeInsert(0u, bodies, shapes, params, false);
    expectTrue(validPreflight.canInsert(), "valid shape insert preflight can insert");
void testRefineBroadphaseAllSlotsInvalidGuard() {
             static_cast<fuse::u32>(fuse::physics::broadphase::RefineBroadphaseRejectReason::AllSlotsInvalid),
                               fuse::physics::broadphase::RefineBroadphaseRejectReason::AllSlotsInvalid),
void testPairBufferSortAlreadySortedGuard() {
                               fuse::physics::broadphase::PairBufferSortRejectReason::AlreadySorted),
void testMergePairsAllInvalidPreflightGuards() {
                 fuse::physics::broadphase::mergePairsIntoBufferRejectReason(invalidPairs, buffer)),
             static_cast<fuse::u32>(fuse::physics::broadphase::MergePairsIntoBufferRejectReason::AllInvalidPairs),
                               fuse::physics::broadphase::MergePairsIntoBufferRejectReason::AllInvalidPairs),
        fuse::physics::broadphase::preflightMergePairsIntoBuffer(invalidPairs, buffer);
    testBroadphaseShapeInsertPreflightGuards();
    testMergePairsAllInvalidPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-f861 ---
    const fuse::physics::broadphase::PairBufferWriteSlotPreflight validPreflight =
    expectTrue(validPreflight.canWrite(), "writeSlot preflight accepts valid slot");
                               fuse::physics::broadphase::CellSpanClampRejectReason::WithinSpan),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanClampRejectReason::WithinSpan),
    expectTrue(emptyPreflight.emptyRange, "span-clamp preflight marks empty range");
    expectTrue(!emptyPreflight.canClamp(), "span-clamp preflight cannot clamp empty range");
                   planeRange, 4u, fuse::physics::broadphase::CellSpanClampRejectReason::WithinSpan),
void testBroadphasePairSlotRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphasePairSlotRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphasePairSlotRejectReason::ZeroPairSlots),
    expectTrue(fuse::physics::broadphase::broadphasePairSlotRejectsForReason(
                   0u, fuse::physics::broadphase::BroadphasePairSlotRejectReason::ZeroPairSlots),
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphasePairSlotRejectReasonName(
                               fuse::physics::broadphase::BroadphasePairSlotRejectReason::ZeroPairSlots),
    const fuse::physics::broadphase::BroadphasePairSlotPreflight zeroPreflight =
        fuse::physics::broadphase::preflightBroadphasePairSlots(0u);
    expectTrue(zeroPreflight.zeroPairSlots, "pair-slot preflight marks zero slots");
    expectTrue(!zeroPreflight.canWriteSlots(), "pair-slot preflight cannot write zero slots");
    const fuse::physics::broadphase::BroadphasePairSlotPreflight validPreflight =
        fuse::physics::broadphase::preflightBroadphasePairSlots(4u);
    expectTrue(validPreflight.canWriteSlots(), "pair-slot preflight accepts non-zero slots");
    testBroadphasePairSlotRejectReasonGuards();

// --- deepen additive from deepen-b4-broadphase-guards-7f22 ---
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 1u, 0u, 2u);
void testPairBufferPrepareSlotsPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferPrepareSlotsRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferPrepareSlotsRejectReason::ZeroSlots),
    expectTrue(fuse::physics::broadphase::pairBufferPrepareSlotsRejectsForReason(
                   0u, fuse::physics::broadphase::PairBufferPrepareSlotsRejectReason::ZeroSlots),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferPrepareSlotsRejectReasonName(
                               fuse::physics::broadphase::PairBufferPrepareSlotsRejectReason::ZeroSlots),
    const fuse::physics::broadphase::PairBufferPrepareSlotsPreflight zeroPreflight =
    expectTrue(!zeroPreflight.canPrepare(), "prepare preflight rejects zero slot count");
    expectTrue(zeroPreflight.zeroSlots, "prepare preflight marks zero slots");
    const fuse::physics::broadphase::PairBufferPrepareSlotsPreflight validPreflight =
        fuse::physics::broadphase::preflightPairBufferPrepareSlots(4u);
    expectTrue(validPreflight.canPrepare(), "prepare preflight accepts non-zero slot count");
                 fuse::physics::broadphase::cellSpanClampRejectReason(withinRange, 8u)),
                   wideRange, 8u, fuse::physics::broadphase::CellSpanClampRejectReason::ExceedsSpanPerAxis),
                               fuse::physics::broadphase::CellSpanClampRejectReason::ExceedsSpanPerAxis),
        fuse::physics::broadphase::preflightCellSpanClamp(wideRange, 8u);
                   planeRange, 8u, fuse::physics::broadphase::CellSpanClampRejectReason::ExceedsSpanPerAxis),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroCellSlots),
                   0u, fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroCellSlots),
                               fuse::physics::broadphase::BroadphaseCellPairRejectReason::ZeroCellSlots),
        fuse::physics::broadphase::preflightBroadphaseCellPairGeneration(0u);
    expectTrue(!zeroPreflight.canDispatch(), "cell-pair preflight rejects zero cell slots");
    expectTrue(zeroPreflight.zeroCellSlots, "cell-pair preflight marks zeroCellSlots");
        fuse::physics::broadphase::preflightBroadphaseCellPairGeneration(4u);
    expectTrue(validPreflight.canDispatch(), "cell-pair preflight accepts non-zero cell slots");
void testShouldRunBroadphasePairGenerationGuards() {
    testPairBufferPrepareSlotsPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-6421 ---
    const fuse::physics::broadphase::CellSpanPreflight withinSpan =
        fuse::physics::broadphase::preflightCellSpan(inBudget, 8u);
    const fuse::physics::broadphase::CellSpanPreflight overSpan =
    const fuse::physics::broadphase::CellSpanPreflight emptySpan =
        fuse::physics::broadphase::preflightCellSpan(inverted, 4u);
                 fuse::physics::broadphase::cellSpanRejectReason(validRange, 8u)),
                   validRange, 8u, fuse::physics::broadphase::CellSpanRejectReason::None),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellSpanRejectReason::ExceedsMaxSpan),
                   planeRange, 4u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsMaxSpan),
void testMergePairsIntoBufferRejectsForReasonGuards() {
                   pairs, buffer, fuse::physics::broadphase::MergePairsIntoBufferRejectReason::None),
void testBroadphaseDedupeAndClampPreflightIntegration() {
    testBroadphaseDedupeAndClampPreflightIntegration();

// --- deepen additive from deepen-b4-broadphase-guards-1618 ---
                 fuse::physics::broadphase::pairBufferAcceptPairsRejectReason(buffer, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::AtCapacity),
                               fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::AtCapacity),
                 fuse::physics::broadphase::pairBufferAcceptPairsRejectReason(buffer, 1u)),
    const fuse::physics::broadphase::PairBufferAcceptPairsPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferAcceptPairs(buffer, 1u);
void testCellSpanRejectReasonAndPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellSpanRejectReason(validRange, 8u)),
                   validRange, 2u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanLimit),
                               fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanLimit),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellSpanRejectReason(inverted, 4u)),
        fuse::physics::broadphase::preflightCellSpan2D(planeRange, 2u);
    expectTrue(!planePreflight.canClamp(), "2D span preflight rejects over-limit range");
                   planeRange, 2u, fuse::physics::broadphase::CellSpanRejectReason::ExceedsSpanLimit),
void testRefineBroadphasePreflightRejectsForReasonGuards() {
    expectTrue(fuse::physics::broadphase::refineBroadphasePreflightRejectsForReason(
void testDedupeBroadphasePreflightRejectsForReasonGuards() {
    expectTrue(fuse::physics::broadphase::dedupeBroadphasePreflightRejectsForReason(
                   buffer, fuse::physics::broadphase::DedupeBroadphaseRejectReason::SinglePair),
                   buffer, fuse::physics::broadphase::DedupeBroadphaseRejectReason::None),
void testMergeBroadphasePreflightRejectsForReasonGuards() {
    expectTrue(fuse::physics::broadphase::mergeBroadphasePreflightRejectsForReason(
                   bodies, shapes, fuse::physics::broadphase::BroadphaseMergeRejectReason::None),
    testCellSpanRejectReasonAndPreflightGuards();
    testRefineBroadphasePreflightRejectsForReasonGuards();
    testDedupeBroadphasePreflightRejectsForReasonGuards();
    testMergeBroadphasePreflightRejectsForReasonGuards();

// --- deepen additive from b4-broadphase-deepen-ed2f ---
void testPairBufferPushShouldRunGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::cellSpanRejectReason(validRange)),
                   inverted, fuse::physics::broadphase::CellSpanRejectReason::EmptyRange),
        fuse::physics::broadphase::preflightCellSpan(validRange);
        fuse::physics::broadphase::preflightCellSpan(planeRange);
    expectTrue(planePreflight.canUseRange(), "2D cell-span preflight accepts valid range");
    expectEq(planePreflight.occupancyCount, 8u, "2D cell-span preflight reports occupancy count");
void testShapeCellInsertPreflightGuards() {
    const fuse::physics::broadphase::ShapeCellInsertPreflight withinBudget =
        fuse::physics::broadphase::preflightShapeCellInsert(validRange, 8u);
                               fuse::physics::broadphase::ShapeCellInsertRejectReason::ExceedsBudget),
    const fuse::physics::broadphase::ShapeCellInsertPreflight overBudget =
        fuse::physics::broadphase::preflightShapeCellInsert(validRange, 7u);
                 fuse::physics::broadphase::shapeCellInsertRejectReason(inverted, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::EmptyRange),
    testShapeCellInsertPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-6a82 ---
                   buffer, 3u, fuse::physics::broadphase::PairBufferAcceptPairsRejectReason::AtCapacity),
void testBroadphaseCellSlotRejectReasonGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellSlotRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellSlotRejectReason::ZeroSlots),
    expectTrue(fuse::physics::broadphase::broadphaseCellSlotRejectsForReason(
                   0u, fuse::physics::broadphase::BroadphaseCellSlotRejectReason::ZeroSlots),
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseCellSlotRejectReasonName(
                               fuse::physics::broadphase::BroadphaseCellSlotRejectReason::ZeroSlots),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellSlotRejectReason(4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellSlotRejectReason::None),
    const fuse::physics::broadphase::BroadphaseCellSlotPreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseCellSlots(4u);
    testBroadphaseCellSlotRejectReasonGuards();

// --- deepen additive from deepen-b4-broadphase-guards-e86f ---
void testPairBufferAcceptPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferAcceptRejectReason(buffer, 2u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptRejectReason::None),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferAcceptRejectReason(buffer, 3u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::PairBufferAcceptRejectReason::ExceedsCapacity),
    expectTrue(std::strcmp(fuse::physics::broadphase::pairBufferAcceptRejectReasonName(
                               fuse::physics::broadphase::PairBufferAcceptRejectReason::ExceedsCapacity),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::pairBufferAcceptRejectReason(buffer, 0u)),
    const fuse::physics::broadphase::PairBufferAcceptPreflight preflight =
        fuse::physics::broadphase::preflightPairBufferAccept(buffer, 1u);
void testCellRangeSpanClampPreflightGuards() {
                 fuse::physics::broadphase::cellRangeSpanClampRejectReason(wideRange, 0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellRangeSpanClampRejectReason::UnlimitedSpan),
                 fuse::physics::broadphase::cellRangeSpanClampRejectReason(inverted, 8u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::CellRangeSpanClampRejectReason::EmptyRange),
    expectTrue(fuse::physics::broadphase::cellRangeSpanClampRejectsForReason(
                   inverted, 8u, fuse::physics::broadphase::CellRangeSpanClampRejectReason::EmptyRange),
    const fuse::physics::broadphase::CellRangeSpanClampPreflight preflight =
        fuse::physics::broadphase::preflightCellRangeSpanClamp(wideRange, 8u);
    expectTrue(std::strcmp(fuse::physics::broadphase::cellRangeSpanClampRejectReasonName(
                               fuse::physics::broadphase::CellRangeSpanClampRejectReason::UnlimitedSpan),
    const fuse::physics::broadphase::CellRangeSpanClampPreflight planePreflight =
        fuse::physics::broadphase::preflightCellRangeSpanClamp(planeRange, 4u);
    expectTrue(planePreflight.needsClamp(), "2D span-clamp preflight requests clamp for wide range");
void testBroadphaseCellPairBuildPreflightGuards() {
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellPairBuildRejectReason(0u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairBuildRejectReason::NoCellSlots),
    expectTrue(std::strcmp(fuse::physics::broadphase::broadphaseCellPairBuildRejectReasonName(
                               fuse::physics::broadphase::BroadphaseCellPairBuildRejectReason::NoCellSlots),
    expectEq(static_cast<fuse::u32>(fuse::physics::broadphase::broadphaseCellPairBuildRejectReason(4u)),
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairBuildRejectReason::None),
    const fuse::physics::broadphase::BroadphaseCellPairBuildPreflight preflight =
        fuse::physics::broadphase::preflightBroadphaseCellPairBuild(2u);
    testPairBufferAcceptPreflightGuards();
    testCellRangeSpanClampPreflightGuards();
    testBroadphaseCellPairBuildPreflightGuards();

// --- deepen additive from deepen-b4-broadphase-guards-1225 ---
                 fuse::physics::broadphase::pairBufferWriteSlotRejectReason(buffer, 1u, 1u, 1u)),
        fuse::physics::broadphase::preflightPairBufferWriteSlot(buffer, 0u, 2u, 3u);
    const fuse::physics::broadphase::CellSpanClampPreflight widePreflight =
    expectTrue(widePreflight.needsClamp(), "wide-range preflight needs clamp");
    expectTrue(widePreflight.exceedsSpanPerAxis, "wide-range preflight marks exceedsSpanPerAxis");
                 fuse::physics::broadphase::cellSpanClampRejectReason(inverted, 8u)),
    expectTrue(!fuse::physics::broadphase::preflightCellSpanClamp(inverted, 8u).canClamp(),
    const fuse::physics::broadphase::CellSpanClampPreflight planePreflight =
        fuse::physics::broadphase::preflightCellSpanClamp2D(planeRange, 8u);
    expectTrue(planePreflight.needsClamp(), "2D wide-range preflight needs clamp");
    expectEq(planePreflight.spanPerAxis.x, 16, "2D preflight reports x span");

// --- deepen additive from deepen-b4-broadphase-guards-39a9 ---
    const fuse::physics::broadphase::ShapeCellInsertPreflight orphanPreflight =
    expectTrue(orphanPreflight.outOfRangeBody, "orphan shape marks out-of-range body");
    expectTrue(!orphanPreflight.canInsert(), "orphan shape cannot insert into cells");
    const fuse::physics::broadphase::ShapeCellInsertPreflight budgetPreflight =
    expectTrue(budgetPreflight.occupancyRejected, "huge shape marks occupancy rejected");
    expectEq(static_cast<fuse::u32>(budgetPreflight.reason),
             static_cast<fuse::u32>(fuse::physics::broadphase::ShapeCellInsertRejectReason::OccupancyRejected),
void testBroadphaseCellPairGenPreflightGuards() {
    const fuse::physics::broadphase::BroadphaseCellPairGenPreflight preflight =
             static_cast<fuse::u32>(fuse::physics::broadphase::BroadphaseCellPairGenRejectReason::None),
    testBroadphaseCellPairGenPreflightGuards();
