#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/rotation.hpp>

#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fuse::physics::broadphase {

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

const char* cellSpanRejectReasonName(CellSpanRejectReason reason) {
    switch (reason) {
    case CellSpanRejectReason::None:
        return "None";
    case CellSpanRejectReason::EmptyRange:
        return "EmptyRange";
    case CellSpanRejectReason::ExceedsSpan:
        return "ExceedsSpan";
    }
    return "Unknown";
}

const char* cellOccupancyRejectReasonName(CellOccupancyRejectReason reason) {
    switch (reason) {
    case CellOccupancyRejectReason::None:
        return "None";
    case CellOccupancyRejectReason::EmptyRange:
        return "EmptyRange";
    case CellOccupancyRejectReason::ExceedsBudget:
        return "ExceedsBudget";
    }
    return "Unknown";
}

const char* broadphaseRejectReasonName(BroadphaseRejectReason reason) {
    switch (reason) {
    case BroadphaseRejectReason::None:
        return "None";
    case BroadphaseRejectReason::EmptyInput:
        return "EmptyInput";
    case BroadphaseRejectReason::SingletonInput:
        return "SingletonInput";
    }
    return "Unknown";
}

const char* refineBroadphaseRejectReasonName(RefineBroadphaseRejectReason reason) {
    switch (reason) {
    case RefineBroadphaseRejectReason::None:
        return "None";
    case RefineBroadphaseRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case RefineBroadphaseRejectReason::EmptyInput:
        return "EmptyInput";
    case RefineBroadphaseRejectReason::NoValidPairs:
        return "NoValidPairs";
    }
    return "Unknown";
}

const char* dedupeBroadphaseRejectReasonName(DedupeBroadphaseRejectReason reason) {
    switch (reason) {
    case DedupeBroadphaseRejectReason::None:
        return "None";
    case DedupeBroadphaseRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case DedupeBroadphaseRejectReason::SinglePair:
        return "SinglePair";
    }
    return "Unknown";
}

const char* cellPairGenRejectReasonName(CellPairGenRejectReason reason) {
    switch (reason) {
    case CellPairGenRejectReason::None:
        return "None";
    case CellPairGenRejectReason::EmptyCell:
        return "EmptyCell";
    case CellPairGenRejectReason::SingletonOccupant:
        return "SingletonOccupant";
    }
    return "Unknown";
}

const char* shapeCellInsertRejectReasonName(ShapeCellInsertRejectReason reason) {
    switch (reason) {
    case ShapeCellInsertRejectReason::None:
        return "None";
    case ShapeCellInsertRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    case ShapeCellInsertRejectReason::OccupancySkipped:
        return "OccupancySkipped";
    }
    return "Unknown";
}

const char* broadphaseCellPairGenRejectReasonName(BroadphaseCellPairGenRejectReason reason) {
    switch (reason) {
    case BroadphaseCellPairGenRejectReason::None:
        return "None";
    case BroadphaseCellPairGenRejectReason::EmptyCells:
        return "EmptyCells";
    }
    return "Unknown";
}

const char* mergeBroadphaseRejectReasonName(BroadphaseMergeRejectReason reason) {
    switch (reason) {
    case BroadphaseMergeRejectReason::None:
        return "None";
    case BroadphaseMergeRejectReason::EmptyPlaneBodies:
        return "EmptyPlaneBodies";
    case BroadphaseMergeRejectReason::EmptyDynamicBodies:
        return "EmptyDynamicBodies";
    }
    return "Unknown";
}

namespace {

constexpr u32 kBuildGrainSize = 8u;
constexpr u32 kCellGrainSize = 4u;
constexpr u32 kPlanePairGrainSize = 16u;

/// World AABB half extents of a box shape on `bodyIndex` (orientation-aware).
vec3 worldBoxHalfExtents(const RigidBodySoA& bodies, u32 bodyIndex, vec3 halfExtents) {
    return bodyIndex < bodies.orientations.size() ? orientedBoxHalfExtents(bodies.orientations[bodyIndex], halfExtents)
                                                  : halfExtents;
}

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

u32 countUniqueCellOccupantsImpl(const std::vector<u32>& occupants) {
    if (occupants.empty()) {
        return 0u;
    }
    return static_cast<u32>(uniqueOccupants(occupants).size());
}

CellPairGenPreflight preflightCellPairGenerationImpl(const std::vector<u32>& occupants) {
    CellPairGenPreflight preflight{};
    preflight.uniqueOccupantCount = countUniqueCellOccupantsImpl(occupants);
    preflight.reason = cellPairGenRejectReason(preflight.uniqueOccupantCount);
    preflight.emptyCell = preflight.reason == CellPairGenRejectReason::EmptyCell;
    preflight.singletonOccupant = preflight.reason == CellPairGenRejectReason::SingletonOccupant;
    preflight.pairCount = estimateCellPairCount(preflight.uniqueOccupantCount);
    return preflight;
}

u32 countPairsForCell(const std::vector<u32>& occupants) {
    const CellPairGenPreflight preflight = preflightCellPairGenerationImpl(occupants);
    if (!preflight.canGenerate()) {
        return 0u;
    }
    return preflight.pairCount;
}

void generatePairsForCell(const std::vector<u32>& occupants, std::vector<CandidatePair>& out) {
    if (!preflightCellPairGenerationImpl(occupants).canGenerate()) {
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
    if (!preflightCellPairGenerationImpl(occupants).canGenerate()) {
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

ShapeCellInsertRejectReason shapeCellInsertRejectReasonImpl(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {
    const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
    if (bodyIndex >= bodies.count()) {
        return ShapeCellInsertRejectReason::OutOfRangeBody;
    }

    const vec3 position = bodies.positions[bodyIndex];
    const f32 cellSize = clampCellSize(params.cellSize);
    const u32 maxSpan = params.maxCellSpanPerAxis;
    const u32 maxOccupancy = params.maxCellOccupancy;
    const CollisionShapeType type = shapeType(shapes, shapeIndex);

    if (use2D) {
        CellRange2 range = {};
        if (type == CollisionShapeType::Box) {
            const vec3 halfExtents = worldBoxHalfExtents(bodies, bodyIndex, shapes.params[shapeIndex]);
            const aabb bounds = aabbFromBox(position, halfExtents);
            range = cellRangeFromAabb2D(bounds, cellSize, maxSpan);
        } else {
            const f32 radius = shapeRadius(shapes, shapeIndex);
            range = cellRangeFromSphere2D({position.x, position.y}, radius, cellSize, maxSpan);
        }
        if (canSkipCellOccupancyIteration(range, maxOccupancy)) {
            return ShapeCellInsertRejectReason::OccupancySkipped;
        }
        return ShapeCellInsertRejectReason::None;
    }

    CellRange3 range = {};
    if (type == CollisionShapeType::Box) {
        const vec3 halfExtents = worldBoxHalfExtents(bodies, bodyIndex, shapes.params[shapeIndex]);
        range = cellRangeFromBox(position, halfExtents, cellSize, maxSpan);
    } else {
        const f32 radius = shapeRadius(shapes, shapeIndex);
        range = cellRangeFromSphere(position, radius, cellSize, maxSpan);
    }
    if (canSkipCellOccupancyIteration(range, maxOccupancy)) {
        return ShapeCellInsertRejectReason::OccupancySkipped;
    }
    return ShapeCellInsertRejectReason::None;
}

ShapeCellInsertPreflight preflightShapeCellInsertImpl(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {
    ShapeCellInsertPreflight preflight{};
    preflight.reason = shapeCellInsertRejectReasonImpl(shapeIndex, bodies, shapes, params, use2D);
    preflight.outOfRangeBody = preflight.reason == ShapeCellInsertRejectReason::OutOfRangeBody;
    preflight.occupancySkipped = preflight.reason == ShapeCellInsertRejectReason::OccupancySkipped;

    if (preflight.reason != ShapeCellInsertRejectReason::OutOfRangeBody) {
        const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
        const vec3 position = bodies.positions[bodyIndex];
        const f32 cellSize = clampCellSize(params.cellSize);
        const u32 maxSpan = params.maxCellSpanPerAxis;
        const CollisionShapeType type = shapeType(shapes, shapeIndex);
        if (use2D) {
            CellRange2 range = {};
            if (type == CollisionShapeType::Box) {
                const vec3 halfExtents = worldBoxHalfExtents(bodies, bodyIndex, shapes.params[shapeIndex]);
                const aabb bounds = aabbFromBox(position, halfExtents);
                range = cellRangeFromAabb2D(bounds, cellSize, maxSpan);
            } else {
                const f32 radius = shapeRadius(shapes, shapeIndex);
                range = cellRangeFromSphere2D({position.x, position.y}, radius, cellSize, maxSpan);
            }
            preflight.occupancyCount = estimateCellOccupancyCount(range);
        } else {
            CellRange3 range = {};
            if (type == CollisionShapeType::Box) {
                const vec3 halfExtents = worldBoxHalfExtents(bodies, bodyIndex, shapes.params[shapeIndex]);
                range = cellRangeFromBox(position, halfExtents, cellSize, maxSpan);
            } else {
                const f32 radius = shapeRadius(shapes, shapeIndex);
                range = cellRangeFromSphere(position, radius, cellSize, maxSpan);
            }
            preflight.occupancyCount = estimateCellOccupancyCount(range);
        }
    }

    return preflight;
}

void populateShapeCells(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D,
    CellBuckets& cells) {
    const ShapeCellInsertPreflight insertPreflight =
        preflightShapeCellInsertImpl(shapeIndex, bodies, shapes, params, use2D);
    if (!insertPreflight.canInsert()) {
        return;
    }

    const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
    const vec3 position = bodies.positions[bodyIndex];
    const f32 cellSize = clampCellSize(params.cellSize);
    const u32 tableSize = clampTableSize(params.tableSize);
    const u32 maxSpan = params.maxCellSpanPerAxis;
    const CollisionShapeType type = shapeType(shapes, shapeIndex);

    if (use2D) {
        CellRange2 range = {};
        if (type == CollisionShapeType::Box) {
            const vec3 halfExtents = worldBoxHalfExtents(bodies, bodyIndex, shapes.params[shapeIndex]);
            const aabb bounds = aabbFromBox(position, halfExtents);
            range = cellRangeFromAabb2D(bounds, cellSize, maxSpan);
        } else {
            const f32 radius = shapeRadius(shapes, shapeIndex);
            range = cellRangeFromSphere2D({position.x, position.y}, radius, cellSize, maxSpan);
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
        const vec3 halfExtents = worldBoxHalfExtents(bodies, bodyIndex, shapes.params[shapeIndex]);
        range = cellRangeFromBox(position, halfExtents, cellSize, maxSpan);
    } else {
        const f32 radius = shapeRadius(shapes, shapeIndex);
        range = cellRangeFromSphere(position, radius, cellSize, maxSpan);
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
    if (!shouldRunMergePairsIntoBuffer(pairs, buffer)) {
        return;
    }

    for (const CandidatePair& pair : pairs) {
        if (!preflightPairBufferPush(buffer, pair.bodyA, pair.bodyB).canPush()) {
            break;
        }
        buffer.push(pair.bodyA, pair.bodyB);
    }
}

void dedupeBuffer(PairBufferSoA& buffer) {
    if (!shouldRunDedupeBroadphase(buffer) || !shouldRunPairBufferDedupe(buffer)) {
        return;
    }

    std::vector<CandidatePair> pairs = buffer.toVector();
    dedupePairs(pairs);

    buffer.clear();
    for (const CandidatePair& pair : pairs) {
        buffer.push(pair.bodyA, pair.bodyB);
    }
}

constexpr u32 kInvalidShapeIndex = 0xFFFFFFFFu;

/// Shape index for a body: shapes are usually added one per body in body order, so try that first.
u32 bodyShapeIndex(const CollisionShapeSoA& shapes, u32 bodyIndex) {
    if (bodyIndex < shapes.count() && shapes.bodyIndices[bodyIndex] == bodyIndex) {
        return bodyIndex;
    }
    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        if (shapeBodyIndex(shapes, shapeIndex) == bodyIndex) {
            return shapeIndex;
        }
    }
    return kInvalidShapeIndex;
}

/// Same bounds the cell insertion uses, so refine never rejects a pair the grid found overlapping.
aabb bodyShapeBounds(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes, u32 bodyIndex, vec3 position) {
    const u32 shapeIndex = bodyShapeIndex(shapes, bodyIndex);
    if (shapeIndex == kInvalidShapeIndex) {
        return aabbFromSphere(position, 0.5f);
    }
    if (shapeType(shapes, shapeIndex) == CollisionShapeType::Box) {
        return aabbFromBox(position, worldBoxHalfExtents(bodies, bodyIndex, shapes.params[shapeIndex]));
    }
    return aabbFromSphere(position, shapeRadius(shapes, shapeIndex));
}

bool pairPassesCollisionLayers(u32 bodyA, u32 bodyB, const RigidBodySoA& bodies) {
    if (bodyA >= bodies.collisionLayers.size() || bodyB >= bodies.collisionLayers.size()) {
        return true;
    }
    return collisionLayersCollide(bodies.collisionLayers[bodyA], bodies.collisionMasks[bodyA],
                                  bodies.collisionLayers[bodyB], bodies.collisionMasks[bodyB]);
}

bool pairPassesAabbRefine(
    u32 bodyA,
    u32 bodyB,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (!isValidCandidatePair(bodyA, bodyB, bodies.count())) {
        return false;
    }
    if (!pairPassesCollisionLayers(bodyA, bodyB, bodies)) {
        return false;
    }

    return aabbOverlap(bodyShapeBounds(bodies, shapes, bodyA, bodies.positions[bodyA]),
                       bodyShapeBounds(bodies, shapes, bodyB, bodies.positions[bodyB]));
}

void runBroadphaseIntoBufferInternal(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D,
    PairBufferSoA& buffer) {
    buffer.clear();
    if (!preflightBroadphase(bodies, shapes).canRun()) {
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
    // No shared cells still needs the plane merge below: planes are unbounded and never
    // share a cell with bodies far from the plane body's origin.
    if (shouldRunBroadphaseCellPairGen(totalCellSlots)) {
        fuse::jobs::parallel_for(0u, tableSize, kCellGrainSize, [&](u32 cellIndex) {
            if (cells.buckets[cellIndex].empty()) {
                return;
            }
            writePairsForCellSlots(cells.buckets[cellIndex], cellSlotOffsets[cellIndex], buffer);
        });
        buffer.compact();
        dedupeBuffer(buffer);
    }

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

    if (shouldRunBroadphaseMerge(bodies, shapes)) {
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
                if (!pairPassesCollisionLayers(dynamicBody, planeBody, bodies)) {
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

    if (shouldRunPairBufferClamp(buffer)) {
        buffer.applyMaxCapacityClamp();
    }
}

void refineBroadphasePairsParallelImpl(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    PairBufferSoA& buffer) {
    if (!shouldRunRefineBroadphase(bodies, shapes, buffer)) {
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

    if (shouldRunPairBufferCompaction(buffer)) {
        buffer.compact();
    }
}

} // namespace

u32 countUniqueCellOccupants(const std::vector<u32>& occupants) {
    return countUniqueCellOccupantsImpl(occupants);
}

CellPairGenPreflight preflightCellPairGeneration(const std::vector<u32>& occupants) {
    return preflightCellPairGenerationImpl(occupants);
}

bool canSkipCellPairGeneration(const std::vector<u32>& occupants) {
    return !preflightCellPairGeneration(occupants).canGenerate();
}

bool shouldRunCellPairGeneration(const std::vector<u32>& occupants) {
    return preflightCellPairGeneration(occupants).canGenerate();
}

ShapeCellInsertRejectReason shapeCellInsertRejectReason(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {
    return shapeCellInsertRejectReasonImpl(shapeIndex, bodies, shapes, params, use2D);
}

ShapeCellInsertPreflight preflightShapeCellInsert(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {
    return preflightShapeCellInsertImpl(shapeIndex, bodies, shapes, params, use2D);
}

bool shapeCellInsertRejectsForReason(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D,
    ShapeCellInsertRejectReason expected) {
    return shapeCellInsertRejectReason(shapeIndex, bodies, shapes, params, use2D) == expected;
}

bool canSkipShapeCellInsert(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {
    return !preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();
}

bool shouldRunShapeCellInsert(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {
    return preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();
}

BroadphaseCellPairGenRejectReason broadphaseCellPairGenRejectReason(u32 totalCellSlots) {
    if (totalCellSlots == 0u) {
        return BroadphaseCellPairGenRejectReason::EmptyCells;
    }
    return BroadphaseCellPairGenRejectReason::None;
}

bool broadphaseCellPairGenRejectsForReason(u32 totalCellSlots, BroadphaseCellPairGenRejectReason expected) {
    return broadphaseCellPairGenRejectReason(totalCellSlots) == expected;
}

BroadphaseCellPairGenPreflight preflightBroadphaseCellPairGen(u32 totalCellSlots) {
    BroadphaseCellPairGenPreflight preflight{};
    preflight.reason = broadphaseCellPairGenRejectReason(totalCellSlots);
    preflight.emptyCells = preflight.reason == BroadphaseCellPairGenRejectReason::EmptyCells;
    return preflight;
}

bool canSkipBroadphaseCellPairGen(u32 totalCellSlots) {
    return !preflightBroadphaseCellPairGen(totalCellSlots).canGenerate();
}

bool shouldRunBroadphaseCellPairGen(u32 totalCellSlots) {
    return preflightBroadphaseCellPairGen(totalCellSlots).canGenerate();
}

RefineBroadphaseRejectReason refineBroadphaseRejectReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return RefineBroadphaseRejectReason::EmptyBuffer;
    }
    if (canSkipBroadphase(bodies, shapes)) {
        return RefineBroadphaseRejectReason::EmptyInput;
    }
    if (!buffer.hasValidPairs()) {
        return RefineBroadphaseRejectReason::NoValidPairs;
    }
    return RefineBroadphaseRejectReason::None;
}

bool refineBroadphaseRejectsForReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer,
    RefineBroadphaseRejectReason expected) {
    return refineBroadphaseRejectReason(bodies, shapes, buffer) == expected;
}

RefineBroadphasePreflight preflightRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    RefineBroadphasePreflight preflight{};
    preflight.emptyBuffer = buffer.canSkipSoAIteration();
    preflight.noValidPairs = !buffer.hasValidPairs();
    preflight.emptyInput = canSkipBroadphase(bodies, shapes);
    preflight.reason = refineBroadphaseRejectReason(bodies, shapes, buffer);
    return preflight;
}

bool canSkipRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return !preflightRefineBroadphase(bodies, shapes, buffer).canRefine();
}

bool shouldRunRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return preflightRefineBroadphase(bodies, shapes, buffer).canRefine();
}

bool wouldSkipRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer,
    RefineBroadphaseRejectReason* reason) {
    const RefineBroadphaseRejectReason reject = refineBroadphaseRejectReason(bodies, shapes, buffer);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject != RefineBroadphaseRejectReason::None;
}

DedupeBroadphaseRejectReason dedupeBroadphaseRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return DedupeBroadphaseRejectReason::EmptyBuffer;
    }
    if (buffer.activeCount <= 1u) {
        return DedupeBroadphaseRejectReason::SinglePair;
    }
    return DedupeBroadphaseRejectReason::None;
}

bool dedupeBroadphaseRejectsForReason(const PairBufferSoA& buffer, DedupeBroadphaseRejectReason expected) {
    return dedupeBroadphaseRejectReason(buffer) == expected;
}

DedupeBroadphasePreflight preflightDedupeBroadphase(const PairBufferSoA& buffer) {
    DedupeBroadphasePreflight preflight{};
    preflight.emptyBuffer = buffer.canSkipSoAIteration();
    preflight.singlePair = !preflight.emptyBuffer && buffer.activeCount <= 1u;
    preflight.reason = dedupeBroadphaseRejectReason(buffer);
    return preflight;
}

bool shouldRunDedupeBroadphase(const PairBufferSoA& buffer) {
    return preflightDedupeBroadphase(buffer).canDedupe();
}

bool canSkipDedupeBroadphase(const PairBufferSoA& buffer) {
    return !shouldRunDedupeBroadphase(buffer);
}

bool wouldSkipDedupeBroadphase(const PairBufferSoA& buffer, DedupeBroadphaseRejectReason* reason) {
    const DedupeBroadphaseRejectReason reject = dedupeBroadphaseRejectReason(buffer);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject != DedupeBroadphaseRejectReason::None;
}

BroadphaseMergePreflight preflightBroadphaseMerge(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    BroadphaseMergePreflight preflight{};
    bool hasPlaneBodies = false;
    bool hasDynamicBodies = false;

    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        const u32 bodyIndex = shapes.bodyIndices[shapeIndex];
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const CollisionShapeType type = static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
        if (type == CollisionShapeType::Plane) {
            hasPlaneBodies = true;
        } else if ((bodies.flags[bodyIndex] & RB_STATIC) == 0) {
            hasDynamicBodies = true;
        }
        if (hasPlaneBodies && hasDynamicBodies) {
            break;
        }
    }

    preflight.emptyPlaneBodies = !hasPlaneBodies;
    preflight.emptyDynamicBodies = !hasDynamicBodies;
    if (preflight.emptyPlaneBodies) {
        preflight.reason = BroadphaseMergeRejectReason::EmptyPlaneBodies;
    } else if (preflight.emptyDynamicBodies) {
        preflight.reason = BroadphaseMergeRejectReason::EmptyDynamicBodies;
    } else {
        preflight.reason = BroadphaseMergeRejectReason::None;
    }
    return preflight;
}

BroadphaseMergeRejectReason mergeBroadphaseRejectReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflightBroadphaseMerge(bodies, shapes).reason;
}

bool mergeBroadphaseRejectsForReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    BroadphaseMergeRejectReason expected) {
    return mergeBroadphaseRejectReason(bodies, shapes) == expected;
}

bool canSkipBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    return !preflightBroadphaseMerge(bodies, shapes).canMerge();
}

bool shouldRunBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    return preflightBroadphaseMerge(bodies, shapes).canMerge();
}

bool wouldSkipBroadphaseMerge(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    BroadphaseMergeRejectReason* reason) {
    const BroadphaseMergeRejectReason reject = mergeBroadphaseRejectReason(bodies, shapes);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject != BroadphaseMergeRejectReason::None;
}

const char* mergePairsIntoBufferRejectReasonName(MergePairsIntoBufferRejectReason reason) {
    switch (reason) {
    case MergePairsIntoBufferRejectReason::None:
        return "None";
    case MergePairsIntoBufferRejectReason::EmptyPairs:
        return "EmptyPairs";
    case MergePairsIntoBufferRejectReason::BufferFull:
        return "BufferFull";
    }
    return "Unknown";
}

MergePairsIntoBufferRejectReason mergePairsIntoBufferRejectReason(
    const std::vector<CandidatePair>& pairs,
    const PairBufferSoA& buffer) {
    if (pairs.empty()) {
        return MergePairsIntoBufferRejectReason::EmptyPairs;
    }
    if (buffer.isFull()) {
        return MergePairsIntoBufferRejectReason::BufferFull;
    }
    return MergePairsIntoBufferRejectReason::None;
}

bool mergePairsIntoBufferRejectsForReason(
    const std::vector<CandidatePair>& pairs,
    const PairBufferSoA& buffer,
    MergePairsIntoBufferRejectReason expected) {
    return mergePairsIntoBufferRejectReason(pairs, buffer) == expected;
}

MergePairsIntoBufferPreflight preflightMergePairsIntoBuffer(
    const std::vector<CandidatePair>& pairs,
    const PairBufferSoA& buffer) {
    MergePairsIntoBufferPreflight preflight{};
    preflight.reason = mergePairsIntoBufferRejectReason(pairs, buffer);
    preflight.emptyPairs = preflight.reason == MergePairsIntoBufferRejectReason::EmptyPairs;
    preflight.bufferFull = preflight.reason == MergePairsIntoBufferRejectReason::BufferFull;
    return preflight;
}

bool canSkipMergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, const PairBufferSoA& buffer) {
    return !preflightMergePairsIntoBuffer(pairs, buffer).canMerge();
}

bool shouldRunMergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, const PairBufferSoA& buffer) {
    return preflightMergePairsIntoBuffer(pairs, buffer).canMerge();
}

bool wouldSkipMergePairsIntoBuffer(
    const std::vector<CandidatePair>& pairs,
    const PairBufferSoA& buffer,
    MergePairsIntoBufferRejectReason* reason) {
    const MergePairsIntoBufferRejectReason reject = mergePairsIntoBufferRejectReason(pairs, buffer);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject != MergePairsIntoBufferRejectReason::None;
}

void refineBroadphasePairsParallel(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    PairBufferSoA& buffer) {
    refineBroadphasePairsParallelImpl(bodies, shapes, buffer);
}

bool refineBroadphasePairsParallelWithPreflight(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    PairBufferSoA& buffer) {
    if (!shouldRunRefineBroadphase(bodies, shapes, buffer)) {
        return false;
    }
    refineBroadphasePairsParallelImpl(bodies, shapes, buffer);
    return true;
}

bool dedupeBroadphasePairBufferWithPreflight(PairBufferSoA& buffer) {
    if (!shouldRunDedupeBroadphase(buffer)) {
        return false;
    }
    dedupeBuffer(buffer);
    return true;
}

void mergePairsIntoBufferWithPreflight(const std::vector<CandidatePair>& pairs, PairBufferSoA& buffer) {
    mergePairsIntoBuffer(pairs, buffer);
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

namespace {

constexpr u32 kGridMaxCellsPerShape = 64u; // larger shapes are tested against everything
constexpr s64 kGridCellBias = 1 << 20;

s64 gridCoord(f32 value, f32 invCell) {
    return static_cast<s64>(std::floor(value * invCell));
}

u64 gridKey(s64 x, s64 y, s64 z) {
    const u64 mask = (1ull << 21u) - 1ull;
    return ((static_cast<u64>(x + kGridCellBias) & mask) << 42u) |
           ((static_cast<u64>(y + kGridCellBias) & mask) << 21u) | (static_cast<u64>(z + kGridCellBias) & mask);
}

bool gridPairWanted(const RigidBodySoA& bodies, u32 a, u32 b) {
    const bool staticA = (bodies.flags[a] & RB_STATIC) != 0u;
    const bool staticB = (bodies.flags[b] & RB_STATIC) != 0u;
    return !(staticA && staticB) && collisionLayersCollide(bodies.collisionLayers[a], bodies.collisionMasks[a],
                                                            bodies.collisionLayers[b], bodies.collisionMasks[b]);
}

} // namespace

void GridBroadphase::findPairs(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes, f32 cellSize,
                               std::vector<CandidatePair>& out, f32 margin) {
    const vec3 pad{0.5f * margin, 0.5f * margin, 0.5f * margin};
    out.clear();
    const u32 bodyCount = bodies.count();
    const f32 invCell = 1.f / clampCellSize(cellSize);
    m_bounds.resize(bodyCount);
    m_hasBounds.assign(bodyCount, 0u);
    m_entries.clear();
    m_planeShapes.clear();
    m_large.clear();

    for (u32 shape = 0; shape < shapes.count(); ++shape) {
        const u32 body = shapes.bodyIndices[shape];
        if (body >= bodyCount || m_hasBounds[body] != 0u) {
            continue; // one shape per body on this path
        }
        const vec3 p = bodies.positions[body];
        const vec3 params = shapes.params[shape];
        switch (static_cast<CollisionShapeType>(shapes.types[shape])) {
        case CollisionShapeType::Plane:
            m_planeShapes.push_back(shape);
            continue;
        case CollisionShapeType::Box:
            m_bounds[body] = aabbFromBox(p, orientedBoxHalfExtents(bodies.orientations[body], params));
            break;
        case CollisionShapeType::Capsule:
            m_bounds[body] = aabbFromBox(p, orientedCapsuleHalfExtents(bodies.orientations[body], params));
            break;
        default:
            m_bounds[body] = aabbFromSphere(p, params.x);
            break;
        }
        m_bounds[body].min -= pad;
        m_bounds[body].max += pad;
        m_hasBounds[body] = 1u;
        const aabb& box = m_bounds[body];
        const s64 x0 = gridCoord(box.min.x, invCell), x1 = gridCoord(box.max.x, invCell);
        const s64 y0 = gridCoord(box.min.y, invCell), y1 = gridCoord(box.max.y, invCell);
        const s64 z0 = gridCoord(box.min.z, invCell), z1 = gridCoord(box.max.z, invCell);
        const u64 cells = static_cast<u64>(x1 - x0 + 1) * static_cast<u64>(y1 - y0 + 1) * static_cast<u64>(z1 - z0 + 1);
        if (cells > kGridMaxCellsPerShape) {
            m_large.push_back(body);
            continue;
        }
        for (s64 x = x0; x <= x1; ++x) {
            for (s64 y = y0; y <= y1; ++y) {
                for (s64 z = z0; z <= z1; ++z) {
                    m_entries.push_back({gridKey(x, y, z), body});
                }
            }
        }
    }

    std::sort(m_entries.begin(), m_entries.end(),
              [](const Entry& a, const Entry& b) { return a.cell < b.cell || (a.cell == b.cell && a.body < b.body); });
    for (usize begin = 0; begin < m_entries.size();) {
        usize end = begin + 1;
        while (end < m_entries.size() && m_entries[end].cell == m_entries[begin].cell) {
            ++end;
        }
        const u64 cell = m_entries[begin].cell;
        for (usize i = begin; i < end; ++i) {
            const u32 a = m_entries[i].body;
            const aabb& boxA = m_bounds[a];
            for (usize j = i + 1; j < end; ++j) {
                const u32 b = m_entries[j].body;
                const aabb& boxB = m_bounds[b];
                if (!aabbOverlap(boxA, boxB) || !gridPairWanted(bodies, a, b)) {
                    continue;
                }
                // Report from the cell holding the intersection's minimum corner only.
                const u64 owner = gridKey(gridCoord(std::max(boxA.min.x, boxB.min.x), invCell),
                                          gridCoord(std::max(boxA.min.y, boxB.min.y), invCell),
                                          gridCoord(std::max(boxA.min.z, boxB.min.z), invCell));
                if (owner == cell) {
                    out.push_back(canonicalPair(a, b));
                }
            }
        }
        begin = end;
    }

    // Oversized shapes: exact tests against every bounded body (and each other once).
    for (usize i = 0; i < m_large.size(); ++i) {
        const u32 a = m_large[i];
        for (u32 b = 0; b < bodyCount; ++b) {
            if (b == a || m_hasBounds[b] == 0u) {
                continue;
            }
            const bool otherLarge = std::find(m_large.begin(), m_large.end(), b) != m_large.end();
            if ((otherLarge && b < a) || !aabbOverlap(m_bounds[a], m_bounds[b]) || !gridPairWanted(bodies, a, b)) {
                continue;
            }
            out.push_back(canonicalPair(a, b));
        }
    }

    // Planes: every non-static body whose bounds reach the plane.
    for (const u32 planeShape : m_planeShapes) {
        const u32 plane = shapes.bodyIndices[planeShape];
        const vec3 n = shapes.params[planeShape];
        const f32 d = shapes.scalars[planeShape];
        for (u32 b = 0; b < bodyCount; ++b) {
            if (m_hasBounds[b] == 0u || (bodies.flags[b] & RB_STATIC) != 0u || !gridPairWanted(bodies, plane, b)) {
                continue;
            }
            const aabb& box = m_bounds[b];
            const vec3 center = (box.min + box.max) * 0.5f;
            const vec3 half = (box.max - box.min) * 0.5f;
            const f32 extent = std::fabs(n.x) * half.x + std::fabs(n.y) * half.y + std::fabs(n.z) * half.z;
            if (center.dot(n) - d <= extent + pad.x) {
                out.push_back(canonicalPair(plane, b));
            }
        }
    }
}

} // namespace fuse::physics::broadphase
