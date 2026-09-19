#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <climits>
#include <cmath>
#include <vector>

namespace fuse::physics::broadphase {

/// Diagnostic reason a candidate pair is rejected before broadphase output (B4.2 deepen).
enum class CandidatePairRejectReason : u8 {
    None = 0,
    SelfPair,
    OutOfRangeBody,
};
struct PairBufferSoA;

struct SpatialHashParams {
    f32 cellSize = 2.f;
    u32 tableSize = 1024;
    u32 bodyCount = 0;
    /// Per-axis cell span clamp for shape occupancy iteration (0 = unlimited stub).
    u32 maxCellSpanPerAxis = 64u;
    /// Per-shape cell occupancy budget before hash insertion is skipped (0 = unlimited stub).
    u32 maxCellOccupancy = 0u;
    /// Per-shape total cell occupancy budget (0 = unlimited stub).
    u32 maxCellOccupancyCount = 0u;
    /// Total cell occupancy budget per shape insertion (0 = unlimited stub).
    u32 maxCellOccupancyPerShape = 0u;
    /// Per-shape cell occupancy budget before hash insert (0 = unlimited stub).
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
    AabbSeparated,
    BufferFull,
};

/// Human-readable label for diagnostics and test assertions (B4.2 deepen).
const char* candidatePairRejectReasonName(CandidatePairRejectReason reason);
/// Human-readable label for diagnostics and test assertions (B4.2 deepen pass).
const char* candidate_pair_reject_reason_name(CandidatePairRejectReason reason);

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
    return CandidatePairRejectReason::None;

    const CandidatePair& pair,
    return candidatePairRejectReason(pair.bodyA, pair.bodyB, bodyCount);
/// Diagnostic reason a broadphase candidate pair is rejected (B4.2 deepen follow-up).
enum class CandidateRejectReason : u8 {
    AabbSeparated,
    BufferFull,

/// Human-readable label for candidate reject reasons (logging / tests).
const char* candidateRejectReasonLabel(CandidateRejectReason reason);

/// Empty-pair guard: true when both indices refer to the same body.
FUSE_PHYSICS_INLINE bool isEmptyCandidatePair(u32 bodyA, u32 bodyB) {
    return candidatePairRejectReason(bodyA, bodyB) == CandidatePairRejectReason::SelfPair;
}

/// Out-of-range guard: true when either index is at or beyond `bodyCount`.
FUSE_PHYSICS_INLINE bool isOutOfRangeCandidatePair(u32 bodyA, u32 bodyB, u32 bodyCount) {
    return bodyCount > 0u &&
           candidatePairRejectReason(bodyA, bodyB, bodyCount) == CandidatePairRejectReason::OutOfRangeBody;
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

    const CandidatePair& pair,
    return candidatePairRejectsForReason(pair.bodyA, pair.bodyB, bodyCount, expected);

/// Empty-set guard: true when broadphase has no bodies or no collision shapes.
FUSE_PHYSICS_INLINE bool isEmptyBroadphaseInput(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return bodies.count() == 0u || shapes.count() == 0u;

/// Singleton-set guard: true when fewer than two bodies or shapes can emit any pair.
FUSE_PHYSICS_INLINE bool isSingletonBroadphaseInput(
    return bodies.count() < 2u || shapes.count() < 2u;

/// True when spatial-hash pair generation cannot emit pairs (empty or singleton stub).
FUSE_PHYSICS_INLINE bool canSkipBroadphasePairGeneration(
    return isEmptyBroadphaseInput(bodies, shapes) || isSingletonBroadphaseInput(bodies, shapes);

/// Non-mutating pair-generation predicate — inverse of `canSkipBroadphasePairGeneration` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunBroadphasePairGeneration(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !canSkipBroadphasePairGeneration(bodies, shapes);
}

/// True when the broadphase pipeline may early-out before hash build (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipBroadphase(
    return canSkipBroadphasePairGeneration(bodies, shapes);

/// Non-mutating broadphase launch predicate — inverse of `canSkipBroadphase` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !canSkipBroadphase(bodies, shapes);
}

/// Non-mutating broadphase predicate — inverse of `canSkipBroadphase` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !canSkipBroadphase(bodies, shapes);
}

/// Why broadphase pair generation would early-out (B4.2 deepen follow-up pass).
enum class BroadphaseRejectReason : u8 {
    None = 0,
    EmptyInput,
    SingletonInput,
};

/// Human-readable label for broadphase reject reasons (logging / tests).
const char* broadphaseRejectReasonName(BroadphaseRejectReason reason);

/// Diagnose why broadphase would skip; vacuously succeeds on populated scenes.
FUSE_PHYSICS_INLINE BroadphaseRejectReason broadphaseRejectReason(
    if (isEmptyBroadphaseInput(bodies, shapes)) {
        return BroadphaseRejectReason::EmptyInput;
    if (isSingletonBroadphaseInput(bodies, shapes)) {
        return BroadphaseRejectReason::SingletonInput;
    return BroadphaseRejectReason::None;

/// Returns true when `broadphaseRejectReason` matches `expected` (B4.2 deepen follow-up pass).
FUSE_PHYSICS_INLINE bool broadphaseRejectsForReason(
    const CollisionShapeSoA& shapes,
    BroadphaseRejectReason expected) {
    return broadphaseRejectReason(bodies, shapes) == expected;

/// Read-only broadphase launch diagnostics — no mutation (B4.2 deepen follow-up).
struct BroadphasePreflight {
    BroadphaseRejectReason reason = BroadphaseRejectReason::None;
    bool emptyInput = false;
    bool singletonInput = false;

    bool canRun() const { return reason == BroadphaseRejectReason::None; }

FUSE_PHYSICS_INLINE BroadphasePreflight preflightBroadphase(
    BroadphasePreflight preflight{};
    preflight.reason = broadphaseRejectReason(bodies, shapes);
    preflight.emptyInput = preflight.reason == BroadphaseRejectReason::EmptyInput;
    preflight.singletonInput = preflight.reason == BroadphaseRejectReason::SingletonInput;
    return preflight;

/// Non-mutating broadphase predicate — inverse of `canSkipBroadphase` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunBroadphase(
    return preflightBroadphase(bodies, shapes).canRun();

/// Non-mutating pair-generation predicate — inverse of `canSkipBroadphasePairGeneration` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunBroadphasePairGeneration(
    return !canSkipBroadphasePairGeneration(bodies, shapes);
/// Out-of-range guard: true when either index is at or beyond `bodyCount`.
FUSE_PHYSICS_INLINE bool isOutOfRangeCandidatePair(u32 bodyA, u32 bodyB, u32 bodyCount) {
    return bodyCount > 0u &&
           candidatePairRejectReason(bodyA, bodyB, bodyCount) == CandidatePairRejectReason::OutOfRangeBody;

/// Convenience inverse of `candidatePairRejectReason` — true when the pair must be skipped.
FUSE_PHYSICS_INLINE bool isRejectedCandidatePair(u32 bodyA, u32 bodyB, u32 bodyCount = 0u) {
    return candidatePairRejectReason(bodyA, bodyB, bodyCount) != CandidatePairRejectReason::None;

FUSE_PHYSICS_INLINE bool isRejectedCandidatePair(const CandidatePair& pair, u32 bodyCount = 0u) {
    return isRejectedCandidatePair(pair.bodyA, pair.bodyB, bodyCount);

/// Returns the first reject reason including AABB refine (sphere-expanded AABB overlap stub).
CandidatePairRejectReason candidatePairRejectReason(
    const CollisionShapeSoA& shapes);




/// Empty-set guard: true when broadphase has no bodies or shapes to process.
FUSE_PHYSICS_INLINE bool canSkipBroadphase(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {

/// Const preflight for broadphase input empty-set guard (B4.2 deepen pass).
struct BroadphaseInputPreflight {
    u32 bodyCount = 0;
    u32 shapeCount = 0;
    bool emptyBodies = false;
    bool emptyShapes = false;
    bool skipped = false;

    bool can_build() const { return !skipped; }
};

BroadphaseInputPreflight preflight_broadphase_input(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// True when broadphase should skip before hash build (B4.2 deepen pass).
bool should_skip_broadphase_build(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Read-only broadphase launch diagnostics — no mutation (B4.2 deepen follow-up).
struct BroadphasePreflight {
    bool emptyInput = false;

    bool canRun() const { return !emptyInput; }
};

FUSE_PHYSICS_INLINE BroadphasePreflight preflightBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    BroadphasePreflight preflight{};
    preflight.emptyInput = canSkipBroadphase(bodies, shapes);
    return preflight;
}

/// Clamp cell size to a positive stub default (broadphase occupancy guard).
FUSE_PHYSICS_INLINE f32 clampCellSize(f32 cellSize) {
    return cellSize > 0.f ? cellSize : 1.f;
/// Returns the first reject reason for a pair index pair, or `None` when generation may proceed.
FUSE_PHYSICS_INLINE CandidateRejectReason candidatePairRejectReason(u32 bodyA, u32 bodyB, u32 bodyCount = 0u) {
    if (isEmptyCandidatePair(bodyA, bodyB)) {
        return CandidateRejectReason::SelfPair;
    if (bodyCount > 0u && (bodyA >= bodyCount || bodyB >= bodyCount)) {
        return CandidateRejectReason::OutOfRangeBody;
    return CandidateRejectReason::None;

FUSE_PHYSICS_INLINE CandidateRejectReason candidatePairRejectReason(const CandidatePair& pair, u32 bodyCount = 0u) {
    return candidatePairRejectReason(pair.bodyA, pair.bodyB, bodyCount);

/// Convenience inverse of `candidatePairRejectReason` — true when the pair must be skipped.
FUSE_PHYSICS_INLINE bool isRejectedCandidatePair(u32 bodyA, u32 bodyB, u32 bodyCount = 0u) {
    return candidatePairRejectReason(bodyA, bodyB, bodyCount) != CandidateRejectReason::None;

FUSE_PHYSICS_INLINE bool isRejectedCandidatePair(const CandidatePair& pair, u32 bodyCount = 0u) {
    return isRejectedCandidatePair(pair.bodyA, pair.bodyB, bodyCount);

/// Returns the first reject reason including AABB refine (sphere-expanded AABB overlap stub).
CandidateRejectReason candidatePairRejectReason(
    const CollisionShapeSoA& shapes);
/// Returns the first reject reason for a pair, or `None` when the pair may be emitted.
FUSE_PHYSICS_INLINE CandidatePairRejectReason candidatePairRejectReason(u32 bodyA, u32 bodyB, u32 bodyCount = 0u) {
        return CandidatePairRejectReason::SelfPair;
        return CandidatePairRejectReason::OutOfRangeBody;
    return CandidatePairRejectReason::None;

FUSE_PHYSICS_INLINE CandidatePairRejectReason candidatePairRejectReason(const CandidatePair& pair, u32 bodyCount = 0u) {

/// Clamp non-positive cell sizes to the broadphase stub default.

/// Clamp hash table size to at least one bucket (broadphase stub guard).
FUSE_PHYSICS_INLINE u32 clampTableSize(u32 tableSize) {
    return tableSize > 0u ? tableSize : 1u;
}

/// Clamp per-axis cell span budget (0 = unlimited stub).
FUSE_PHYSICS_INLINE u32 clampMaxCellSpanPerAxis(u32 maxSpanPerAxis) {
    return maxSpanPerAxis;
}

/// Sanitize broadphase params before occupancy iteration (B4.2 deepen pass).
FUSE_PHYSICS_INLINE SpatialHashParams sanitizeSpatialHashParams(SpatialHashParams params) {
    params.cellSize = clampCellSize(params.cellSize);
    params.tableSize = clampTableSize(params.tableSize);
    params.maxCellSpanPerAxis = clampMaxCellSpanPerAxis(params.maxCellSpanPerAxis);
    return params;
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

struct PairBufferSoA;

/// True when any axis has an inverted min/max span (empty occupancy iteration).
FUSE_PHYSICS_INLINE bool isEmptyCellRange(const CellRange3& range) {
FUSE_PHYSICS_INLINE s32 cellAxisSpan(s32 minCell, s32 maxCell) {
    return maxCell >= minCell ? (maxCell - minCell + 1) : 0;
}

FUSE_PHYSICS_INLINE bool cellRangeIsEmpty(const CellRange3& range) {
    return range.minCell.x > range.maxCell.x || range.minCell.y > range.maxCell.y ||
           range.minCell.z > range.maxCell.z;
}

FUSE_PHYSICS_INLINE bool isEmptyCellRange(const CellRange2& range) {
    return range.minCell.x > range.maxCell.x || range.minCell.y > range.maxCell.y;

/// Per-axis inclusive cell span for occupancy budgeting stubs.
FUSE_PHYSICS_INLINE ivec3 cellSpanPerAxis(const CellRange3& range) {
    if (isEmptyCellRange(range)) {
        return {};
    return {
        range.maxCell.x - range.minCell.x + 1,
        range.maxCell.y - range.minCell.y + 1,
        range.maxCell.z - range.minCell.z + 1,
    };

FUSE_PHYSICS_INLINE ivec2 cellSpanPerAxis(const CellRange2& range) {

/// Occupancy cell count stub for budgeting (0 when the range is empty).
FUSE_PHYSICS_INLINE u32 estimateCellOccupancyCount(const CellRange3& range) {
        return 0u;
    const ivec3 span = cellSpanPerAxis(range);
    return static_cast<u32>(span.x) * static_cast<u32>(span.y) * static_cast<u32>(span.z);

FUSE_PHYSICS_INLINE u32 estimateCellOccupancyCount(const CellRange2& range) {
    const ivec2 span = cellSpanPerAxis(range);
    return static_cast<u32>(span.x) * static_cast<u32>(span.y);

/// True when `maxCells == 0` (unlimited occupancy budget stub).
FUSE_PHYSICS_INLINE bool isUnboundedCellOccupancyBudget(u32 maxCells) {
    return maxCells == 0u;

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
    if (isUnboundedCellOccupancyBudget(maxCells)) {
        return false;
    return estimateCellOccupancyCount(range) > maxCells;

FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange2& range, u32 maxCells) {

/// Inverse of `exceedsCellOccupancyBudget` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool cellOccupancyWithinBudget(const CellRange3& range, u32 maxCells) {
    return !exceedsCellOccupancyBudget(range, maxCells);

FUSE_PHYSICS_INLINE bool cellOccupancyWithinBudget(const CellRange2& range, u32 maxCells) {

/// Remaining occupancy slots before `maxCells` is exceeded (unlimited when `maxCells == 0`).
FUSE_PHYSICS_INLINE u32 occupancyBudgetRemaining(const CellRange3& range, u32 maxCells) {
    if (isUnboundedCellOccupancyBudget(maxCells) || isEmptyCellRange(range)) {
        return maxCells;
    const u32 occupancy = estimateCellOccupancyCount(range);
    return occupancy >= maxCells ? 0u : maxCells - occupancy;

FUSE_PHYSICS_INLINE u32 occupancyBudgetRemaining(const CellRange2& range, u32 maxCells) {

/// Why cell occupancy iteration preflight rejected the range (B4.2 deepen follow-up).
enum class CellOccupancyRejectReason : u8 {
    None = 0,
    EmptyRange,
    ExceedsBudget,

/// Human-readable label for cell-occupancy reject reasons (logging / tests).
const char* cellOccupancyRejectReasonName(CellOccupancyRejectReason reason);

/// Diagnose why cell occupancy iteration would reject; vacuously succeeds on valid ranges.
FUSE_PHYSICS_INLINE CellOccupancyRejectReason cellOccupancyRejectReason(const CellRange3& range, u32 maxCells) {
        return CellOccupancyRejectReason::EmptyRange;
    if (exceedsCellOccupancyBudget(range, maxCells)) {
        return CellOccupancyRejectReason::ExceedsBudget;
    return CellOccupancyRejectReason::None;

FUSE_PHYSICS_INLINE CellOccupancyRejectReason cellOccupancyRejectReason(const CellRange2& range, u32 maxCells) {

/// Cell-capacity preflight for shape occupancy iteration (B4.2 deepen follow-up).
struct CellOccupancyPreflight {
    CellOccupancyRejectReason reason = CellOccupancyRejectReason::None;
    bool emptyRange = false;
    bool exceedsBudget = false;
    u32 occupancyCount = 0;

    bool canIterate() const { return reason == CellOccupancyRejectReason::None; }

FUSE_PHYSICS_INLINE CellOccupancyPreflight preflightCellOccupancy(const CellRange3& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.reason = cellOccupancyRejectReason(range, maxCells);
    preflight.emptyRange = preflight.reason == CellOccupancyRejectReason::EmptyRange;
    preflight.occupancyCount = estimateCellOccupancyCount(range);
    preflight.exceedsBudget = preflight.reason == CellOccupancyRejectReason::ExceedsBudget;
    return preflight;

FUSE_PHYSICS_INLINE CellOccupancyPreflight preflightCellOccupancy(const CellRange2& range, u32 maxCells) {

/// Non-mutating cell-occupancy skip predicate — inverse of `canIterate` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipCellOccupancyIteration(const CellRange3& range, u32 maxCells) {
    return !preflightCellOccupancy(range, maxCells).canIterate();

FUSE_PHYSICS_INLINE bool canSkipCellOccupancyIteration(const CellRange2& range, u32 maxCells) {

/// Non-mutating cell-occupancy predicate — mirrors `preflightCellOccupancy` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunCellOccupancyIteration(const CellRange3& range, u32 maxCells) {
    return preflightCellOccupancy(range, maxCells).canIterate();

FUSE_PHYSICS_INLINE bool shouldRunCellOccupancyIteration(const CellRange2& range, u32 maxCells) {

/// Early-out when cell-occupancy preflight would reject — same ordering as `canSkipCellOccupancyIteration` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool wouldSkipCellOccupancyIteration(
    const CellRange3& range,
    u32 maxCells,
    CellOccupancyRejectReason* reason = nullptr) {
    const CellOccupancyRejectReason reject = cellOccupancyRejectReason(range, maxCells);
    if (reason != nullptr) {
        *reason = reject;
    return reject != CellOccupancyRejectReason::None;

    const CellRange2& range,

/// Returns true when `cellOccupancyRejectReason` matches `expected` (B4.2 deepen follow-up pass).
FUSE_PHYSICS_INLINE bool cellOccupancyRejectsForReason(
    CellOccupancyRejectReason expected) {
    return cellOccupancyRejectReason(range, maxCells) == expected;


/// True when any axis span exceeds `maxSpanPerAxis` (0 = unlimited span budget).
FUSE_PHYSICS_INLINE bool exceedsCellSpanPerAxis(const CellRange3& range, u32 maxSpanPerAxis) {
    if (maxSpanPerAxis == 0u || isEmptyCellRange(range)) {
/// Clamp cell size to a positive value (broadphase stub guard).
FUSE_PHYSICS_INLINE f32 clampCellSize(f32 cellSize) {
    return cellSize > 0.f ? cellSize : 1.f;

/// Per-axis inclusive cell span for a 3D range.
FUSE_PHYSICS_INLINE ivec3 cellSpan3(const CellRange3& range) {
        range.maxCell.x - range.minCell.x,
        range.maxCell.y - range.minCell.y,
        range.maxCell.z - range.minCell.z,

/// Per-axis inclusive cell span for a 2D range.
FUSE_PHYSICS_INLINE ivec2 cellSpan2(const CellRange2& range) {

/// True when any axis span exceeds `maxSpanPerAxis` (0 = unlimited).
FUSE_PHYSICS_INLINE bool cellSpanExceedsClamp(const CellRange3& range, u32 maxSpanPerAxis) {
    if (maxSpanPerAxis == 0u) {
    const ivec3 span = cellSpan3(range);
    return span.x > static_cast<s32>(maxSpanPerAxis) || span.y > static_cast<s32>(maxSpanPerAxis) ||
           span.z > static_cast<s32>(maxSpanPerAxis);
}

FUSE_PHYSICS_INLINE bool exceedsCellSpanPerAxis(const CellRange2& range, u32 maxSpanPerAxis) {
    return span.x > static_cast<s32>(maxSpanPerAxis) || span.y > static_cast<s32>(maxSpanPerAxis);

/// Inverse of `exceedsCellSpanPerAxis` (B4.2 deepen follow-up pass).
FUSE_PHYSICS_INLINE bool cellSpanWithinPerAxisLimit(const CellRange3& range, u32 maxSpanPerAxis) {
    return !exceedsCellSpanPerAxis(range, maxSpanPerAxis);

FUSE_PHYSICS_INLINE bool cellSpanWithinPerAxisLimit(const CellRange2& range, u32 maxSpanPerAxis) {

/// Why per-axis cell-span clamp would modify the range (B4.2 deepen follow-up pass).
enum class CellSpanRejectReason : u8 {
    ExceedsSpan,

/// Human-readable label for cell-span reject reasons (logging / tests).
const char* cellSpanRejectReasonName(CellSpanRejectReason reason);

/// Diagnose why span clamp would apply; vacuously succeeds when span is within limit.
FUSE_PHYSICS_INLINE CellSpanRejectReason cellSpanRejectReason(const CellRange3& range, u32 maxSpanPerAxis) {
        return CellSpanRejectReason::EmptyRange;
    if (exceedsCellSpanPerAxis(range, maxSpanPerAxis)) {
        return CellSpanRejectReason::ExceedsSpan;
    return CellSpanRejectReason::None;

FUSE_PHYSICS_INLINE CellSpanRejectReason cellSpanRejectReason(const CellRange2& range, u32 maxSpanPerAxis) {

/// Returns true when `cellSpanRejectReason` matches `expected` (B4.2 deepen follow-up pass).
FUSE_PHYSICS_INLINE bool cellSpanRejectsForReason(
    u32 maxSpanPerAxis,
    CellSpanRejectReason expected) {
    return cellSpanRejectReason(range, maxSpanPerAxis) == expected;


/// Cell-span preflight for occupancy iteration clamp (B4.2 deepen follow-up pass).
struct CellSpanPreflight {
    CellSpanRejectReason reason = CellSpanRejectReason::None;
    bool exceedsSpan = false;
    ivec3 spanPerAxis{};

    bool withinLimit() const { return reason == CellSpanRejectReason::None; }

FUSE_PHYSICS_INLINE CellSpanPreflight preflightCellSpan(const CellRange3& range, u32 maxSpanPerAxis) {
    CellSpanPreflight preflight{};
    preflight.reason = cellSpanRejectReason(range, maxSpanPerAxis);
    preflight.emptyRange = preflight.reason == CellSpanRejectReason::EmptyRange;
    preflight.exceedsSpan = preflight.reason == CellSpanRejectReason::ExceedsSpan;
    preflight.spanPerAxis = cellSpanPerAxis(range);

FUSE_PHYSICS_INLINE CellSpanPreflight preflightCellSpan2D(const CellRange2& range, u32 maxSpanPerAxis) {
    preflight.spanPerAxis = {span.x, span.y, 1};

/// Non-mutating span-clamp skip predicate — true when clamp would be a no-op (B4.2 deepen follow-up pass).
FUSE_PHYSICS_INLINE bool canSkipCellSpanClamp(const CellRange3& range, u32 maxSpanPerAxis) {
        return true;
    return cellSpanRejectReason(range, maxSpanPerAxis) != CellSpanRejectReason::ExceedsSpan;

FUSE_PHYSICS_INLINE bool canSkipCellSpanClamp(const CellRange2& range, u32 maxSpanPerAxis) {

/// Non-mutating span-clamp predicate — true when per-axis span exceeds the budget (B4.2 deepen follow-up pass).
FUSE_PHYSICS_INLINE bool shouldRunCellSpanClamp(const CellRange3& range, u32 maxSpanPerAxis) {
    return cellSpanRejectReason(range, maxSpanPerAxis) == CellSpanRejectReason::ExceedsSpan;

FUSE_PHYSICS_INLINE bool shouldRunCellSpanClamp(const CellRange2& range, u32 maxSpanPerAxis) {

/// Early-out when cell-span clamp preflight would reject iteration — same ordering as `canSkipCellSpanClamp` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool wouldSkipCellSpanClamp(
    CellSpanRejectReason* reason = nullptr) {
    const CellSpanRejectReason reject = cellSpanRejectReason(range, maxSpanPerAxis);
    return reject == CellSpanRejectReason::ExceedsSpan;

/// True when a hash cell has fewer than two occupants (no pairs possible).
FUSE_PHYSICS_INLINE bool isEmptyCellBucket(usize occupantCount) {
    return occupantCount < 2u;

/// True when any axis span exceeds `maxSpanPerAxis` before clamp (0 = unlimited).
        return false;
    const ivec3 span = cellSpanPerAxis(range);

FUSE_PHYSICS_INLINE bool cellSpanExceedsClamp(const CellRange2& range, u32 maxSpanPerAxis) {
    const ivec2 span = cellSpanPerAxis(range);

/// Skip shape→cell insertion when range is empty or over occupancy budget (0 = unlimited).
FUSE_PHYSICS_INLINE bool canSkipShapeCellInsertion(const CellRange3& range, u32 maxOccupancy) {
    return isEmptyCellRange(range) || exceedsCellOccupancyBudget(range, maxOccupancy);

FUSE_PHYSICS_INLINE bool canSkipShapeCellInsertion(const CellRange2& range, u32 maxOccupancy) {

/// Const preflight for cell occupancy budgeting (B4.2 deepen pass).
struct CellCapacityPreflight {
    u32 occupancyCount = 0u;
    bool emptyRange = false;
    bool exceedsSpanClamp = false;
    bool exceedsOccupancyBudget = false;
    bool skipped = false;

    bool can_insert() const { return !skipped && !emptyRange && !exceedsOccupancyBudget; }
};

FUSE_PHYSICS_INLINE CellCapacityPreflight preflight_cell_capacity(
    u32 maxOccupancy = 0u) {
    CellCapacityPreflight preflight{};
    if (isEmptyCellRange(range)) {
        preflight.emptyRange = true;
        preflight.skipped = true;
    preflight.exceedsSpanClamp = cellSpanExceedsClamp(range, maxSpanPerAxis);
    preflight.exceedsOccupancyBudget = exceedsCellOccupancyBudget(range, maxOccupancy);

/// Why cell occupancy iteration preflight rejected the range (B4.2 deepen follow-up).
enum class CellOccupancyRejectReason : u8 {
    None = 0,
    EmptyRange,
    ExceedsBudget,

/// Human-readable label for cell-occupancy reject reasons (logging / tests).
const char* cellOccupancyRejectReasonName(CellOccupancyRejectReason reason);

/// Diagnose why cell occupancy iteration would reject; vacuously succeeds on valid ranges.
FUSE_PHYSICS_INLINE CellOccupancyRejectReason cellOccupancyRejectReason(const CellRange3& range, u32 maxCells) {
        return CellOccupancyRejectReason::EmptyRange;
    if (exceedsCellOccupancyBudget(range, maxCells)) {
        return CellOccupancyRejectReason::ExceedsBudget;
    return CellOccupancyRejectReason::None;

FUSE_PHYSICS_INLINE CellOccupancyRejectReason cellOccupancyRejectReason(const CellRange2& range, u32 maxCells) {

/// Cell-capacity preflight for shape occupancy iteration (B4.2 deepen follow-up).
struct CellOccupancyPreflight {
    bool exceedsBudget = false;
    u32 occupancyCount = 0;

    bool canIterate() const { return !emptyRange && !exceedsBudget; }

    preflight.emptyRange = isEmptyCellRange(range);
    preflight.exceedsBudget = cellOccupancyRejectReason(range, maxCells) ==
                              CellOccupancyRejectReason::ExceedsBudget;



/// Non-mutating cell-occupancy skip predicate — inverse of `preflightCellOccupancy` (B4.2 deepen pass).


CellOccupancyPreflight preflightCellOccupancy(const CellRange3& range, u32 maxCells);

CellOccupancyPreflight preflightCellOccupancy(const CellRange2& range, u32 maxCells);


}

/// True when cell occupancy iteration would early-out (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipCellOccupancyIteration(const CellRange3& range, u32 maxCells) {
    return cellOccupancyRejectReason(range, maxCells) != CellOccupancyRejectReason::None;
}

FUSE_PHYSICS_INLINE bool canSkipCellOccupancyIteration(const CellRange2& range, u32 maxCells) {
    return cellOccupancyRejectReason(range, maxCells) != CellOccupancyRejectReason::None;
}

/// Non-mutating cell-occupancy predicate — inverse of `canSkipCellOccupancyIteration` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool shouldIterateCellOccupancy(const CellRange3& range, u32 maxCells) {
    return !canSkipCellOccupancyIteration(range, maxCells);
}

FUSE_PHYSICS_INLINE bool shouldIterateCellOccupancy(const CellRange2& range, u32 maxCells) {
    return !canSkipCellOccupancyIteration(range, maxCells);
}

/// Returns true when `cellOccupancyRejectReason` matches `expected` (B4.2 deepen follow-up pass).
FUSE_PHYSICS_INLINE bool cellOccupancyRejectsForReason(
    const CellRange3& range,
    u32 maxCells,
    CellOccupancyRejectReason expected) {
    return cellOccupancyRejectReason(range, maxCells) == expected;

    const CellRange2& range,

/// Pair-list sizing stub: unique-body pair count n*(n-1)/2 (0 when n < 2).
FUSE_PHYSICS_INLINE u32 estimatePairCountForUniqueBodies(u32 uniqueBodyCount) {
    return uniqueBodyCount > 1u ? uniqueBodyCount * (uniqueBodyCount - 1u) / 2u : 0u;

/// Per-cell pair-count stub from unique occupant count (alias for cell-pair gen budgeting).
FUSE_PHYSICS_INLINE u32 estimateCellPairCount(u32 uniqueOccupantCount) {
    return estimatePairCountForUniqueBodies(uniqueOccupantCount);

/// Why per-cell pair generation would early-out (B4.2 deepen pass).
enum class CellPairGenRejectReason : u8 {
    EmptyCell,
    SingletonOccupant,

/// Human-readable label for cell-pair generation reject reasons (logging / tests).
const char* cellPairGenRejectReasonName(CellPairGenRejectReason reason);

/// Diagnose why cell pair generation would skip; vacuously succeeds when pairs may be emitted.
FUSE_PHYSICS_INLINE CellPairGenRejectReason cellPairGenRejectReason(u32 uniqueOccupantCount) {
    if (uniqueOccupantCount == 0u) {
        return CellPairGenRejectReason::EmptyCell;
    if (uniqueOccupantCount < 2u) {
        return CellPairGenRejectReason::SingletonOccupant;
    return CellPairGenRejectReason::None;

/// Returns true when `cellPairGenRejectReason` matches `expected` (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool cellPairGenRejectsForReason(u32 uniqueOccupantCount, CellPairGenRejectReason expected) {
    return cellPairGenRejectReason(uniqueOccupantCount) == expected;

/// Read-only per-cell pair-generation diagnostics — no mutation (B4.2 deepen pass).
struct CellPairGenPreflight {
    CellPairGenRejectReason reason = CellPairGenRejectReason::None;
    bool emptyCell = false;
    bool singletonOccupant = false;
    u32 uniqueOccupantCount = 0;
    u32 pairCount = 0;

    bool canGenerate() const { return reason == CellPairGenRejectReason::None; }

/// Count unique body indices in a hash-cell occupant list (cell-pair gen budgeting stub).
u32 countUniqueCellOccupants(const std::vector<u32>& occupants);

CellPairGenPreflight preflightCellPairGeneration(const std::vector<u32>& occupants);

/// Non-mutating cell-pair generation skip predicate — inverse of `canGenerate` (B4.2 deepen pass).
bool canSkipCellPairGeneration(const std::vector<u32>& occupants);

/// Non-mutating cell-pair generation predicate — mirrors `preflightCellPairGeneration` (B4.2 deepen pass).
bool shouldRunCellPairGeneration(const std::vector<u32>& occupants);

/// Why shape→cell insertion would early-out (B4.2 deepen pass).
enum class ShapeCellInsertRejectReason : u8 {
    OutOfRangeBody,
    OccupancySkipped,

/// Human-readable label for shape cell-insert reject reasons (logging / tests).
const char* shapeCellInsertRejectReasonName(ShapeCellInsertRejectReason reason);

/// Diagnose why shape→cell insertion would skip; vacuously succeeds when insertion may proceed.
ShapeCellInsertRejectReason shapeCellInsertRejectReason(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D);

/// Returns true when `shapeCellInsertRejectReason` matches `expected` (B4.2 deepen pass).
bool shapeCellInsertRejectsForReason(
    bool use2D,
    ShapeCellInsertRejectReason expected);

/// Read-only shape→cell insert diagnostics — no mutation (B4.2 deepen pass).
struct ShapeCellInsertPreflight {
    ShapeCellInsertRejectReason reason = ShapeCellInsertRejectReason::None;
    bool outOfRangeBody = false;
    bool occupancySkipped = false;

    bool canInsert() const { return reason == ShapeCellInsertRejectReason::None; }

ShapeCellInsertPreflight preflightShapeCellInsert(

/// Non-mutating shape→cell insert skip predicate — inverse of `canInsert` (B4.2 deepen pass).
bool canSkipShapeCellInsert(

/// Non-mutating shape→cell insert predicate — mirrors `preflightShapeCellInsert` (B4.2 deepen pass).
bool shouldRunShapeCellInsert(
/// Empty broadphase input guard: no bodies or shapes to hash.
FUSE_PHYSICS_INLINE bool canSkipBroadphase(u32 bodyCount, u32 shapeCount) {
    return bodyCount == 0u || shapeCount == 0u;
}

/// Empty pair-list guard for vector broadphase output.
FUSE_PHYSICS_INLINE bool isEmptyCandidatePairList(const std::vector<CandidatePair>& pairs) {
    return pairs.empty();

/// Cell with fewer than two occupants cannot emit candidate pairs.
FUSE_PHYSICS_INLINE bool canSkipCellPairGeneration(u32 occupantCount) {
    return occupantCount < 2u;

/// Pair-list dedupe is a no-op for zero or one pairs.
FUSE_PHYSICS_INLINE bool canSkipPairListDedupe(u32 pairCount) {
    return pairCount <= 1u;

/// True when any axis span exceeds the per-axis budget (0 = never exceeds).
FUSE_PHYSICS_INLINE bool exceedsMaxCellSpanPerAxis(const CellRange3& range, u32 maxSpanPerAxis) {
    if (maxSpanPerAxis == 0u || isEmptyCellRange(range)) {
        return false;
    const ivec3 span = cellSpanPerAxis(range);
    return span.x > static_cast<s32>(maxSpanPerAxis) || span.y > static_cast<s32>(maxSpanPerAxis) ||
           span.z > static_cast<s32>(maxSpanPerAxis);

FUSE_PHYSICS_INLINE bool exceedsMaxCellSpanPerAxis(const CellRange2& range, u32 maxSpanPerAxis) {
    const ivec2 span = cellSpanPerAxis(range);

/// True when estimated cell count exceeds budget (0 = unlimited).
FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange3& range, u32 maxCells) {
    if (maxCells == 0u) {
    return estimateCellOccupancyCount(range) > maxCells;

FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange2& range, u32 maxCells) {

/// Shrink range from the largest axis until occupancy fits budget (no-op when budget is 0).
FUSE_PHYSICS_INLINE CellRange3 shrinkCellRangeToOccupancyBudget(CellRange3 range, u32 maxCells) {
    if (maxCells == 0u || isEmptyCellRange(range)) {
        return range;

    while (estimateCellOccupancyCount(range) > maxCells) {
        if (span.x >= span.y && span.x >= span.z && span.x > 1) {
            if (range.maxCell.x > range.minCell.x) {
                --range.maxCell.x;
            } else {
                ++range.minCell.x;
        } else if (span.y >= span.z && span.y > 1) {
            if (range.maxCell.y > range.minCell.y) {
                --range.maxCell.y;
                ++range.minCell.y;
        } else if (span.z > 1) {
            if (range.maxCell.z > range.minCell.z) {
                --range.maxCell.z;
                ++range.minCell.z;
            break;
        if (isEmptyCellRange(range)) {

FUSE_PHYSICS_INLINE CellRange2 shrinkCellRangeToOccupancyBudget(CellRange2 range, u32 maxCells) {

        if (span.x >= span.y && span.x > 1) {
        } else if (span.y > 1) {

/// Count pairs passing the validity guard.
FUSE_PHYSICS_INLINE u32 countValidCandidatePairs(
    const std::vector<CandidatePair>& pairs,
    u32 bodyCount = 0u) {
    u32 count = 0u;
    for (const CandidatePair& pair : pairs) {
        if (isValidCandidatePair(pair, bodyCount)) {
            ++count;
    return count;

/// Clamp per-axis cell span budget (0 = unlimited stub).
FUSE_PHYSICS_INLINE u32 clampMaxCellSpanPerAxis(u32 maxSpanPerAxis) {
    return maxSpanPerAxis;
/// True when any per-axis span exceeds `maxSpanPerAxis` before clamping (0 = unlimited).
FUSE_PHYSICS_INLINE bool cellSpanExceedsClamp(const CellRange3& range, u32 maxSpanPerAxis) {

FUSE_PHYSICS_INLINE bool cellSpanExceedsClamp(const CellRange2& range, u32 maxSpanPerAxis) {

/// Cell-capacity guard: true when occupancy exceeds `maxCells` (0 = unlimited).

/// Max 3D cell occupancy implied by a per-axis span clamp (0 = unlimited budget).
FUSE_PHYSICS_INLINE u32 maxCellOccupancyBudget3D(u32 maxSpanPerAxis) {
    return maxSpanPerAxis > 0u ? maxSpanPerAxis * maxSpanPerAxis * maxSpanPerAxis : 0u;
}

/// Max 2D cell occupancy implied by a per-axis span clamp (0 = unlimited budget).
FUSE_PHYSICS_INLINE u32 maxCellOccupancyBudget2D(u32 maxSpanPerAxis) {
    return maxSpanPerAxis > 0u ? maxSpanPerAxis * maxSpanPerAxis : 0u;

/// Const preflight for shape→cell insertion occupancy (B4.2 deepen pass).
struct ShapeCellOccupancyPreflight {
    u32 estimatedCells = 0;
    u32 maxCells = 0;
    bool exceedsBudget = false;
    bool skipped = false;

    bool can_insert() const { return !skipped && !exceedsBudget; }
};

/// Populate shape cell occupancy preflight without mutating hash buckets (B4.2 deepen pass).
FUSE_PHYSICS_INLINE ShapeCellOccupancyPreflight preflight_shape_cell_occupancy(
    const CellRange3& range,
    u32 maxSpanPerAxis) {
    ShapeCellOccupancyPreflight preflight{};
        preflight.skipped = true;
        return preflight;

    preflight.estimatedCells = estimateCellOccupancyCount(range);
    preflight.maxCells = maxCellOccupancyBudget3D(maxSpanPerAxis);
    preflight.exceedsBudget =
        preflight.maxCells > 0u && exceedsCellOccupancyBudget(range, preflight.maxCells);

    const CellRange2& range,

    preflight.maxCells = maxCellOccupancyBudget2D(maxSpanPerAxis);

/// Max occupancy budget from per-axis span clamp (0 = unlimited).
FUSE_PHYSICS_INLINE u32 estimateMaxCellOccupancyBudget(u32 maxSpanPerAxis, bool use3D) {
    if (maxSpanPerAxis == 0u) {
        return 0u;
    const u32 span = maxSpanPerAxis;
    return use3D ? span * span * span : span * span;

/// Const preflight for broadphase dispatch (B4.2 deepen pass).
struct BroadphasePreflight {
    u32 bodyCount = 0;
    u32 shapeCount = 0;
    bool emptyInput = false;

    bool can_run() const { return !skipped && !emptyInput; }


    bool can_dispatch() const { return !skipped; }

/// Populate broadphase preflight without running hash build (B4.2 deepen pass).
BroadphasePreflight preflight_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Const preflight for parallel pair refine dispatch (B4.2 deepen pass).
struct BroadphaseRefinePreflight {
    u32 pairCount = 0;
    bool emptyBuffer = false;

    bool can_refine() const { return !skipped && !emptyInput && !emptyBuffer; }

BroadphaseRefinePreflight preflight_broadphase_refine(
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer);

/// True when refine dispatch may early-out before AABB overlap tests (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipBroadphaseRefine(
    const PairBufferSoA& buffer) {
    return !preflight_broadphase_refine(bodies, shapes, buffer).can_refine();
/// Cell-capacity preflight for shape occupancy iteration (B4.2 deepen pass).
struct CellOccupancyPreflight {
    u32 cellCount = 0u;

/// Populate cell-occupancy preflight without mutating the range (B4.2 deepen pass).
FUSE_PHYSICS_INLINE CellOccupancyPreflight preflightCellOccupancy(const CellRange3& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.cellCount = estimateCellOccupancyCount(range);
    if (maxCells > 0u) {
        preflight.exceedsBudget = preflight.cellCount > maxCells;

FUSE_PHYSICS_INLINE CellOccupancyPreflight preflightCellOccupancy(const CellRange2& range, u32 maxCells) {

/// Per-shape cell budget derived from `maxCellSpanPerAxis` (0 = unlimited).
FUSE_PHYSICS_INLINE u32 perShapeCellBudget(u32 maxCellSpanPerAxis, bool use2D) {
    if (maxCellSpanPerAxis == 0u) {
    const u32 span = maxCellSpanPerAxis;
    return use2D ? span * span : span * span * span;

/// Refine-pass preflight for parallel pair invalidation (B4.2 deepen pass).
    bool emptyBroadphaseInput = false;

BroadphaseRefinePreflight preflightRefineBroadphasePairs(
    const PairBufferSoA& buffer,

/// Early-out guard combining refine preflight checks (B4.2 deepen pass).
bool canSkipRefineBroadphasePairs(

/// Dedupe-pass preflight: true when sort+unique would be a no-op (B4.2 deepen pass).
bool canSkipDedupeBuffer(const PairBufferSoA& buffer);
/// Const preflight for cell occupancy budgeting (B4.2 deepen pass).
    u32 estimatedCells = 0u;
    u32 maxCells = 0u;
    bool emptyRange = false;

    bool can_insert_cells() const { return !skipped && !emptyRange && !exceedsBudget; }

/// Populate cell occupancy preflight without mutating ranges (B4.2 deepen pass).
FUSE_PHYSICS_INLINE CellOccupancyPreflight preflight_cell_occupancy(const CellRange3& range, u32 maxCells = 0u) {
    preflight.maxCells = maxCells;
    preflight.emptyRange = isEmptyCellRange(range);
    if (preflight.emptyRange) {

    preflight.exceedsBudget = exceedsCellOccupancyBudget(range, maxCells);
    preflight.skipped = preflight.exceedsBudget;

FUSE_PHYSICS_INLINE CellOccupancyPreflight preflight_cell_occupancy(const CellRange2& range, u32 maxCells = 0u) {


struct BroadphaseDispatchPreflight {

    bool can_dispatch() const { return !skipped && !emptyInput; }

/// Populate broadphase dispatch preflight without building hash tables (B4.2 deepen pass).
FUSE_PHYSICS_INLINE BroadphaseDispatchPreflight preflight_broadphase_dispatch(
    const CollisionShapeSoA& shapes) {
    BroadphaseDispatchPreflight preflight{};
    preflight.emptyInput = isEmptyBroadphaseInput(bodies, shapes);
    preflight.skipped = preflight.emptyInput;
/// Early-out guard for broadphase dispatch (B4.2 deepen pass).
bool should_skip_broadphase(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes);

/// Const preflight for shape cell-occupancy iteration (B4.2 deepen pass).

    bool can_iterate() const { return !skipped && !emptyRange && !exceedsBudget; }

/// Populate cell-occupancy preflight without iterating cells (B4.2 deepen pass).
CellOccupancyPreflight preflight_cell_occupancy(const CellRange3& range, u32 maxCells);
CellOccupancyPreflight preflight_cell_occupancy(const CellRange2& range, u32 maxCells);

/// Const preflight for parallel pair refine (B4.2 deepen pass).
struct RefineBroadphasePreflight {

    bool can_refine() const { return !skipped && pairCount > 0u; }

/// Populate refine preflight without invalidating pair slots (B4.2 deepen pass).
RefineBroadphasePreflight preflight_refine_broadphase_pairs(

/// Early-out guard for parallel pair refine (B4.2 deepen pass).
bool should_skip_refine_broadphase_pairs(
/// Const preflight for shape cell occupancy budgeting (B4.2 deepen pass).
    bool isEmptyRange = false;

    bool can_populate_cells() const { return !isEmptyRange && !exceedsBudget; }

FUSE_PHYSICS_INLINE CellOccupancyPreflight preflight_cell_occupancy(const CellRange3& range, u32 maxCells) {
    preflight.isEmptyRange = isEmptyCellRange(range);
    if (preflight.isEmptyRange) {

FUSE_PHYSICS_INLINE CellOccupancyPreflight preflight_cell_occupancy(const CellRange2& range, u32 maxCells) {

/// Returns true when shape cell insertion may be skipped before hash build (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool should_skip_shape_cell_population(const CellRange3& range, u32 maxCells) {
    const CellOccupancyPreflight preflight = preflight_cell_occupancy(range, maxCells);
    return preflight.isEmptyRange;

FUSE_PHYSICS_INLINE bool should_skip_shape_cell_population(const CellRange2& range, u32 maxCells) {

struct PairBufferSoA;

/// Const preflight for broadphase pair-buffer dedupe (B4.2 deepen pass).
struct BroadphaseDedupePreflight {
    u32 validPairCount = 0u;

    bool can_dedupe() const { return !skipped; }

/// Populate dedupe preflight without mutating pair storage (B4.2 deepen pass).
BroadphaseDedupePreflight preflight_dedupe_pairs(const PairBufferSoA& buffer);

/// Const preflight for parallel broadphase pair refine (B4.2 deepen pass).

    bool can_refine() const { return !skipped; }

BroadphaseRefinePreflight preflight_refine_broadphase_pairs(

/// Returns true when refine may early-out before parallel invalidation (B4.2 deepen pass).

    bool can_insert() const { return !emptyRange && !exceedsBudget; }

        preflight.emptyRange = true;


/// Derive a per-shape cell budget from `maxSpanPerAxis` (0 = unlimited).
FUSE_PHYSICS_INLINE u32 maxCellBudgetFromSpanPerAxis(u32 maxSpanPerAxis, bool use2D) {
    return use2D ? maxSpanPerAxis * maxSpanPerAxis : maxSpanPerAxis * maxSpanPerAxis * maxSpanPerAxis;

/// True when shape cell insertion should be skipped before hash build (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool should_skip_shape_cell_insertion(const CellRange3& range, u32 maxSpanPerAxis) {
    const CellOccupancyPreflight preflight =
        preflight_cell_occupancy(range, maxCellBudgetFromSpanPerAxis(maxSpanPerAxis, false));
    return !preflight.can_insert();

FUSE_PHYSICS_INLINE bool should_skip_shape_cell_insertion(const CellRange2& range, u32 maxSpanPerAxis) {
        preflight_cell_occupancy(range, maxCellBudgetFromSpanPerAxis(maxSpanPerAxis, true));


/// Broadphase refine preflight for parallel AABB refine dispatch (B4.2 deepen pass).
    bool emptyScene = false;


/// Populate refine preflight without mutating the pair buffer (B4.2 deepen pass).

/// True when parallel refine should early-out before AABB overlap tests (B4.2 deepen pass).
bool should_skip_broadphase_refine(
    u32 occupancyCount = 0;

    bool within_budget() const { return !exceedsBudget; }
    bool can_insert() const { return !skipped && within_budget(); }


/// True when shape cell iteration should skip due to an empty range (B4.2 deepen pass).
bool should_skip_shape_cell_insert(const CellRange3& range, u32 maxCells = 0u);
bool should_skip_shape_cell_insert(const CellRange2& range, u32 maxCells = 0u);

/// Clamp broadphase params to safe stub defaults (positive cell size, at least one bucket).
FUSE_PHYSICS_INLINE SpatialHashParams normalizeSpatialHashParams(SpatialHashParams params) {
    params.cellSize = clampCellSize(params.cellSize);
    params.tableSize = clampTableSize(params.tableSize);
    return params;

FUSE_PHYSICS_INLINE bool cellSpanExceedsClamp(const CellRange2& range, u32 maxSpanPerAxis) {
    const ivec2 span = cellSpan2(range);
FUSE_PHYSICS_INLINE bool cellRangeIsEmpty(const CellRange2& range) {

FUSE_PHYSICS_INLINE u32 cellRangeVolume3(const CellRange3& range) {
    if (cellRangeIsEmpty(range)) {
    return static_cast<u32>(cellAxisSpan(range.minCell.x, range.maxCell.x)) *
           static_cast<u32>(cellAxisSpan(range.minCell.y, range.maxCell.y)) *
           static_cast<u32>(cellAxisSpan(range.minCell.z, range.maxCell.z));

FUSE_PHYSICS_INLINE u32 cellRangeVolume2(const CellRange2& range) {
           static_cast<u32>(cellAxisSpan(range.minCell.y, range.maxCell.y));

/// True when a hash cell cannot emit candidate pairs (single occupant or empty).
FUSE_PHYSICS_INLINE bool shouldSkipCellPairGeneration(u32 occupantCount) {
    return occupantCount < 2u;
/// Total cell slots covered by a range (0 when empty).
FUSE_PHYSICS_INLINE u32 cellOccupancyCount(const CellRange3& range) {
    if (isEmptyCellRange(range)) {
        return 0u;
    }
    const ivec3 span = cellSpanPerAxis(range);
    return static_cast<u32>(span.x) * static_cast<u32>(span.y) * static_cast<u32>(span.z);

FUSE_PHYSICS_INLINE u32 cellOccupancyCount(const CellRange2& range) {
    const ivec2 span = cellSpanPerAxis(range);
    return static_cast<u32>(span.x) * static_cast<u32>(span.y);

/// True when occupancy exceeds a stub budget (0 = unlimited).
FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange3& range, u32 maxCells) {
    return maxCells > 0u && cellOccupancyCount(range) > maxCells;

FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange2& range, u32 maxCells) {

/// Derive a per-shape occupancy budget from span clamp (0 = unlimited).
FUSE_PHYSICS_INLINE u32 cellOccupancyBudgetFromSpan(u32 maxSpanPerAxis) {
    if (maxSpanPerAxis == 0u) {
    const u32 halfSpan = maxSpanPerAxis / 2u;
    const u32 effectiveSpan = halfSpan * 2u + 1u;
    return effectiveSpan * effectiveSpan * effectiveSpan;

FUSE_PHYSICS_INLINE u32 cellOccupancyBudgetFromSpan2D(u32 maxSpanPerAxis) {
    return effectiveSpan * effectiveSpan;

/// True when both body and shape SoA inputs are empty (broadphase early-out guard).
FUSE_PHYSICS_INLINE bool isEmptyBroadphaseInput(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return bodies.count() == 0u || shapes.count() == 0u;
}

/// True when a hash cell has fewer than two occupants (no pairs possible).
FUSE_PHYSICS_INLINE bool isEmptyCellBucket(usize occupantCount) {
    return occupantCount < 2u;
}

/// True when a candidate pair vector has no entries.
FUSE_PHYSICS_INLINE bool isEmptyPairList(const std::vector<CandidatePair>& pairs) {
    return pairs.empty();

/// True when occupancy count exceeds the per-shape budget (0 budget = never exceeds).
FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(u32 occupancyCount, u32 maxOccupancy) {
    return maxOccupancy > 0u && occupancyCount > maxOccupancy;

FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange3& range, u32 maxOccupancy) {
    return exceedsCellOccupancyBudget(estimateCellOccupancyCount(range), maxOccupancy);

FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange2& range, u32 maxOccupancy) {

/// Skip shape→cell insertion when range is empty or over occupancy budget.
FUSE_PHYSICS_INLINE bool canSkipShapeCellInsertion(const CellRange3& range, u32 maxOccupancy) {
    return isEmptyCellRange(range) || exceedsCellOccupancyBudget(range, maxOccupancy);

FUSE_PHYSICS_INLINE bool canSkipShapeCellInsertion(const CellRange2& range, u32 maxOccupancy) {

/// Count pairs in a list that pass the validity guard.
FUSE_PHYSICS_INLINE u32 countValidCandidatePairs(
    const std::vector<CandidatePair>& pairs,
    u32 bodyCount = 0u) {
    u32 validCount = 0u;
    for (const CandidatePair& pair : pairs) {
        if (isValidCandidatePair(pair, bodyCount)) {
            ++validCount;
    return validCount;

/// True when every pair in the list is valid and canonical (bodyA <= bodyB).
FUSE_PHYSICS_INLINE bool pairListIsCanonical(
        if (!isValidCandidatePair(pair, bodyCount) || pair.bodyA > pair.bodyB) {
            return false;
    return true;

/// Remove invalid/self pairs from a candidate list in place; returns remaining count.
u32 pruneInvalidCandidatePairs(std::vector<CandidatePair>& pairs, u32 bodyCount = 0u);
/// Sanitize broadphase params before occupancy iteration (extends normalize with span budget).
FUSE_PHYSICS_INLINE SpatialHashParams sanitizeSpatialHashParams(SpatialHashParams params) {
    params = normalizeSpatialHashParams(params);
    params.maxCellSpanPerAxis = clampMaxCellSpanPerAxis(params.maxCellSpanPerAxis);
    return params;

/// True when any axis coordinate span exceeds `maxSpanPerAxis` before clamp (0 = unlimited).
FUSE_PHYSICS_INLINE bool cellSpanExceedsClamp(const CellRange3& range, u32 maxSpanPerAxis) {
    if (maxSpanPerAxis == 0u || isEmptyCellRange(range)) {
    const s32 spanX = range.maxCell.x - range.minCell.x;
    const s32 spanY = range.maxCell.y - range.minCell.y;
    const s32 spanZ = range.maxCell.z - range.minCell.z;
    return spanX > static_cast<s32>(maxSpanPerAxis) || spanY > static_cast<s32>(maxSpanPerAxis) ||
           spanZ > static_cast<s32>(maxSpanPerAxis);

FUSE_PHYSICS_INLINE bool cellSpanExceedsClamp(const CellRange2& range, u32 maxSpanPerAxis) {
    return spanX > static_cast<s32>(maxSpanPerAxis) || spanY > static_cast<s32>(maxSpanPerAxis);

/// True when occupancy exceeds a stub budget (0 = unlimited).
FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange3& range, u32 maxCells) {
    return maxCells > 0u && estimateCellOccupancyCount(range) > maxCells;

FUSE_PHYSICS_INLINE bool exceedsCellOccupancyBudget(const CellRange2& range, u32 maxCells) {

/// Derive a per-shape occupancy budget from span clamp (0 = unlimited).
FUSE_PHYSICS_INLINE u32 cellOccupancyBudgetFromSpan(u32 maxSpanPerAxis) {
    if (maxSpanPerAxis == 0u) {
        return 0u;
    const u32 halfSpan = maxSpanPerAxis / 2u;
    const u32 effectiveSpan = halfSpan * 2u + 1u;
    return effectiveSpan * effectiveSpan * effectiveSpan;

FUSE_PHYSICS_INLINE u32 cellOccupancyBudgetFromSpan2D(u32 maxSpanPerAxis) {
    return effectiveSpan * effectiveSpan;

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

/// Occupancy count after per-axis span clamp (budgeting stub).
FUSE_PHYSICS_INLINE u32 estimateCellOccupancyCountAfterClamp(const CellRange3& range, u32 maxSpanPerAxis) {
    return estimateCellOccupancyCount(clampCellRange3(range, maxSpanPerAxis));
}

FUSE_PHYSICS_INLINE u32 estimateCellOccupancyCountAfterClamp(const CellRange2& range, u32 maxSpanPerAxis) {
    return estimateCellOccupancyCount(clampCellRange2(range, maxSpanPerAxis));
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

FUSE_PHYSICS_INLINE CellRange3 cellRangeFromSphere(vec3 center, f32 radius, f32 cellSize, u32 maxSpanPerAxis = 64u) {
    const f32 cell = clampCellSize(cellSize);
    CellRange3 range = {
        worldToCell({center.x - radius, center.y - radius, center.z - radius}, cell),
        worldToCell({center.x + radius, center.y + radius, center.z + radius}, cell),
    };
    return clampCellRange3(range, maxSpanPerAxis);
}

FUSE_PHYSICS_INLINE CellRange2 cellRangeFromSphere2D(vec2 center, f32 radius, f32 cellSize, u32 maxSpanPerAxis = 64u) {
    CellRange2 range = {
        worldToCell2D({center.x - radius, center.y - radius}, cell),
        worldToCell2D({center.x + radius, center.y + radius}, cell),
    return clampCellRange2(range, maxSpanPerAxis);

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

/// True when broadphase input has no bodies or shapes to process.
FUSE_PHYSICS_INLINE bool shouldSkipBroadphaseInput(u32 bodyCount, u32 shapeCount) {
    return bodyCount == 0u || shapeCount == 0u;
}

/// True when a hash cell cannot emit candidate pairs (single occupant or empty).
FUSE_PHYSICS_INLINE bool shouldSkipCellPairGeneration(u32 occupantCount) {
    return occupantCount < 2u;

/// True when refine can skip scanning pair slots (empty buffer or missing scene data).
FUSE_PHYSICS_INLINE bool shouldSkipBroadphaseRefine(
    u32 activeCount,
    u32 pairSlotCount,
    u32 bodyCount,
    u32 shapeCount) {
    return (activeCount == 0u && pairSlotCount == 0u) || shouldSkipBroadphaseInput(bodyCount, shapeCount);
/// Const preflight for broadphase input dispatch (B4.2 deepen pass).
/// Const preflight for broadphase input (B4.2 deepen pass).
struct BroadphaseInputPreflight {
    bool emptyBodies = false;
    bool emptyShapes = false;
    bool skipped = false;

    bool can_run() const { return !skipped; }
};

/// Populate input preflight without running hash build (B4.2 deepen pass).
BroadphaseInputPreflight preflight_broadphase_input(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when broadphase should skip before hash build (B4.2 deepen pass).
bool should_skip_broadphase(

/// Const preflight for cell occupancy before hash insert (B4.2 deepen pass).
struct CellOccupancyPreflight {
    u32 estimatedCells = 0;
    u32 maxCells = 0;
    bool exceedsBudget = false;
    bool emptyRange = false;

    bool can_insert() const { return !skipped && !emptyRange && !exceedsBudget; }


/// Populate cell occupancy preflight without mutating hash buckets (B4.2 deepen pass).
CellOccupancyPreflight preflight_cell_occupancy(const CellRange3& range, u32 maxCells);
CellOccupancyPreflight preflight_cell_occupancy(const CellRange2& range, u32 maxCells);

/// True when shape cell insert may be skipped (empty range or occupancy budget exceeded).
FUSE_PHYSICS_INLINE bool canSkipCellOccupancyInsert(const CellRange3& range, u32 maxCells) {
    if (isEmptyCellRange(range)) {
        return true;
    return maxCells > 0u && exceedsCellOccupancyBudget(range, maxCells);

FUSE_PHYSICS_INLINE bool canSkipCellOccupancyInsert(const CellRange2& range, u32 maxCells) {


/// Const preflight for broadphase pair refine dispatch (B4.2 deepen pass).
struct RefineBroadphasePreflight {
    bool emptyBuffer = false;
    bool emptyInput = false;

    bool can_refine() const { return !skipped; }

/// Populate refine preflight without invalidating pair slots (B4.2 deepen pass).
RefineBroadphasePreflight preflight_refine_broadphase(


    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer);

/// Returns true when refine should skip before AABB overlap pass (B4.2 deepen pass).
bool should_skip_refine_broadphase(
/// Const preflight for pair-buffer capacity (B4.2 deepen pass).
struct PairBufferPreflight {
    u32 activeCount = 0u;
    u32 remaining = UINT32_MAX;
    bool empty = false;
    bool full = false;
    bool hasDropped = false;

    bool can_push(u32 additionalCount = 1u) const;

PairBufferPreflight preflight_pair_buffer(const PairBufferSoA& buffer);

/// Const preflight for parallel AABB refine (B4.2 deepen pass).
struct BroadphaseRefinePreflight {
    u32 pairCount = 0u;


BroadphaseRefinePreflight preflight_broadphase_refine(
    const PairBufferSoA& buffer,

/// Const preflight for canonical pair dedupe (B4.2 deepen pass).
struct BroadphaseDedupePreflight {
    bool noOp = false;

    bool needs_dedupe() const { return !skipped && !noOp; }

BroadphaseDedupePreflight preflight_broadphase_dedupe(const PairBufferSoA& buffer);
/// Const preflight for broadphase pair refine (B4.2 deepen pass).
    u32 validPairCount = 0u;

    bool can_refine() const {
        return !skipped && !emptyInput && !emptyBuffer && validPairCount > 0u;


/// Early-out guard combining refine preflight checks (B4.2 deepen pass).
bool canSkipRefineBroadphase(
/// Const preflight for broadphase dispatch (B4.2 deepen follow-up).
struct BroadphasePreflight {
    bool singletonInput = false;

    bool can_dispatch() const { return !skipped; }

/// Populate broadphase preflight without running hash build (B4.2 deepen follow-up).
BroadphasePreflight preflight_broadphase(

/// Returns true when broadphase dispatch should early-out (B4.2 deepen follow-up).
bool can_skip_broadphase_dispatch(

/// Const preflight for refine dispatch (B4.2 deepen follow-up).
    bool noValidPairs = false;
    bool skippedBroadphase = false;


/// Populate refine preflight without invalidating pair slots (B4.2 deepen follow-up).

/// Returns true when refine should early-out before parallel invalidation (B4.2 deepen follow-up).

/// Const preflight for dedupe dispatch (B4.2 deepen follow-up).
struct DedupeBroadphasePreflight {
    u32 activeCount = 0;

    bool can_dedupe() const { return !skipped; }

/// Populate dedupe preflight without mutating the pair buffer (B4.2 deepen follow-up).
DedupeBroadphasePreflight preflight_dedupe_broadphase(const PairBufferSoA& buffer);

/// Returns true when sort+unique dedupe would leave the buffer unchanged (B4.2 deepen follow-up).
bool should_skip_dedupe_broadphase(const PairBufferSoA& buffer);

/// Const preflight for shape→cell occupancy insertion (B4.2 deepen follow-up).
    u32 occupancyCount = 0;
    u32 budgetRemaining = 0;

    bool can_insert() const { return !skipped; }

/// Populate 3D cell-occupancy preflight without hash insertion (B4.2 deepen follow-up).

/// Populate 2D cell-occupancy preflight without hash insertion (B4.2 deepen follow-up).
CellOccupancyPreflight preflight_cell_occupancy_2d(const CellRange2& range, u32 maxCells);

/// Returns true when shape→cell insertion should be skipped for occupancy (B4.2 deepen follow-up).
bool should_skip_shape_cell_insertion(const CellRange3& range, u32 maxCells);

/// Returns true when 2D shape→cell insertion should be skipped for occupancy (B4.2 deepen follow-up).
bool should_skip_shape_cell_insertion_2d(const CellRange2& range, u32 maxCells);

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

/// Why broadphase pair refine would early-out (B4.2 deepen follow-up pass).
enum class RefineBroadphaseRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    EmptyInput,
    NoValidPairs,
};

/// Human-readable label for refine reject reasons (logging / tests).
const char* refineBroadphaseRejectReasonName(RefineBroadphaseRejectReason reason);

/// Diagnose why refine would skip; vacuously succeeds when refine may proceed.
RefineBroadphaseRejectReason refineBroadphaseRejectReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer);

/// Returns true when `refineBroadphaseRejectReason` matches `expected` (B4.2 deepen follow-up pass).
bool refineBroadphaseRejectsForReason(
    const PairBufferSoA& buffer,
    RefineBroadphaseRejectReason expected);

/// Read-only refine diagnostics — no mutation (B4.2 deepen follow-up).
struct RefineBroadphasePreflight {
    RefineBroadphaseRejectReason reason = RefineBroadphaseRejectReason::None;
    bool emptyBuffer = false;
    bool emptyInput = false;
    bool noValidPairs = false;

    bool canRefine() const { return reason == RefineBroadphaseRejectReason::None; }

RefineBroadphasePreflight preflightRefineBroadphase(

/// Non-mutating refine predicate — same guards as `preflightRefineBroadphase`.
bool canSkipRefineBroadphase(

/// Non-mutating refine predicate — inverse of `canSkipRefineBroadphase` (B4.2 deepen pass).
bool shouldRunRefineBroadphase(

/// Early-out when refine preflight would reject — same ordering as `canSkipRefineBroadphase` (B4.2 deepen pass).
bool wouldSkipRefineBroadphase(
    RefineBroadphaseRejectReason* reason = nullptr);

/// Non-mutating refine launch predicate — inverse of `canSkipRefineBroadphase` (B4.2 deepen pass).
bool shouldRunRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer);

/// Non-mutating refine predicate — inverse of `canSkipRefineBroadphase` (B4.2 deepen pass).
bool shouldRunRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer);

/// Why broadphase pair dedupe would early-out (B4.2 deepen follow-up pass).
enum class DedupeBroadphaseRejectReason : u8 {
    SinglePair,

/// Human-readable label for dedupe reject reasons (logging / tests).
const char* dedupeBroadphaseRejectReasonName(DedupeBroadphaseRejectReason reason);

/// Diagnose why dedupe would skip; vacuously succeeds when dedupe may proceed.
DedupeBroadphaseRejectReason dedupeBroadphaseRejectReason(const PairBufferSoA& buffer);

/// Returns true when `dedupeBroadphaseRejectReason` matches `expected` (B4.2 deepen follow-up pass).
bool dedupeBroadphaseRejectsForReason(const PairBufferSoA& buffer, DedupeBroadphaseRejectReason expected);

/// Read-only dedupe diagnostics — no mutation (B4.2 deepen follow-up).
struct DedupeBroadphasePreflight {
    DedupeBroadphaseRejectReason reason = DedupeBroadphaseRejectReason::None;
    bool singlePair = false;

    bool canDedupe() const { return reason == DedupeBroadphaseRejectReason::None; }
    bool canRefine() const { return !emptyBuffer && !emptyInput && !noValidPairs; }
};

    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer);


    bool emptyBuffer = false;

    bool canDedupe() const { return !emptyBuffer && !singlePair; }

DedupeBroadphasePreflight preflightDedupeBroadphase(const PairBufferSoA& buffer);

/// Non-mutating dedupe predicate — mirrors `PairBufferSoA::canSkipDedupe` inversion.
bool shouldRunDedupeBroadphase(const PairBufferSoA& buffer);

/// Non-mutating dedupe skip predicate — inverse of `shouldRunDedupeBroadphase` (B4.2 deepen follow-up pass).
bool canSkipDedupeBroadphase(const PairBufferSoA& buffer);

/// Early-out when dedupe preflight would reject — same ordering as `canSkipDedupeBroadphase` (B4.2 deepen pass).
bool wouldSkipDedupeBroadphase(const PairBufferSoA& buffer, DedupeBroadphaseRejectReason* reason = nullptr);

/// Why plane/dynamic merge would early-out (B4.2 deepen pass).
enum class BroadphaseMergeRejectReason : u8 {
    EmptyPlaneBodies,
    EmptyDynamicBodies,

/// Human-readable label for merge reject reasons (logging / tests).
const char* mergeBroadphaseRejectReasonName(BroadphaseMergeRejectReason reason);

/// Diagnose why merge would skip; vacuously succeeds when merge may proceed.
BroadphaseMergeRejectReason mergeBroadphaseRejectReason(
    const CollisionShapeSoA& shapes);

/// Returns true when `mergeBroadphaseRejectReason` matches `expected` (B4.2 deepen pass).
bool mergeBroadphaseRejectsForReason(
    None = 0,
};

const char* broadphaseMergeRejectReasonName(BroadphaseMergeRejectReason reason);

BroadphaseMergeRejectReason broadphaseMergeRejectReason(
    const RigidBodySoA& bodies,

/// Returns true when `broadphaseMergeRejectReason` matches `expected` (B4.2 deepen pass).
bool broadphaseMergeRejectsForReason(
/// Why plane/dynamic merge would early-out (B4.2 deepen follow-up pass).



/// Returns true when `broadphaseMergeRejectReason` matches `expected` (B4.2 deepen follow-up pass).









    const CollisionShapeSoA& shapes,
    BroadphaseMergeRejectReason expected);

/// Read-only plane/dynamic merge diagnostics — no mutation (B4.2 deepen follow-up pass).
struct BroadphaseMergePreflight {
    BroadphaseMergeRejectReason reason = BroadphaseMergeRejectReason::None;
    bool emptyPlaneBodies = false;
    bool emptyDynamicBodies = false;

    bool canMerge() const { return reason == BroadphaseMergeRejectReason::None; }
};

BroadphaseMergePreflight preflightBroadphaseMerge(

/// Non-mutating merge skip predicate — inverse of `shouldRunBroadphaseMerge` (B4.2 deepen pass).
bool canSkipBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes);

/// Non-mutating merge predicate — mirrors `preflightBroadphaseMerge` (B4.2 deepen pass).
bool shouldRunBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes);

/// Early-out when plane/dynamic merge preflight would reject — same ordering as `canSkipBroadphaseMerge` (B4.2 deepen pass).
bool wouldSkipBroadphaseMerge(
    BroadphaseMergeRejectReason* reason = nullptr);

/// Why merge-into-buffer would early-out before pushing pairs (B4.2 deepen pass).
enum class MergePairsIntoBufferRejectReason : u8 {
    EmptyPairs,
    BufferFull,

/// Human-readable label for merge-into-buffer reject reasons (logging / tests).
const char* mergePairsIntoBufferRejectReasonName(MergePairsIntoBufferRejectReason reason);

/// Diagnose why merge-into-buffer would skip; vacuously succeeds when merge may proceed.
MergePairsIntoBufferRejectReason mergePairsIntoBufferRejectReason(
    const std::vector<CandidatePair>& pairs,

/// Returns true when `mergePairsIntoBufferRejectReason` matches `expected` (B4.2 deepen pass).
bool mergePairsIntoBufferRejectsForReason(
    MergePairsIntoBufferRejectReason expected);

/// Read-only merge-into-buffer diagnostics — no mutation (B4.2 deepen pass).
struct MergePairsIntoBufferPreflight {
    MergePairsIntoBufferRejectReason reason = MergePairsIntoBufferRejectReason::None;
    bool emptyPairs = false;
    bool bufferFull = false;

    bool canMerge() const { return reason == MergePairsIntoBufferRejectReason::None; }

MergePairsIntoBufferPreflight preflightMergePairsIntoBuffer(

/// Non-mutating merge-into-buffer skip predicate — inverse of `canMerge` (B4.2 deepen pass).
bool canSkipMergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, const PairBufferSoA& buffer);

/// Non-mutating merge-into-buffer predicate — mirrors `preflightMergePairsIntoBuffer` (B4.2 deepen pass).
bool shouldRunMergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, const PairBufferSoA& buffer);

/// Early-out when merge-into-buffer preflight would reject — same ordering as `canSkipMergePairsIntoBuffer` (B4.2 deepen pass).
bool wouldSkipMergePairsIntoBuffer(
    MergePairsIntoBufferRejectReason* reason = nullptr);

/// Const preflight for parallel pair refine dispatch (B4.2 deepen pass).
    u32 pairCount = 0;
    u32 validPairCount = 0;
    bool skipped = false;

    bool can_refine() const { return !skipped && validPairCount > 0u; }

RefineBroadphasePreflight preflight_refine_broadphase(

/// True when refineBroadphasePairsParallel may early-out (B4.2 deepen pass).
bool should_skip_refine_broadphase(

/// Non-mutating merge skip predicate — inverse of `preflightBroadphaseMerge` (B4.2 deepen pass).
bool canSkipBroadphaseMerge(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating merge launch predicate — mirrors `preflightBroadphaseMerge` (B4.2 deepen pass).
bool shouldRunBroadphaseMerge(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating merge skip predicate — inverse of `preflightBroadphaseMerge().canMerge()`.
bool canSkipBroadphaseMerge(

/// Non-mutating merge predicate — inverse of `canSkipBroadphaseMerge` (B4.2 deepen pass).

/// Non-mutating merge skip predicate — mirrors `preflightBroadphaseMerge` inversion (B4.2 deepen pass).

/// Non-mutating merge predicate — inverse of `preflightBroadphaseMerge().canMerge()`.
bool canSkipBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes);

/// Non-mutating merge predicate — mirrors `preflightBroadphaseMerge().canMerge()`.
bool shouldRunBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes);

/// Non-mutating merge skip predicate — inverse of `preflightBroadphaseMerge` (B4.2 deepen pass).

/// Parallel pair refine stub: invalidate separated pairs via `sphereAabbOverlap`, then compact.
void refineBroadphasePairsParallel(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    PairBufferSoA& buffer);

/// Refine only when `preflightRefineBroadphase` allows; returns false when skipped (B4.2 deepen follow-up pass).
bool refineBroadphasePairsParallelWithPreflight(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    PairBufferSoA& buffer);

/// Dedupe pair buffer only when `preflightDedupeBroadphase` allows; returns false when skipped (B4.2 deepen pass).
bool dedupeBroadphasePairBufferWithPreflight(PairBufferSoA& buffer);

/// Merge candidate pairs into buffer only when `preflightMergePairsIntoBuffer` allows (B4.2 deepen follow-up pass).
void mergePairsIntoBufferWithPreflight(const std::vector<CandidatePair>& pairs, PairBufferSoA& buffer);

/// Why broadphase cell-pair generation would early-out (B4.2 deepen pass).
enum class BroadphaseCellPairGenRejectReason : u8 {
    None = 0,
    EmptyCells,
};

/// Human-readable label for broadphase cell-pair generation reject reasons (logging / tests).
const char* broadphaseCellPairGenRejectReasonName(BroadphaseCellPairGenRejectReason reason);

/// Diagnose why cell-pair generation would skip; vacuously succeeds when generation may proceed.
BroadphaseCellPairGenRejectReason broadphaseCellPairGenRejectReason(u32 totalCellSlots);

/// Returns true when `broadphaseCellPairGenRejectReason` matches `expected` (B4.2 deepen pass).
bool broadphaseCellPairGenRejectsForReason(u32 totalCellSlots, BroadphaseCellPairGenRejectReason expected);

/// Read-only broadphase cell-pair generation diagnostics — no mutation (B4.2 deepen pass).
struct BroadphaseCellPairGenPreflight {
    BroadphaseCellPairGenRejectReason reason = BroadphaseCellPairGenRejectReason::None;
    bool emptyCells = false;

    bool canGenerate() const { return reason == BroadphaseCellPairGenRejectReason::None; }

BroadphaseCellPairGenPreflight preflightBroadphaseCellPairGen(u32 totalCellSlots);

/// Non-mutating broadphase cell-pair generation skip predicate — inverse of `canGenerate` (B4.2 deepen pass).
bool canSkipBroadphaseCellPairGen(u32 totalCellSlots);

/// Non-mutating broadphase cell-pair generation predicate — mirrors `preflightBroadphaseCellPairGen` (B4.2 deepen pass).
bool shouldRunBroadphaseCellPairGen(u32 totalCellSlots);
/// Remove invalid pairs in-place (self-pair, optional OOB); returns removed count.
u32 pruneInvalidCandidatePairs(std::vector<CandidatePair>& pairs, u32 bodyCount = 0u);

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
