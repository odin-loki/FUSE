#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <vector>

namespace fuse::physics::broadphase {

/// Diagnostic reason a candidate pair is rejected before broadphase output (B4.2 deepen).
enum class CandidatePairRejectReason : u8 {
    None = 0,
    SelfPair,
    OutOfRangeBody,
};

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

/// Empty-pair guard: true when both indices refer to the same body.
FUSE_PHYSICS_INLINE bool isEmptyCandidatePair(u32 bodyA, u32 bodyB) {
    return bodyA == bodyB;
}

/// Candidate-pair validity stub: rejects self-pairs and optional out-of-range indices.
FUSE_PHYSICS_INLINE bool isValidCandidatePair(u32 bodyA, u32 bodyB, u32 bodyCount = 0u) {
    if (isEmptyCandidatePair(bodyA, bodyB)) {
        return false;
    }
    if (bodyCount == 0u) {
        return true;
    }
    return bodyA < bodyCount && bodyB < bodyCount;
}

FUSE_PHYSICS_INLINE bool isEmptyCandidatePair(const CandidatePair& pair) {
    return isEmptyCandidatePair(pair.bodyA, pair.bodyB);
}

FUSE_PHYSICS_INLINE bool isValidCandidatePair(const CandidatePair& pair, u32 bodyCount = 0u) {
    return isValidCandidatePair(pair.bodyA, pair.bodyB, bodyCount);
}

/// Returns the first reject reason for a pair, or `None` when the pair may be emitted.
FUSE_PHYSICS_INLINE CandidatePairRejectReason candidatePairRejectReason(u32 bodyA, u32 bodyB, u32 bodyCount = 0u) {
    if (isEmptyCandidatePair(bodyA, bodyB)) {
        return CandidatePairRejectReason::SelfPair;
    }
    if (bodyCount > 0u && (bodyA >= bodyCount || bodyB >= bodyCount)) {
        return CandidatePairRejectReason::OutOfRangeBody;
    }
    return CandidatePairRejectReason::None;
}

FUSE_PHYSICS_INLINE CandidatePairRejectReason candidatePairRejectReason(const CandidatePair& pair, u32 bodyCount = 0u) {
    return candidatePairRejectReason(pair.bodyA, pair.bodyB, bodyCount);
}

/// Clamp non-positive cell sizes to the broadphase stub default.
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

FUSE_PHYSICS_INLINE s32 cellAxisSpan(s32 minCell, s32 maxCell) {
    return maxCell >= minCell ? (maxCell - minCell + 1) : 0;
}

FUSE_PHYSICS_INLINE bool cellRangeIsEmpty(const CellRange3& range) {
    return range.minCell.x > range.maxCell.x || range.minCell.y > range.maxCell.y ||
           range.minCell.z > range.maxCell.z;
}

FUSE_PHYSICS_INLINE bool cellRangeIsEmpty(const CellRange2& range) {
    return range.minCell.x > range.maxCell.x || range.minCell.y > range.maxCell.y;
}

FUSE_PHYSICS_INLINE u32 cellRangeVolume3(const CellRange3& range) {
    if (cellRangeIsEmpty(range)) {
        return 0u;
    }
    return static_cast<u32>(cellAxisSpan(range.minCell.x, range.maxCell.x)) *
           static_cast<u32>(cellAxisSpan(range.minCell.y, range.maxCell.y)) *
           static_cast<u32>(cellAxisSpan(range.minCell.z, range.maxCell.z));
}

FUSE_PHYSICS_INLINE u32 cellRangeVolume2(const CellRange2& range) {
    if (cellRangeIsEmpty(range)) {
        return 0u;
    }
    return static_cast<u32>(cellAxisSpan(range.minCell.x, range.maxCell.x)) *
           static_cast<u32>(cellAxisSpan(range.minCell.y, range.maxCell.y));
}

/// True when a hash cell cannot emit candidate pairs (single occupant or empty).
FUSE_PHYSICS_INLINE bool shouldSkipCellPairGeneration(u32 occupantCount) {
    return occupantCount < 2u;
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
    const f32 invCell = cellSize > 0.f ? 1.f / cellSize : 1.f;
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
    const f32 invCell = cellSize > 0.f ? 1.f / cellSize : 1.f;
    return {
        static_cast<s32>(std::floor(position.x * invCell)),
        static_cast<s32>(std::floor(position.y * invCell)),
    };
}

FUSE_PHYSICS_INLINE CellRange3 cellRangeFromSphere(vec3 center, f32 radius, f32 cellSize, u32 maxSpanPerAxis = 64u) {
    const f32 cell = clampCellSize(cellSize);
    CellRange3 range = {
        worldToCell({center.x - radius, center.y - radius, center.z - radius}, cell),
        worldToCell({center.x + radius, center.y + radius, center.z + radius}, cell),
    };
    return clampCellRange3(range, maxSpanPerAxis);
}

FUSE_PHYSICS_INLINE CellRange2 cellRangeFromSphere2D(vec2 center, f32 radius, f32 cellSize, u32 maxSpanPerAxis = 64u) {
    const f32 cell = clampCellSize(cellSize);
    CellRange2 range = {
        worldToCell2D({center.x - radius, center.y - radius}, cell),
        worldToCell2D({center.x + radius, center.y + radius}, cell),
    };
    return clampCellRange2(range, maxSpanPerAxis);
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

FUSE_PHYSICS_INLINE bool aabbOverlap(const aabb& a, const aabb& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

/// Sphere overlap stub via expanded AABB test (CPU reference for CUDA broadphase refine).
FUSE_PHYSICS_INLINE bool sphereAabbOverlap(vec3 centerA, f32 radiusA, vec3 centerB, f32 radiusB) {
    return aabbOverlap(aabbFromSphere(centerA, radiusA), aabbFromSphere(centerB, radiusB));
}

struct PairBufferSoA;

/// True when broadphase input has no bodies or shapes to process.
FUSE_PHYSICS_INLINE bool shouldSkipBroadphaseInput(u32 bodyCount, u32 shapeCount) {
    return bodyCount == 0u || shapeCount == 0u;
}

/// True when refine can skip scanning pair slots (empty buffer or missing scene data).
FUSE_PHYSICS_INLINE bool shouldSkipBroadphaseRefine(
    u32 activeCount,
    u32 pairSlotCount,
    u32 bodyCount,
    u32 shapeCount) {
    return (activeCount == 0u && pairSlotCount == 0u) || shouldSkipBroadphaseInput(bodyCount, shapeCount);
}

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
