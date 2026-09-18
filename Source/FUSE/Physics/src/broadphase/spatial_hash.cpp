#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>

#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fuse::physics::broadphase {

BroadphasePreflight preflight_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    BroadphasePreflight preflight{};
    preflight.emptyInput = isEmptyBroadphaseInput(bodies, shapes);
    preflight.singletonInput = isSingletonBroadphaseInput(bodies, shapes);
    preflight.skipped = preflight.emptyInput || preflight.singletonInput;
    return preflight;
}

bool can_skip_broadphase_dispatch(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflight_broadphase(bodies, shapes).skipped;
}

RefineBroadphasePreflight preflight_refine_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    RefineBroadphasePreflight preflight{};
    preflight.emptyBuffer = buffer.canSkipSoAIteration();
    preflight.noValidPairs = !buffer.hasValidPairs();
    preflight.skippedBroadphase = canSkipBroadphase(bodies, shapes);
    preflight.skipped =
        preflight.emptyBuffer || preflight.noValidPairs || preflight.skippedBroadphase;
    return preflight;
}

bool should_skip_refine_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return preflight_refine_broadphase(bodies, shapes, buffer).skipped;
}

DedupeBroadphasePreflight preflight_dedupe_broadphase(const PairBufferSoA& buffer) {
    DedupeBroadphasePreflight preflight{};
    preflight.activeCount = buffer.activeCount;
    preflight.skipped = buffer.canSkipDedupePass();
    return preflight;
}

bool should_skip_dedupe_broadphase(const PairBufferSoA& buffer) {
    return preflight_dedupe_broadphase(buffer).skipped;
}

CellOccupancyPreflight preflight_cell_occupancy(const CellRange3& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.emptyRange = isEmptyCellRange(range);
    preflight.occupancyCount = estimateCellOccupancyCount(range);
    preflight.budgetRemaining = occupancyBudgetRemaining(range, maxCells);
    preflight.exceedsBudget = exceedsCellOccupancyBudget(range, maxCells);
    preflight.skipped = preflight.emptyRange || preflight.exceedsBudget;
    return preflight;
}

CellOccupancyPreflight preflight_cell_occupancy_2d(const CellRange2& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.emptyRange = isEmptyCellRange(range);
    preflight.occupancyCount = estimateCellOccupancyCount(range);
    preflight.budgetRemaining = occupancyBudgetRemaining(range, maxCells);
    preflight.exceedsBudget = exceedsCellOccupancyBudget(range, maxCells);
    preflight.skipped = preflight.emptyRange || preflight.exceedsBudget;
    return preflight;
}

bool should_skip_shape_cell_insertion(const CellRange3& range, u32 maxCells) {
    return preflight_cell_occupancy(range, maxCells).skipped;
}

bool should_skip_shape_cell_insertion_2d(const CellRange2& range, u32 maxCells) {
    return preflight_cell_occupancy_2d(range, maxCells).skipped;
}

const char* candidatePairRejectReasonName(CandidatePairRejectReason reason) {
    switch (reason) {
    case CandidatePairRejectReason::None:
        return "None";
    case CandidatePairRejectReason::SelfPair:
        return "SelfPair";
    case CandidatePairRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    }
    return "Unknown";
}

namespace {

constexpr u32 kBuildGrainSize = 8u;
constexpr u32 kCellGrainSize = 4u;
constexpr u32 kPlanePairGrainSize = 16u;

f32 shapeRadius(const CollisionShapeSoA& shapes, u32 shapeIndex) {
    if (shapeIndex >= shapes.count()) {
        return 0.5f;
    }
    return shapes.params[shapeIndex].x;
}

u32 shapeBodyIndex(const CollisionShapeSoA& shapes, u32 shapeIndex) {
    if (shapeIndex >= shapes.count()) {
        return 0;
    }
    return shapes.bodyIndices[shapeIndex];
}

CollisionShapeType shapeType(const CollisionShapeSoA& shapes, u32 shapeIndex) {
    if (shapeIndex >= shapes.count()) {
        return CollisionShapeType::Sphere;
    }
    return static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
}

CandidatePair canonicalPair(u32 bodyA, u32 bodyB) {
    if (bodyA > bodyB) {
        std::swap(bodyA, bodyB);
    }
    return {bodyA, bodyB};
}

void appendPair(std::vector<CandidatePair>& pairs, u32 bodyA, u32 bodyB) {
    if (!isValidCandidatePair(bodyA, bodyB)) {
        return;
    }
    pairs.push_back(canonicalPair(bodyA, bodyB));
}

void dedupePairs(std::vector<CandidatePair>& pairs) {
    std::sort(pairs.begin(), pairs.end(), [](const CandidatePair& left, const CandidatePair& right) {
        return left.bodyA < right.bodyA || (left.bodyA == right.bodyA && left.bodyB < right.bodyB);
    });
    pairs.erase(std::unique(pairs.begin(), pairs.end(),
                            [](const CandidatePair& left, const CandidatePair& right) {
                                return left.bodyA == right.bodyA && left.bodyB == right.bodyB;
                            }),
                pairs.end());
}

struct CellBuckets {
    explicit CellBuckets(u32 tableSize) : buckets(tableSize), locks(tableSize) {}

    std::vector<std::vector<u32>> buckets;
    std::vector<std::mutex> locks;

    void insert(u32 key, u32 bodyIndex) {
        std::lock_guard<std::mutex> guard(locks[key]);
        buckets[key].push_back(bodyIndex);
    }
};

std::vector<u32> uniqueOccupants(const std::vector<u32>& occupants) {
    std::vector<u32> uniqueBodies = occupants;
    std::sort(uniqueBodies.begin(), uniqueBodies.end());
    uniqueBodies.erase(std::unique(uniqueBodies.begin(), uniqueBodies.end()), uniqueBodies.end());
    return uniqueBodies;
}

u32 countPairsForCell(const std::vector<u32>& occupants) {
    if (occupants.size() < 2u) {
        return 0u;
    }
    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    const u32 bodyCount = static_cast<u32>(uniqueBodies.size());
    return bodyCount > 1u ? bodyCount * (bodyCount - 1u) / 2u : 0u;
}

void generatePairsForCell(const std::vector<u32>& occupants, std::vector<CandidatePair>& out) {
    if (occupants.size() < 2u) {
        return;
    }
    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    for (usize i = 0; i < uniqueBodies.size(); ++i) {
        for (usize j = i + 1; j < uniqueBodies.size(); ++j) {
            appendPair(out, uniqueBodies[i], uniqueBodies[j]);
        }
    }
}

void writePairsForCellSlots(
    const std::vector<u32>& occupants,
    u32 slotStart,
    PairBufferSoA& buffer) {
    if (occupants.size() < 2u) {
        return;
    }
    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    u32 slot = slotStart;
    for (usize i = 0; i < uniqueBodies.size(); ++i) {
        for (usize j = i + 1; j < uniqueBodies.size(); ++j) {
            buffer.writeSlot(slot++, uniqueBodies[i], uniqueBodies[j]);
        }
    }
}

void populateShapeCells(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D,
    CellBuckets& cells) {
    const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
    if (bodyIndex >= bodies.count()) {
        return;
    }

    const vec3 position = bodies.positions[bodyIndex];
    const f32 cellSize = clampCellSize(params.cellSize);
    const u32 tableSize = clampTableSize(params.tableSize);
    const u32 maxSpan = params.maxCellSpanPerAxis;
    const u32 maxOccupancy = params.maxCellOccupancy;
    const CollisionShapeType type = shapeType(shapes, shapeIndex);

    if (use2D) {
        CellRange2 range = {};
        if (type == CollisionShapeType::Box) {
            const vec3 halfExtents = shapes.params[shapeIndex];
            const aabb bounds = aabbFromBox(position, halfExtents);
            range = cellRangeFromAabb2D(bounds, cellSize, maxSpan);
        } else {
            const f32 radius = shapeRadius(shapes, shapeIndex);
            range = cellRangeFromSphere2D({position.x, position.y}, radius, cellSize, maxSpan);
        }
        if (should_skip_shape_cell_insertion_2d(range, maxOccupancy)) {
            return;
        }
        for (s32 cy = range.minCell.y; cy <= range.maxCell.y; ++cy) {
            for (s32 cx = range.minCell.x; cx <= range.maxCell.x; ++cx) {
                const u32 key = spatialHash2D(cx, cy, tableSize);
                cells.insert(key, bodyIndex);
            }
        }
        return;
    }

    CellRange3 range = {};
    if (type == CollisionShapeType::Box) {
        const vec3 halfExtents = shapes.params[shapeIndex];
        range = cellRangeFromBox(position, halfExtents, cellSize, maxSpan);
    } else {
        const f32 radius = shapeRadius(shapes, shapeIndex);
        range = cellRangeFromSphere(position, radius, cellSize, maxSpan);
    }
    if (should_skip_shape_cell_insertion(range, maxOccupancy)) {
        return;
    }
    for (s32 cz = range.minCell.z; cz <= range.maxCell.z; ++cz) {
        for (s32 cy = range.minCell.y; cy <= range.maxCell.y; ++cy) {
            for (s32 cx = range.minCell.x; cx <= range.maxCell.x; ++cx) {
                const u32 key = spatialHash(cx, cy, cz, tableSize);
                cells.insert(key, bodyIndex);
            }
        }
    }
}

void mergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, PairBufferSoA& buffer) {
    if (pairs.empty()) {
        return;
    }

    for (const CandidatePair& pair : pairs) {
        if (!buffer.canAcceptPairs(1u)) {
            break;
        }
        buffer.push(pair.bodyA, pair.bodyB);
    }
}

void dedupeBuffer(PairBufferSoA& buffer) {
    if (should_skip_dedupe_broadphase(buffer)) {
        return;
    }

    std::vector<CandidatePair> pairs = buffer.toVector();
    dedupePairs(pairs);

    buffer.clear();
    for (const CandidatePair& pair : pairs) {
        buffer.push(pair.bodyA, pair.bodyB);
    }
}

f32 bodyShapeRadius(const CollisionShapeSoA& shapes, u32 bodyIndex) {
    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        if (shapeBodyIndex(shapes, shapeIndex) == bodyIndex) {
            return shapeRadius(shapes, shapeIndex);
        }
    }
    return 0.5f;
}

bool pairPassesAabbRefine(
    u32 bodyA,
    u32 bodyB,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (!isValidCandidatePair(bodyA, bodyB, bodies.count())) {
        return false;
    }

    const vec3 posA = bodies.positions[bodyA];
    const vec3 posB = bodies.positions[bodyB];
    const f32 radiusA = bodyShapeRadius(shapes, bodyA);
    const f32 radiusB = bodyShapeRadius(shapes, bodyB);
    return sphereAabbOverlap(posA, radiusA, posB, radiusB);
}

void runBroadphaseIntoBufferInternal(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D,
    PairBufferSoA& buffer) {
    buffer.clear();
    if (can_skip_broadphase_dispatch(bodies, shapes)) {
        return;
    }

    const SpatialHashParams normalizedParams = normalizeSpatialHashParams(params);
    const u32 tableSize = normalizedParams.tableSize;
    CellBuckets cells(tableSize);

    const u32 shapeCount = shapes.count();
    fuse::jobs::parallel_for(0u, shapeCount, kBuildGrainSize, [&](u32 shapeIndex) {
        populateShapeCells(shapeIndex, bodies, shapes, normalizedParams, use2D, cells);
    });

    std::vector<u32> cellSlotOffsets(tableSize, 0u);
    u32 totalCellSlots = 0u;
    for (u32 cellIndex = 0; cellIndex < tableSize; ++cellIndex) {
        cellSlotOffsets[cellIndex] = totalCellSlots;
        totalCellSlots += countPairsForCell(cells.buckets[cellIndex]);
    }

    buffer.preparePairSlots(totalCellSlots);
    if (totalCellSlots == 0u) {
        return;
    }

    fuse::jobs::parallel_for(0u, tableSize, kCellGrainSize, [&](u32 cellIndex) {
        if (cells.buckets[cellIndex].empty()) {
            return;
        }
        writePairsForCellSlots(cells.buckets[cellIndex], cellSlotOffsets[cellIndex], buffer);
    });
    buffer.compact();
    dedupeBuffer(buffer);

    std::vector<u32> planeBodies;
    std::vector<u32> dynamicBodies;
    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (shapeType(shapes, shapeIndex) == CollisionShapeType::Plane) {
            planeBodies.push_back(bodyIndex);
        } else if ((bodies.flags[bodyIndex] & RB_STATIC) == 0) {
            dynamicBodies.push_back(bodyIndex);
        }
    }

    if (!planeBodies.empty() && !dynamicBodies.empty()) {
        std::unordered_set<u64> existing;
        existing.reserve(buffer.activeCount * 2 + 1);
        for (u32 i = 0; i < buffer.activeCount; ++i) {
            const u64 key = (static_cast<u64>(buffer.bodyA[i]) << 32) | buffer.bodyB[i];
            existing.insert(key);
        }

        const u32 dynamicCount = static_cast<u32>(dynamicBodies.size());
        std::vector<std::vector<CandidatePair>> dynamicPlanePairs(dynamicCount);
        fuse::jobs::parallel_for(0u, dynamicCount, kPlanePairGrainSize, [&](u32 dynamicIndex) {
            const u32 dynamicBody = dynamicBodies[dynamicIndex];
            for (u32 planeBody : planeBodies) {
                if (!isValidCandidatePair(dynamicBody, planeBody, bodies.count())) {
                    continue;
                }
                const CandidatePair pair = canonicalPair(dynamicBody, planeBody);
                const u64 key = (static_cast<u64>(pair.bodyA) << 32) | pair.bodyB;
                if (existing.find(key) == existing.end()) {
                    dynamicPlanePairs[dynamicIndex].push_back(pair);
                }
            }
        });

        for (const std::vector<CandidatePair>& bucketPairs : dynamicPlanePairs) {
            mergePairsIntoBuffer(bucketPairs, buffer);
        }
        dedupeBuffer(buffer);
    }

    if (buffer.maxCapacity > 0u) {
        buffer.applyMaxCapacityClamp();
    }
}

void refineBroadphasePairsParallelImpl(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    PairBufferSoA& buffer) {
    if (should_skip_refine_broadphase(bodies, shapes, buffer)) {
        return;
    }

    const u32 pairCount = buffer.activeCount;
    fuse::jobs::parallel_for(0u, pairCount, kPlanePairGrainSize, [&](u32 pairIndex) {
        if (pairIndex >= buffer.activeCount || !buffer.slotIsValid(pairIndex)) {
            return;
        }

        const u32 bodyA = buffer.bodyA[pairIndex];
        const u32 bodyB = buffer.bodyB[pairIndex];
        if (!isValidCandidatePair(bodyA, bodyB, bodies.count())) {
            buffer.invalidateSlot(pairIndex);
            return;
        }
        if (!pairPassesAabbRefine(bodyA, bodyB, bodies, shapes)) {
            buffer.invalidateSlot(pairIndex);
        }
    });

    buffer.compact();
}

} // namespace

void refineBroadphasePairsParallel(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    PairBufferSoA& buffer) {
    refineBroadphasePairsParallelImpl(bodies, shapes, buffer);
}

void runBroadphaseIntoBuffer(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    PairBufferSoA& buffer) {
    runBroadphaseIntoBufferInternal(bodies, shapes, params, false, buffer);
}

void runBroadphase2DIntoBuffer(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    PairBufferSoA& buffer) {
    runBroadphaseIntoBufferInternal(bodies, shapes, params, true, buffer);
}

std::vector<CandidatePair> runBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params) {
    PairBufferSoA buffer;
    buffer.reserveForUniqueBodies(params.bodyCount > 0u ? params.bodyCount : 32u);
    runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    return buffer.toVector();
}

std::vector<CandidatePair> runBroadphase2D(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params) {
    PairBufferSoA buffer;
    buffer.reserveForUniqueBodies(params.bodyCount > 0u ? params.bodyCount : 32u);
    runBroadphase2DIntoBuffer(bodies, shapes, params, buffer);
    return buffer.toVector();
}

} // namespace fuse::physics::broadphase
