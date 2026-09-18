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

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseInput(bodies, shapes),
               "bodies without shapes is empty broadphase input");
    expectTrue(fuse::physics::broadphase::canSkipBroadphase(bodies, shapes),
               "canSkipBroadphase when shapes are missing");

    bodies.clear();
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    expectTrue(fuse::physics::broadphase::isEmptyBroadphaseInput(bodies, shapes),
               "shapes without bodies is empty broadphase input");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
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
    expectTrue(fuse::physics::broadphase::cellOccupancyWithinBudget(smallRange, 0u),
               "zero budget means unlimited occupancy");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    expectEq(fuse::physics::broadphase::estimateCellOccupancyCount(planeRange), 8u,
             "small 2D range has eight cells");
    expectTrue(fuse::physics::broadphase::exceedsCellOccupancyBudget(planeRange, 4u),
               "2D occupancy budget guard flags overflow");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    expectTrue(fuse::physics::broadphase::cellOccupancyWithinBudget(inverted, 1u),
               "empty range is within any positive budget");
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

void testCellOccupancyPreflightGuards() {
    const fuse::physics::broadphase::CellRange3 unitRange = {{0, 0, 0}, {1, 1, 1}};
    const auto withinBudget =
        fuse::physics::broadphase::preflightCellOccupancy(unitRange, 8u);
    expectTrue(!withinBudget.skipped, "non-empty range is not skipped");
    expectEq(withinBudget.cellCount, 8u, "preflight reports occupancy count");
    expectTrue(!withinBudget.exceedsBudget, "range at budget limit does not exceed");

    const auto overBudget =
        fuse::physics::broadphase::preflightCellOccupancy(unitRange, 7u);
    expectTrue(overBudget.exceedsBudget, "preflight flags occupancy above budget");

    fuse::physics::broadphase::CellRange3 inverted = {{2, 2, 2}, {1, 1, 1}};
    const auto emptyPreflight =
        fuse::physics::broadphase::preflightCellOccupancy(inverted, 4u);
    expectTrue(emptyPreflight.skipped, "empty range preflight is skipped");
    expectEq(emptyPreflight.cellCount, 0u, "empty range reports zero cells");

    const fuse::physics::broadphase::CellRange2 planeRange = {{0, 0}, {3, 1}};
    const auto planePreflight =
        fuse::physics::broadphase::preflightCellOccupancy(planeRange, 8u);
    expectEq(planePreflight.cellCount, 8u, "2D preflight reports occupancy count");
}

void testPerShapeCellBudgetGuard() {
    expectEq(fuse::physics::broadphase::perShapeCellBudget(0u, false), 0u,
             "zero span means unlimited per-shape budget");
    expectEq(fuse::physics::broadphase::perShapeCellBudget(4u, true), 16u,
             "2D per-shape budget is span squared");
    expectEq(fuse::physics::broadphase::perShapeCellBudget(4u, false), 64u,
             "3D per-shape budget is span cubed");
}

void testPairSlotPreflightGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.setMaxCapacity(4u);

    const auto zeroSlots = fuse::physics::broadphase::preflightPairSlots(0u, buffer);
    expectTrue(zeroSlots.skipped, "zero slot preflight is skipped");

    const auto withinCapacity = fuse::physics::broadphase::preflightPairSlots(3u, buffer);
    expectTrue(!withinCapacity.skipped, "non-zero slot preflight is active");
    expectTrue(!withinCapacity.exceedsBufferCapacity, "slots within maxCapacity pass preflight");

    const auto exceedsCapacity = fuse::physics::broadphase::preflightPairSlots(8u, buffer);
    expectTrue(exceedsCapacity.exceedsBufferCapacity,
               "slot count above maxCapacity is flagged");
}

void testRefineBroadphasePreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    fuse::physics::broadphase::PairBufferSoA buffer;

    const auto emptyScene =
        fuse::physics::broadphase::preflightRefineBroadphasePairs(buffer, bodies, shapes);
    expectTrue(emptyScene.skipped, "refine preflight skips empty scene");
    expectTrue(emptyScene.emptyBroadphaseInput, "refine preflight marks empty broadphase input");
    expectTrue(emptyScene.emptyBuffer, "refine preflight marks empty buffer");
    expectTrue(fuse::physics::broadphase::canSkipRefineBroadphasePairs(buffer, bodies, shapes),
               "canSkipRefineBroadphasePairs on empty scene");

    bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 1, {1.f, 0.f, 0.f});
    buffer.push(0u, 1u);

    const auto populated =
        fuse::physics::broadphase::preflightRefineBroadphasePairs(buffer, bodies, shapes);
    expectTrue(!populated.skipped, "refine preflight runs with valid pairs");
    expectTrue(!populated.emptyBroadphaseInput, "populated scene is not empty input");
    expectTrue(!populated.emptyBuffer, "non-empty buffer is not empty");
    expectTrue(!fuse::physics::broadphase::canSkipRefineBroadphasePairs(buffer, bodies, shapes),
               "canSkipRefineBroadphasePairs false with valid pairs");
}

void testPairBufferDedupeAndCompactGuards() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    expectTrue(fuse::physics::broadphase::canSkipDedupeBuffer(buffer),
               "empty buffer skips dedupe");
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer skips compactAndClamp");

    buffer.push(0u, 1u);
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
}

void testPairBufferSlotValidityBounds() {
    fuse::physics::broadphase::PairBufferSoA buffer;
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, 0u, 1u);
    expectTrue(buffer.slotIsValid(0u), "in-range slot is valid");
    expectTrue(!buffer.slotIsValid(2u), "slot beyond pairSlotCount is invalid");
    expectTrue(!buffer.slotIsValid(99u), "slot beyond storage is invalid");

    buffer.invalidateSlot(99u);
    expectTrue(buffer.slotIsValid(0u), "out-of-range invalidate is a no-op");
    buffer.invalidateSlot(0u);
    expectTrue(!buffer.slotIsValid(0u), "in-range invalidate clears slot");
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
    testCellOccupancyPreflightGuards();
    testPerShapeCellBudgetGuard();
    testPairSlotPreflightGuards();
    testRefineBroadphasePreflightGuards();
    testPairBufferDedupeAndCompactGuards();
    testPairBufferSlotValidityBounds();
    testBroadphaseBoxShapeCellRange();

    if (g_failures == 0) {
        std::printf("fuse_physics_broadphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_broadphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
