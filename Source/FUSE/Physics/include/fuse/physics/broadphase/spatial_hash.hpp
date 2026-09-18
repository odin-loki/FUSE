#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <vector>

namespace fuse::physics::broadphase {

struct SpatialHashParams {
    f32 cellSize = 2.f;
    u32 tableSize = 1024;
    u32 bodyCount = 0;
    /// Per-axis cell span clamp for shape occupancy iteration (0 = unlimited stub).
    u32 maxCellSpanPerAxis = 64u;
};

struct CandidatePair {
    u32 bodyA = 0;
    u32 bodyB = 0;
};

/// Diagnostic reason a broadphase candidate pair is rejected (B4.2 deepen).
enum class CandidatePairRejectReason : u8 {
    None = 0,
    SelfPair,
    OutOfRangeBody,
};

/// Human-readable label for diagnostics and test assertions (B4.2 deepen).
const char* candidatePairRejectReasonName(CandidatePairRejectReason reason);

/// Returns the first reject reason for a candidate pair, or `None` when valid.
FUSE_PHYSICS_INLINE CandidatePairRejectReason candidatePairRejectReason(
    u32 bodyA,
    u32 bodyB,
    u32 bodyCount = 0u) {
    if (bodyA == bodyB) {
        return CandidatePairRejectReason::SelfPair;
    }
    if (bodyCount > 0u && (bodyA >= bodyCount || bodyB >= bodyCount)) {
        return CandidatePairRejectReason::OutOfRangeBody;
    }
    return CandidatePairRejectReason::None;
}

FUSE_PHYSICS_INLINE CandidatePairRejectReason candidatePairRejectReason(
    const CandidatePair& pair,
    u32 bodyCount = 0u) {
    return candidatePairRejectReason(pair.bodyA, pair.bodyB, bodyCount);
}

/// Empty-pair guard: true when both indices refer to the same body.
FUSE_PHYSICS_INLINE bool isEmptyCandidatePair(u32 bodyA, u32 bodyB) {
    return candidatePairRejectReason(bodyA, bodyB) == CandidatePairRejectReason::SelfPair;
}

/// Candidate-pair validity stub: rejects self-pairs and optional out-of-range indices.
FUSE_PHYSICS_INLINE bool isValidCandidatePair(u32 bodyA, u32 bodyB, u32 bodyCount = 0u) {
    return candidatePairRejectReason(bodyA, bodyB, bodyCount) == CandidatePairRejectReason::None;
}

FUSE_PHYSICS_INLINE bool isEmptyCandidatePair(const CandidatePair& pair) {
    return isEmptyCandidatePair(pair.bodyA, pair.bodyB);
}

FUSE_PHYSICS_INLINE bool isValidCandidatePair(const CandidatePair& pair, u32 bodyCount = 0u) {
    return isValidCandidatePair(pair.bodyA, pair.bodyB, bodyCount);
}

/// Returns true when `candidatePairRejectReason` matches `expected` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool candidatePairRejectsForReason(
    u32 bodyA,
    u32 bodyB,
    u32 bodyCount,
    CandidatePairRejectReason expected) {
    return candidatePairRejectReason(bodyA, bodyB, bodyCount) == expected;
}

FUSE_PHYSICS_INLINE bool candidatePairRejectsForReason(
    const CandidatePair& pair,
    u32 bodyCount,
    CandidatePairRejectReason expected) {
    return candidatePairRejectsForReason(pair.bodyA, pair.bodyB, bodyCount, expected);
}

/// Empty-set guard: true when broadphase has no bodies or no collision shapes.
FUSE_PHYSICS_INLINE bool isEmptyBroadphaseInput(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return bodies.count() == 0u || shapes.count() == 0u;
}

/// True when the broadphase pipeline may early-out before hash build (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return isEmptyBroadphaseInput(bodies, shapes);
}

/// Clamp cell size to a positive stub default (broadphase occupancy guard).
FUSE_PHYSICS_INLINE f32 clampCellSize(f32 cellSize) {
    return cellSize > 0.f ? cellSize : 1.f;
}

/// Clamp hash table size to at least one bucket (broadphase stub guard).
FUSE_PHYSICS_INLINE u32 clampTableSize(u32 tableSize) {
    return tableSize > 0u ? tableSize : 1u;
}

/// Clamp hash key into `[0, tableSize)`.
FUSE_PHYSICS_INLINE u32 clampHashKey(u32 key, u32 tableSize) {
    return key % clampTableSize(tableSize);
}

/// Clamp a single cell coordinate between inclusive bounds.
FUSE_PHYSICS_INLINE s32 clampCellCoord(s32 value, s32 minBound, s32 maxBound) {
    if (value < minBound) {
        return minBound;
    }
    if (value > maxBound) {
        return maxBound;
    }
    return value;
}

struct CellRange3 {
    ivec3 minCell{};
    ivec3 maxCell{};
};

struct CellRange2 {
    ivec2 minCell{};
    ivec2 maxCell{};
};

/// True when any axis has an inverted min/max span (empty occupancy iteration).
FUSE_PHYSICS_INLINE bool isEmptyCellRange(const CellRange3& range) {
    return range.minCell.x > range.maxCell.x || range.minCell.y > range.maxCell.y ||
           range.minCell.z > range.maxCell.z;
}

FUSE_PHYSICS_INLINE bool isEmptyCellRange(const CellRange2& range) {
    return range.minCell.x > range.maxCell.x || range.minCell.y > range.maxCell.y;
}

/// Per-axis inclusive cell span for occupancy budgeting stubs.
FUSE_PHYSICS_INLINE ivec3 cellSpanPerAxis(const CellRange3& range) {
    if (isEmptyCellRange(range)) {
        return {};
    }
    return {
        range.maxCell.x - range.minCell.x + 1,
        range.maxCell.y - range.minCell.y + 1,
        range.maxCell.z - range.minCell.z + 1,
    };
}

FUSE_PHYSICS_INLINE ivec2 cellSpanPerAxis(const CellRange2& range) {
    if (isEmptyCellRange(range)) {
        return {};
    }
    return {
        range.maxCell.x - range.minCell.x + 1,
        range.maxCell.y - range.minCell.y + 1,
    };
}

/// Occupancy cell count stub for budgeting (0 when the range is empty).
FUSE_PHYSICS_INLINE u32 estimateCellOccupancyCount(const CellRange3& range) {
    if (isEmptyCellRange(range)) {
        return 0u;
    }
    const ivec3 span = cellSpanPerAxis(range);
    return static_cast<u32>(span.x) * static_cast<u32>(span.y) * static_cast<u32>(span.z);
}

FUSE_PHYSICS_INLINE u32 estimateCellOccupancyCount(const CellRange2& range) {
    if (isEmptyCellRange(range)) {
        return 0u;
    }
    const ivec2 span = cellSpanPerAxis(range);
    return static_cast<u32>(span.x) * static_cast<u32>(span.y);
}

/// True when `maxCells == 0` (unlimited occupancy budget stub, B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool hasUnlimitedCellOccupancyBudget(u32 maxCells) {
    return maxCells == 0u;
}

/// Largest inclusive per-axis span for occupancy budgeting (B4.2 deepen pass).
FUSE_PHYSICS_INLINE u32 maxCellSpanAxis(const CellRange3& range) {
    if (isEmptyCellRange(range)) {
        return 0u;
    }
    const ivec3 span = cellSpanPerAxis(range);
    const s32 axisMax = std::max(span.x, std::max(span.y, span.z));
    return static_cast<u32>(axisMax);
}

FUSE_PHYSICS_INLINE u32 maxCellSpanAxis(const CellRange2& range) {
    if (isEmptyCellRange(range)) {
        return 0u;
    }
    const ivec2 span = cellSpanPerAxis(range);
    return static_cast<u32>(std::max(span.x, span.y));
}

/// Cell-capacity guard: true when any axis span exceeds `maxSpanPerAxis` (0 = unlimited).
FUSE_PHYSICS_INLINE bool exceedsMaxCellSpanPerAxis(const CellRange3& range, u32 maxSpanPerAxis) {
    if (maxSpanPerAxis == 0u) {
        return false;
    }
    return maxCellSpanAxis(range) > maxSpanPerAxis;
}

FUSE_PHYSICS_INLINE bool exceedsMaxCellSpanPerAxis(const CellRange2& range, u32 maxSpanPerAxis) {
    if (maxSpanPerAxis == 0u) {
        return false;
    }
    return maxCellSpanAxis(range) > maxSpanPerAxis;
}

/// Cell-capacity guard: true when occupancy exceeds `maxCells` (0 = unlimited budget).
FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange3& range, u32 maxCells) {
    if (maxCells == 0u) {
        return false;
    }
    return estimateCellOccupancyCount(range) > maxCells;
}

FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange2& range, u32 maxCells) {
    if (maxCells == 0u) {
        return false;
    }
    return estimateCellOccupancyCount(range) > maxCells;
}

/// Inverse of `exceedsCellOccupancyBudget` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool cellOccupancyWithinBudget(const CellRange3& range, u32 maxCells) {
    return !exceedsCellOccupancyBudget(range, maxCells);
}

FUSE_PHYSICS_INLINE bool cellOccupancyWithinBudget(const CellRange2& range, u32 maxCells) {
    return !exceedsCellOccupancyBudget(range, maxCells);
}

/// Pair-list sizing stub: unique-body pair count n*(n-1)/2 (0 when n < 2).
FUSE_PHYSICS_INLINE u32 estimatePairCountForUniqueBodies(u32 uniqueBodyCount) {
    return uniqueBodyCount > 1u ? uniqueBodyCount * (uniqueBodyCount - 1u) / 2u : 0u;
}

/// Clamp broadphase params to safe stub defaults (positive cell size, at least one bucket).
FUSE_PHYSICS_INLINE SpatialHashParams normalizeSpatialHashParams(SpatialHashParams params) {
    params.cellSize = clampCellSize(params.cellSize);
    params.tableSize = clampTableSize(params.tableSize);
    return params;
}

/// Limit per-axis cell span from the range center (CUDA occupancy iteration guard stub).
FUSE_PHYSICS_INLINE CellRange3 clampCellRange3(CellRange3 range, u32 maxSpanPerAxis) {
    if (maxSpanPerAxis == 0u) {
        return range;
    }

    const ivec3 center = {
        (range.minCell.x + range.maxCell.x) / 2,
        (range.minCell.y + range.maxCell.y) / 2,
        (range.minCell.z + range.maxCell.z) / 2,
    };
    const s32 halfSpan = static_cast<s32>(maxSpanPerAxis / 2u);
    range.minCell.x = clampCellCoord(range.minCell.x, center.x - halfSpan, center.x + halfSpan);
    range.maxCell.x = clampCellCoord(range.maxCell.x, center.x - halfSpan, center.x + halfSpan);
    range.minCell.y = clampCellCoord(range.minCell.y, center.y - halfSpan, center.y + halfSpan);
    range.maxCell.y = clampCellCoord(range.maxCell.y, center.y - halfSpan, center.y + halfSpan);
    range.minCell.z = clampCellCoord(range.minCell.z, center.z - halfSpan, center.z + halfSpan);
    range.maxCell.z = clampCellCoord(range.maxCell.z, center.z - halfSpan, center.z + halfSpan);
    return range;
}

FUSE_PHYSICS_INLINE CellRange2 clampCellRange2(CellRange2 range, u32 maxSpanPerAxis) {
    if (maxSpanPerAxis == 0u) {
        return range;
    }

    const ivec2 center = {
        (range.minCell.x + range.maxCell.x) / 2,
        (range.minCell.y + range.maxCell.y) / 2,
    };
    const s32 halfSpan = static_cast<s32>(maxSpanPerAxis / 2u);
    range.minCell.x = clampCellCoord(range.minCell.x, center.x - halfSpan, center.x + halfSpan);
    range.maxCell.x = clampCellCoord(range.maxCell.x, center.x - halfSpan, center.x + halfSpan);
    range.minCell.y = clampCellCoord(range.minCell.y, center.y - halfSpan, center.y + halfSpan);
    range.maxCell.y = clampCellCoord(range.maxCell.y, center.y - halfSpan, center.y + halfSpan);
    return range;
}

FUSE_PHYSICS_INLINE u32 spatialHash(s32 cx, s32 cy, s32 cz, u32 tableSize) {
    constexpr u32 p1 = 73856093u;
    constexpr u32 p2 = 19349663u;
    constexpr u32 p3 = 83492791u;
    const u32 hash = (static_cast<u32>(cx * p1) ^ static_cast<u32>(cy * p2) ^ static_cast<u32>(cz * p3));
    return clampHashKey(hash, tableSize);
}

FUSE_PHYSICS_INLINE ivec3 worldToCell(vec3 position, f32 cellSize) {
    const f32 invCell = 1.f / clampCellSize(cellSize);
    return {
        static_cast<s32>(std::floor(position.x * invCell)),
        static_cast<s32>(std::floor(position.y * invCell)),
        static_cast<s32>(std::floor(position.z * invCell)),
    };
}

FUSE_PHYSICS_INLINE u32 spatialHash2D(s32 cx, s32 cy, u32 tableSize) {
    constexpr u32 p1 = 73856093u;
    constexpr u32 p2 = 19349663u;
    const u32 hash = static_cast<u32>(cx * p1) ^ static_cast<u32>(cy * p2);
    return clampHashKey(hash, tableSize);
}

FUSE_PHYSICS_INLINE ivec2 worldToCell2D(vec2 position, f32 cellSize) {
    const f32 invCell = 1.f / clampCellSize(cellSize);
    return {
        static_cast<s32>(std::floor(position.x * invCell)),
        static_cast<s32>(std::floor(position.y * invCell)),
    };
}

FUSE_PHYSICS_INLINE aabb aabbFromSphere(vec3 center, f32 radius) {
    return {
        {center.x - radius, center.y - radius, center.z - radius},
        {center.x + radius, center.y + radius, center.z + radius},
    };
}

FUSE_PHYSICS_INLINE aabb aabbFromBox(vec3 center, vec3 halfExtents) {
    return {
        {center.x - halfExtents.x, center.y - halfExtents.y, center.z - halfExtents.z},
        {center.x + halfExtents.x, center.y + halfExtents.y, center.z + halfExtents.z},
    };
}

FUSE_PHYSICS_INLINE CellRange3 cellRangeFromAabb(const aabb& bounds, f32 cellSize, u32 maxSpanPerAxis = 64u) {
    const f32 cell = clampCellSize(cellSize);
    CellRange3 range = {
        worldToCell(bounds.min, cell),
        worldToCell(bounds.max, cell),
    };
    return clampCellRange3(range, maxSpanPerAxis);
}

FUSE_PHYSICS_INLINE CellRange2 cellRangeFromAabb2D(const aabb& bounds, f32 cellSize, u32 maxSpanPerAxis = 64u) {
    const f32 cell = clampCellSize(cellSize);
    CellRange2 range = {
        worldToCell2D({bounds.min.x, bounds.min.y}, cell),
        worldToCell2D({bounds.max.x, bounds.max.y}, cell),
    };
    return clampCellRange2(range, maxSpanPerAxis);
}

FUSE_PHYSICS_INLINE CellRange3 cellRangeFromSphere(vec3 center, f32 radius, f32 cellSize, u32 maxSpanPerAxis = 64u) {
    return cellRangeFromAabb(aabbFromSphere(center, radius), cellSize, maxSpanPerAxis);
}

FUSE_PHYSICS_INLINE CellRange2 cellRangeFromSphere2D(vec2 center, f32 radius, f32 cellSize, u32 maxSpanPerAxis = 64u) {
    const f32 cell = clampCellSize(cellSize);
    CellRange2 range = {
        worldToCell2D({center.x - radius, center.y - radius}, cell),
        worldToCell2D({center.x + radius, center.y + radius}, cell),
    };
    return clampCellRange2(range, maxSpanPerAxis);
}

FUSE_PHYSICS_INLINE CellRange3 cellRangeFromBox(vec3 center, vec3 halfExtents, f32 cellSize, u32 maxSpanPerAxis = 64u) {
    return cellRangeFromAabb(aabbFromBox(center, halfExtents), cellSize, maxSpanPerAxis);
}

FUSE_PHYSICS_INLINE bool aabbOverlap(const aabb& a, const aabb& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

/// Sphere overlap stub via expanded AABB test (CPU reference for CUDA broadphase refine).
FUSE_PHYSICS_INLINE bool sphereAabbOverlap(vec3 centerA, f32 radiusA, vec3 centerB, f32 radiusB) {
    return aabbOverlap(aabbFromSphere(centerA, radiusA), aabbFromSphere(centerB, radiusB));
}

struct PairBufferSoA;

/// Job-safe broadphase: parallel shape→cell + per-cell pair generation into reusable SoA slots.
void runBroadphaseIntoBuffer(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    PairBufferSoA& buffer);

/// Job-safe 2D broadphase into reusable SoA pair slots.
void runBroadphase2DIntoBuffer(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    PairBufferSoA& buffer);

/// Parallel pair refine stub: invalidate separated pairs via `sphereAabbOverlap`, then compact.
void refineBroadphasePairsParallel(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    PairBufferSoA& buffer);

/// CPU stub of the CUDA broad-phase pipeline (B4.2).
/// Phase 1 jobifies shape→cell insertion; phase 2 jobifies per-cell candidate generation
/// via `fuse::jobs::parallel_for` (serial when the job scheduler is single-threaded).
std::vector<CandidatePair> runBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params);

/// 2D broad phase for World2D composition (xy plane only).
std::vector<CandidatePair> runBroadphase2D(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params);

} // namespace fuse::physics::broadphase
