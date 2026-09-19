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

u32 pruneInvalidCandidatePairs(std::vector<CandidatePair>& pairs, u32 bodyCount) {
    const auto isInvalid = [&](const CandidatePair& pair) {
        return !isValidCandidatePair(pair, bodyCount);
    };
    const u32 before = static_cast<u32>(pairs.size());
    pairs.erase(std::remove_if(pairs.begin(), pairs.end(), isInvalid), pairs.end());
    return before - static_cast<u32>(pairs.size());
bool PairBufferPreflight::can_push(u32 additionalCount) const {
    if (additionalCount == 0u) {
        return true;
    }
    if (full) {
        return false;
    return remaining >= additionalCount;

BroadphaseInputPreflight preflight_broadphase_input(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    BroadphaseInputPreflight preflight{};
    preflight.emptyBodies = bodies.count() == 0u;
    preflight.emptyShapes = shapes.count() == 0u;
    preflight.skipped = isEmptyBroadphaseInput(bodies, shapes);
    return preflight;

PairBufferPreflight preflight_pair_buffer(const PairBufferSoA& buffer) {
    PairBufferPreflight preflight{};
    preflight.activeCount = buffer.activeCount;
    preflight.remaining = buffer.remainingCapacity();
    preflight.empty = buffer.isEmpty();
    preflight.full = buffer.isFull();
    preflight.hasDropped = buffer.hasDroppedPairs();

BroadphaseRefinePreflight preflight_broadphase_refine(
    const PairBufferSoA& buffer,
    BroadphaseRefinePreflight preflight{};
    preflight.emptyBuffer = buffer.canSkipRefine();
    preflight.emptyInput = isEmptyBroadphaseInput(bodies, shapes);
    preflight.pairCount = buffer.activeCount;
    preflight.skipped = preflight.emptyBuffer || preflight.emptyInput;

BroadphaseDedupePreflight preflight_broadphase_dedupe(const PairBufferSoA& buffer) {
    BroadphaseDedupePreflight preflight{};
    preflight.skipped = buffer.canSkipSoAIteration();
    preflight.noOp = buffer.canSkipDedupe();
BroadphaseDedupePreflight preflight_dedupe_pairs(const PairBufferSoA& buffer) {
    preflight.validPairCount = buffer.countValidSlots();
    preflight.skipped = buffer.canSkipDedupe();

BroadphaseRefinePreflight preflight_refine_broadphase_pairs(
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    preflight.emptyInput = canSkipBroadphase(bodies, shapes);
    preflight.emptyBuffer = buffer.canSkipSoAIteration() || !buffer.hasValidPairs();
    preflight.skipped = preflight.emptyInput || preflight.emptyBuffer;

bool should_skip_refine_broadphase_pairs(
    return preflight_refine_broadphase_pairs(bodies, shapes, buffer).skipped;
    preflight.emptyScene = canSkipBroadphase(bodies, shapes);
    preflight.skipped = preflight.emptyScene || preflight.emptyBuffer;

bool should_skip_broadphase_refine(
    return preflight_broadphase_refine(bodies, shapes, buffer).skipped;
BroadphasePreflight preflight_broadphase(
    BroadphasePreflight preflight{};
    preflight.singletonInput = isSingletonBroadphaseInput(bodies, shapes);
    preflight.skipped = preflight.emptyInput || preflight.singletonInput;

bool can_skip_broadphase_dispatch(
    return preflight_broadphase(bodies, shapes).skipped;

RefineBroadphasePreflight preflight_refine_broadphase(
    RefineBroadphasePreflight preflight{};
    preflight.emptyBuffer = buffer.canSkipSoAIteration();
    preflight.noValidPairs = !buffer.hasValidPairs();
    preflight.skippedBroadphase = canSkipBroadphase(bodies, shapes);
    preflight.skipped =
        preflight.emptyBuffer || preflight.noValidPairs || preflight.skippedBroadphase;

bool should_skip_refine_broadphase(
    return preflight_refine_broadphase(bodies, shapes, buffer).skipped;

DedupeBroadphasePreflight preflight_dedupe_broadphase(const PairBufferSoA& buffer) {
    DedupeBroadphasePreflight preflight{};
    preflight.skipped = buffer.canSkipDedupePass();

bool should_skip_dedupe_broadphase(const PairBufferSoA& buffer) {
    return preflight_dedupe_broadphase(buffer).skipped;

CellOccupancyPreflight preflight_cell_occupancy(const CellRange3& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.emptyRange = isEmptyCellRange(range);
    preflight.occupancyCount = estimateCellOccupancyCount(range);
    preflight.budgetRemaining = occupancyBudgetRemaining(range, maxCells);
    preflight.exceedsBudget = exceedsCellOccupancyBudget(range, maxCells);
    preflight.skipped = preflight.emptyRange || preflight.exceedsBudget;

CellOccupancyPreflight preflight_cell_occupancy_2d(const CellRange2& range, u32 maxCells) {

bool should_skip_shape_cell_insertion(const CellRange3& range, u32 maxCells) {
    return preflight_cell_occupancy(range, maxCells).skipped;

bool should_skip_shape_cell_insertion_2d(const CellRange2& range, u32 maxCells) {
    return preflight_cell_occupancy_2d(range, maxCells).skipped;
}

const char* candidatePairRejectReasonName(CandidatePairRejectReason reason) {
const char* candidate_pair_reject_reason_name(CandidatePairRejectReason reason) {
    switch (reason) {
    case CandidatePairRejectReason::None:
        return "None";
    case CandidatePairRejectReason::SelfPair:
        return "SelfPair";
    case CandidatePairRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    case CandidatePairRejectReason::AabbSeparated:
        return "AabbSeparated";
    case CandidatePairRejectReason::BufferFull:
        return "BufferFull";
    }
    return "Unknown";

const char* cellSpanRejectReasonName(CellSpanRejectReason reason) {
    case CellSpanRejectReason::None:
    case CellSpanRejectReason::EmptyRange:
        return "EmptyRange";
    case CellSpanRejectReason::ExceedsSpan:
        return "ExceedsSpan";

const char* cellOccupancyRejectReasonName(CellOccupancyRejectReason reason) {
    case CellOccupancyRejectReason::None:
    case CellOccupancyRejectReason::EmptyRange:
    case CellOccupancyRejectReason::ExceedsBudget:
        return "ExceedsBudget";

const char* broadphaseRejectReasonName(BroadphaseRejectReason reason) {
    case BroadphaseRejectReason::None:
    case BroadphaseRejectReason::EmptyInput:
        return "EmptyInput";
    case BroadphaseRejectReason::SingletonInput:
        return "SingletonInput";

const char* refineBroadphaseRejectReasonName(RefineBroadphaseRejectReason reason) {
    case RefineBroadphaseRejectReason::None:
    case RefineBroadphaseRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case RefineBroadphaseRejectReason::EmptyInput:
    case RefineBroadphaseRejectReason::NoValidPairs:
        return "NoValidPairs";

const char* dedupeBroadphaseRejectReasonName(DedupeBroadphaseRejectReason reason) {
    case DedupeBroadphaseRejectReason::None:
    case DedupeBroadphaseRejectReason::EmptyBuffer:
    case DedupeBroadphaseRejectReason::SinglePair:
        return "SinglePair";

const char* cellPairGenRejectReasonName(CellPairGenRejectReason reason) {
    case CellPairGenRejectReason::None:
    case CellPairGenRejectReason::EmptyCell:
        return "EmptyCell";
    case CellPairGenRejectReason::SingletonOccupant:
        return "SingletonOccupant";

const char* shapeCellInsertRejectReasonName(ShapeCellInsertRejectReason reason) {
    case ShapeCellInsertRejectReason::None:
    case ShapeCellInsertRejectReason::OutOfRangeBody:
    case ShapeCellInsertRejectReason::OccupancySkipped:
        return "OccupancySkipped";

const char* broadphaseCellPairGenRejectReasonName(BroadphaseCellPairGenRejectReason reason) {
    case BroadphaseCellPairGenRejectReason::None:
    case BroadphaseCellPairGenRejectReason::EmptyCells:
        return "EmptyCells";

const char* mergeBroadphaseRejectReasonName(BroadphaseMergeRejectReason reason) {
    case BroadphaseMergeRejectReason::None:
    case BroadphaseMergeRejectReason::EmptyPlaneBodies:
        return "EmptyPlaneBodies";
    case BroadphaseMergeRejectReason::EmptyDynamicBodies:
        return "EmptyDynamicBodies";
const char* candidateRejectReasonLabel(CandidateRejectReason reason) {
    case CandidateRejectReason::None:
        return "none";
    case CandidateRejectReason::SelfPair:
        return "self_pair";
    case CandidateRejectReason::OutOfRangeBody:
        return "out_of_range_body";
    case CandidateRejectReason::AabbSeparated:
        return "aabb_separated";
    case CandidateRejectReason::BufferFull:
        return "buffer_full";
    return "unknown";
}

u32 pruneInvalidCandidatePairs(std::vector<CandidatePair>& pairs, u32 bodyCount) {
    const auto invalidIt = std::remove_if(pairs.begin(), pairs.end(), [&](const CandidatePair& pair) {
        return !isValidCandidatePair(pair, bodyCount);
    });
    const u32 removed = static_cast<u32>(std::distance(invalidIt, pairs.end()));
    pairs.erase(invalidIt, pairs.end());
    return static_cast<u32>(pairs.size());
}

BroadphasePreflight preflight_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    BroadphasePreflight preflight{};
    preflight.bodyCount = bodies.count();
    preflight.shapeCount = shapes.count();
    preflight.emptyInput = isEmptyBroadphaseInput(bodies, shapes);
    preflight.skipped = preflight.emptyInput;
    return preflight;
}

BroadphaseRefinePreflight preflight_broadphase_refine(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    BroadphaseRefinePreflight preflight{};
    preflight.pairCount = buffer.activeCount;
    preflight.emptyInput = isEmptyBroadphaseInput(bodies, shapes);
    preflight.emptyBuffer = buffer.canSkipRefine();
    preflight.skipped = preflight.emptyInput || preflight.emptyBuffer;
    return preflight;
}

BroadphaseInputPreflight preflight_broadphase_input(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    BroadphaseInputPreflight preflight{};
    preflight.emptyBodies = bodies.count() == 0u;
    preflight.emptyShapes = shapes.count() == 0u;
    preflight.skipped = preflight.emptyBodies || preflight.emptyShapes;
    return preflight;
}

bool should_skip_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflight_broadphase_input(bodies, shapes).skipped;
}

CellOccupancyPreflight preflight_cell_occupancy(const CellRange3& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.maxCells = maxCells;
    preflight.emptyRange = isEmptyCellRange(range);
    if (preflight.emptyRange) {
        preflight.skipped = true;
        return preflight;
    }
    preflight.estimatedCells = estimateCellOccupancyCount(range);
    preflight.exceedsBudget = maxCells > 0u && preflight.estimatedCells > maxCells;
    return preflight;
}

CellOccupancyPreflight preflight_cell_occupancy(const CellRange2& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.maxCells = maxCells;
    preflight.emptyRange = isEmptyCellRange(range);
    if (preflight.emptyRange) {
        preflight.skipped = true;
        return preflight;
    }
    preflight.estimatedCells = estimateCellOccupancyCount(range);
    preflight.exceedsBudget = maxCells > 0u && preflight.estimatedCells > maxCells;
    return preflight;
}

RefineBroadphasePreflight preflight_refine_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    RefineBroadphasePreflight preflight{};
    preflight.emptyBuffer = buffer.canSkipSoAIteration() || !buffer.hasValidPairs();
    preflight.emptyInput = canSkipBroadphase(bodies, shapes);
    preflight.skipped = preflight.emptyBuffer || preflight.emptyInput;
    return preflight;
}

bool should_skip_refine_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return preflight_refine_broadphase(bodies, shapes, buffer).skipped;
}

BroadphaseRefinePreflight preflightRefineBroadphasePairs(
    const PairBufferSoA& buffer,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    BroadphaseRefinePreflight preflight{};
    preflight.emptyBroadphaseInput = canSkipBroadphase(bodies, shapes);
    preflight.emptyBuffer = buffer.canSkipSoAIteration() || !buffer.hasValidPairs();
    preflight.skipped = preflight.emptyBroadphaseInput || preflight.emptyBuffer;
    return preflight;
}

bool canSkipRefineBroadphasePairs(
    const PairBufferSoA& buffer,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflightRefineBroadphasePairs(buffer, bodies, shapes).skipped;
}

bool canSkipDedupeBuffer(const PairBufferSoA& buffer) {
    return buffer.canSkipDedupe();
}

BroadphaseInputPreflight preflight_broadphase_input(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    BroadphaseInputPreflight preflight{};
    preflight.emptyBodies = bodies.count() == 0u;
    preflight.emptyShapes = shapes.count() == 0u;
    preflight.skipped = preflight.emptyBodies || preflight.emptyShapes;
    return preflight;
}

bool should_skip_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflight_broadphase_input(bodies, shapes).skipped;
}

CellOccupancyPreflight preflight_cell_occupancy(const CellRange3& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.maxCells = maxCells;
    preflight.emptyRange = isEmptyCellRange(range);
    if (preflight.emptyRange) {
        preflight.skipped = true;
        return preflight;
    }
    preflight.estimatedCells = estimateCellOccupancyCount(range);
    preflight.exceedsBudget = maxCells > 0u && preflight.estimatedCells > maxCells;
    return preflight;
}

CellOccupancyPreflight preflight_cell_occupancy(const CellRange2& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.maxCells = maxCells;
    preflight.emptyRange = isEmptyCellRange(range);
    if (preflight.emptyRange) {
        preflight.skipped = true;
        return preflight;
    }
    preflight.estimatedCells = estimateCellOccupancyCount(range);
    preflight.exceedsBudget = maxCells > 0u && preflight.estimatedCells > maxCells;
    return preflight;
}

RefineBroadphasePreflight preflight_refine_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    RefineBroadphasePreflight preflight{};
    preflight.emptyBuffer = buffer.canSkipSoAIteration() || !buffer.hasValidPairs();
    preflight.emptyInput = canSkipBroadphase(bodies, shapes);
    preflight.skipped = preflight.emptyBuffer || preflight.emptyInput;
    return preflight;
}

bool should_skip_refine_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return preflight_refine_broadphase(bodies, shapes, buffer).skipped;
}

BroadphaseRefinePreflight preflight_broadphase_refine(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    BroadphaseRefinePreflight preflight{};
    preflight.emptyInput = canSkipBroadphase(bodies, shapes);
    preflight.emptyBuffer = buffer.canSkipSoAIteration() || !buffer.hasValidPairs();
    preflight.validPairCount = buffer.countValidSlots();
    preflight.skipped = preflight.emptyInput || preflight.emptyBuffer || preflight.validPairCount == 0u;
    return preflight;
}

bool canSkipRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return !preflight_broadphase_refine(bodies, shapes, buffer).can_refine();
}

BroadphasePreflight preflight_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    BroadphasePreflight preflight{};
    preflight.bodyCount = bodies.count();
    preflight.shapeCount = shapes.count();
    preflight.skipped = isEmptyBroadphaseInput(bodies, shapes);
    return preflight;
}

bool should_skip_broadphase(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    return !preflight_broadphase(bodies, shapes).can_dispatch();
}

CellOccupancyPreflight preflight_cell_occupancy(const CellRange3& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.maxCells = maxCells;
    preflight.emptyRange = isEmptyCellRange(range);
    if (preflight.emptyRange) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.estimatedCells = estimateCellOccupancyCount(range);
    preflight.exceedsBudget = exceedsCellOccupancyBudget(range, maxCells);
    preflight.skipped = preflight.exceedsBudget;
    return preflight;
}

CellOccupancyPreflight preflight_cell_occupancy(const CellRange2& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.maxCells = maxCells;
    preflight.emptyRange = isEmptyCellRange(range);
    if (preflight.emptyRange) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.estimatedCells = estimateCellOccupancyCount(range);
    preflight.exceedsBudget = exceedsCellOccupancyBudget(range, maxCells);
    preflight.skipped = preflight.exceedsBudget;
    return preflight;
}

RefineBroadphasePreflight preflight_refine_broadphase_pairs(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    RefineBroadphasePreflight preflight{};
    preflight.pairCount = buffer.activeCount;
    preflight.bodyCount = bodies.count();
    preflight.shapeCount = shapes.count();
    preflight.skipped =
        should_skip_broadphase(bodies, shapes) || buffer.canSkipRefine();
    return preflight;
}

bool should_skip_refine_broadphase_pairs(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return !preflight_refine_broadphase_pairs(bodies, shapes, buffer).can_refine();
}

BroadphaseInputPreflight preflight_broadphase_input(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    BroadphaseInputPreflight preflight{};
    preflight.bodyCount = bodies.count();
    preflight.shapeCount = shapes.count();
    preflight.emptyBodies = preflight.bodyCount == 0u;
    preflight.emptyShapes = preflight.shapeCount == 0u;
    preflight.skipped = isEmptyBroadphaseInput(bodies, shapes);
    return preflight;
}

bool should_skip_broadphase_build(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflight_broadphase_input(bodies, shapes).skipped;
}

CellOccupancyPreflight preflight_cell_occupancy(const CellRange3& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.maxCells = maxCells;
    preflight.emptyRange = isEmptyCellRange(range);
    if (preflight.emptyRange) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.occupancyCount = estimateCellOccupancyCount(range);
    preflight.exceedsBudget = exceedsCellOccupancyBudget(range, maxCells);
    return preflight;
}

CellOccupancyPreflight preflight_cell_occupancy(const CellRange2& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.maxCells = maxCells;
    preflight.emptyRange = isEmptyCellRange(range);
    if (preflight.emptyRange) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.occupancyCount = estimateCellOccupancyCount(range);
    preflight.exceedsBudget = exceedsCellOccupancyBudget(range, maxCells);
    return preflight;
}

bool should_skip_shape_cell_insert(const CellRange3& range, u32 maxCells) {
    return preflight_cell_occupancy(range, maxCells).skipped;
}

bool should_skip_shape_cell_insert(const CellRange2& range, u32 maxCells) {
    return preflight_cell_occupancy(range, maxCells).skipped;
}

RefineBroadphasePreflight preflight_refine_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    RefineBroadphasePreflight preflight{};
    preflight.emptyInput = canSkipBroadphase(bodies, shapes);
    preflight.emptyBuffer = buffer.canSkipSoAIteration() || !buffer.hasValidPairs();
    preflight.pairCount = buffer.activeCount;
    preflight.validPairCount = buffer.countValidSlots();
    preflight.skipped = preflight.emptyInput || preflight.emptyBuffer;
    return preflight;
}

bool should_skip_refine_broadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return preflight_refine_broadphase(bodies, shapes, buffer).skipped;
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

const char* refineBroadphaseRejectReasonName(RefineBroadphaseRejectReason reason) {
    case RefineBroadphaseRejectReason::None:
    case RefineBroadphaseRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case RefineBroadphaseRejectReason::EmptyInput:
    case RefineBroadphaseRejectReason::NoValidPairs:
        return "NoValidPairs";

const char* broadphaseMergeRejectReasonName(BroadphaseMergeRejectReason reason) {
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

const char* dedupeBroadphaseRejectReasonName(DedupeBroadphaseRejectReason reason) {
    case DedupeBroadphaseRejectReason::None:
    case DedupeBroadphaseRejectReason::EmptyBuffer:
    case DedupeBroadphaseRejectReason::SinglePair:
        return "SinglePair";

const char* broadphaseMergeRejectReasonName(BroadphaseMergeRejectReason reason) {
    case BroadphaseMergeRejectReason::None:
    case BroadphaseMergeRejectReason::EmptyPlaneBodies:
        return "EmptyPlaneBodies";
    case BroadphaseMergeRejectReason::EmptyDynamicBodies:
        return "EmptyDynamicBodies";

const char* broadphaseMergeRejectReasonName(BroadphaseMergeRejectReason reason) {
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
    if (isRejectedCandidatePair(bodyA, bodyB)) {
        return;
    }
    pairs.push_back(canonicalPair(bodyA, bodyB));
}

void dedupePairs(std::vector<CandidatePair>& pairs) {
    pruneInvalidCandidatePairs(pairs);
    if (canSkipPairListDedupe(static_cast<u32>(pairs.size()))) {
        return;
    }
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
u32 countPairsForCell(const std::vector<u32>& occupants) {
    if (shouldSkipCellPairGeneration(static_cast<u32>(occupants.size()))) {
    if (isEmptyCellBucket(occupants.size())) {
    if (canSkipCellPairGeneration(static_cast<u32>(occupants.size()))) {
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
    if (shouldSkipCellPairGeneration(static_cast<u32>(occupants.size()))) {
    if (isEmptyCellBucket(occupants.size())) {
    if (canSkipCellPairGeneration(static_cast<u32>(occupants.size()))) {
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
    u32 bodyCount,
    PairBufferSoA& buffer) {
    if (!preflightCellPairGenerationImpl(occupants).canGenerate()) {
    if (shouldSkipCellPairGeneration(static_cast<u32>(occupants.size()))) {
    if (isEmptyCellBucket(occupants.size())) {
    if (canSkipCellPairGeneration(static_cast<u32>(occupants.size()))) {
        return;
    }
    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    u32 slot = slotStart;
    for (usize i = 0; i < uniqueBodies.size(); ++i) {
        for (usize j = i + 1; j < uniqueBodies.size(); ++j) {
            buffer.writeSlot(slot++, uniqueBodies[i], uniqueBodies[j], bodyCount);
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
    const u32 maxOccupancy = params.maxCellOccupancyCount;
    const u32 occupancyBudget = use2D ? cellOccupancyBudgetFromSpan2D(maxSpan) : cellOccupancyBudgetFromSpan(maxSpan);
    const u32 occupancyBudget = estimateMaxCellOccupancyBudget(maxSpan, !use2D);
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
        if (canSkipCellOccupancyIteration(range, maxOccupancy)) {
            return ShapeCellInsertRejectReason::OccupancySkipped;
        }
        return ShapeCellInsertRejectReason::None;

    CellRange3 range = {};
    if (type == CollisionShapeType::Box) {
        const vec3 halfExtents = shapes.params[shapeIndex];
        range = cellRangeFromBox(position, halfExtents, cellSize, maxSpan);
    } else {
        const f32 radius = shapeRadius(shapes, shapeIndex);
        range = cellRangeFromSphere(position, radius, cellSize, maxSpan);

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
                const aabb bounds = aabbFromBox(position, halfExtents);
                range = cellRangeFromAabb2D(bounds, cellSize, maxSpan);
                range = cellRangeFromSphere2D({position.x, position.y}, radius, cellSize, maxSpan);
            preflight.occupancyCount = estimateCellOccupancyCount(range);

    return preflight;

void populateShapeCells(
    bool use2D,
    CellBuckets& cells) {
    const ShapeCellInsertPreflight insertPreflight =
        preflightShapeCellInsertImpl(shapeIndex, bodies, shapes, params, use2D);
    if (!insertPreflight.canInsert()) {
        return;

    const u32 tableSize = clampTableSize(params.tableSize);
    const u32 occupancyBudget = use2D ? cellOccupancyBudgetFromSpan2D(maxSpan) : cellOccupancyBudgetFromSpan(maxSpan);

        if (isEmptyCellRange(range) || exceedsCellOccupancyBudget(range, occupancyBudget)) {
        if (canSkipShapeCellInsertion(range, maxOccupancy)) {
        const ShapeCellOccupancyPreflight occupancyPreflight =
            preflight_shape_cell_occupancy(range, maxSpan);
        if (!occupancyPreflight.can_insert()) {
        if (isEmptyCellRange(range)) {
        if (canSkipCellOccupancyInsert(range, params.maxCellOccupancy)) {
        const CellOccupancyPreflight occupancyPreflight =
            preflightCellOccupancy(range, perShapeCellBudget(maxSpan, true));
        if (occupancyPreflight.skipped || occupancyPreflight.exceedsBudget) {
        if (!preflight_cell_occupancy(range).can_insert_cells()) {
        if (!preflight_cell_occupancy(range, occupancyBudget).can_iterate()) {
        if (should_skip_shape_cell_population(range, 0u)) {
        if (should_skip_shape_cell_insertion(range, maxSpan)) {
        if (should_skip_shape_cell_insert(range)) {
        if (should_skip_shape_cell_insertion_2d(range, maxOccupancy)) {
            return;
        }
        if (params.maxCellOccupancyPerShape > 0u) {
            range = shrinkCellRangeToOccupancyBudget(range, params.maxCellOccupancyPerShape);
            if (isEmptyCellRange(range)) {
                return;
            }
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
    if (isEmptyCellRange(range) || exceedsCellOccupancyBudget(range, occupancyBudget)) {
    if (canSkipShapeCellInsertion(range, maxOccupancy)) {
    const ShapeCellOccupancyPreflight occupancyPreflight = preflight_shape_cell_occupancy(range, maxSpan);
    if (!occupancyPreflight.can_insert()) {
    const CellOccupancyPreflight occupancyPreflight =
        preflightCellOccupancy(range, perShapeCellBudget(maxSpan, false));
    if (occupancyPreflight.skipped || occupancyPreflight.exceedsBudget) {
    if (canSkipCellOccupancyInsert(range, params.maxCellOccupancy)) {
    if (!preflight_cell_occupancy(range).can_insert_cells()) {
    if (!preflight_cell_occupancy(range, occupancyBudget).can_iterate()) {
    if (should_skip_shape_cell_population(range, 0u)) {
    if (should_skip_shape_cell_insertion(range, maxSpan)) {
    if (should_skip_shape_cell_insert(range)) {
    if (should_skip_shape_cell_insertion(range, maxOccupancy)) {
    if (canSkipCellOccupancyIteration(range, maxOccupancy)) {
        return;
    }
    if (isEmptyCellRange(range)) {
    if (canSkipCellOccupancyInsert(range, params.maxCellOccupancy)) {
        return;
    }
    if (params.maxCellOccupancyPerShape > 0u) {
        range = shrinkCellRangeToOccupancyBudget(range, params.maxCellOccupancyPerShape);
        if (isEmptyCellRange(range)) {
            return;
        }
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
    const PairBufferSoA::DedupePreflight preflight = buffer.preflight_dedupe();
    if (!preflight.needs_dedupe()) {
    if (should_skip_dedupe_pair_buffer(buffer)) {
    if (canSkipDedupeBuffer(buffer)) {
    const BroadphaseDedupePreflight preflight = preflight_broadphase_dedupe(buffer);
    if (should_skip_pair_buffer_dedupe(buffer)) {
    const BroadphaseDedupePreflight preflight = preflight_dedupe_pairs(buffer);
    if (!preflight.can_dedupe()) {
    if (!shouldRunDedupeBroadphase(buffer)) {
    if (should_skip_dedupe_broadphase(buffer)) {
    if (canSkipDedupeBroadphase(buffer) || buffer.canSkipDedupePass()) {
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

CandidateRejectReason candidatePairRejectReasonImpl(
    const CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    const CandidateRejectReason indexReason = candidatePairRejectReason(pair, bodies.count());
    if (indexReason != CandidateRejectReason::None) {
        return indexReason;
    }

    const vec3 posA = bodies.positions[pair.bodyA];
    const vec3 posB = bodies.positions[pair.bodyB];
    const f32 radiusA = bodyShapeRadius(shapes, pair.bodyA);
    const f32 radiusB = bodyShapeRadius(shapes, pair.bodyB);
    if (!sphereAabbOverlap(posA, radiusA, posB, radiusB)) {
        return CandidateRejectReason::AabbSeparated;
    }
    return CandidateRejectReason::None;
}

bool pairPassesAabbRefine(
    u32 bodyA,
    u32 bodyB,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return candidatePairRejectReasonImpl({bodyA, bodyB}, bodies, shapes) == CandidateRejectReason::None;
    const CandidatePairRejectReason indexReason = candidatePairRejectReason(bodyA, bodyB, bodies.count());
    if (indexReason != CandidatePairRejectReason::None) {
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
    if (!preflightBroadphase(bodies, shapes).canRun()) {
    if (shouldSkipBroadphaseInput(bodies.count(), shapes.count())) {
    if (isEmptyBroadphaseInput(bodies, shapes)) {
    if (canSkipBroadphase(bodies.count(), shapes.count())) {
    if (canSkipBroadphase(bodies, shapes)) {
    const BroadphasePreflight preflight = preflight_broadphase(bodies, shapes);
    if (!preflight.can_run()) {
    if (should_skip_broadphase(bodies, shapes)) {
    const BroadphaseInputPreflight inputPreflight = preflight_broadphase_input(bodies, shapes);
    if (!inputPreflight.can_run()) {
    if (!preflight_broadphase_dispatch(bodies, shapes).can_dispatch()) {
    if (should_skip_broadphase_build(bodies, shapes)) {
    if (can_skip_broadphase_dispatch(bodies, shapes)) {
        return;
    }

    const SpatialHashParams normalizedParams = normalizeSpatialHashParams(params);
    const u32 tableSize = normalizedParams.tableSize;
    const SpatialHashParams safeParams = sanitizeSpatialHashParams(params);
    const u32 tableSize = clampTableSize(safeParams.tableSize);

    const u32 tableSize = safeParams.tableSize;
    const u32 bodyCount = bodies.count();
    CellBuckets cells(tableSize);

    const u32 shapeCount = shapes.count();
    fuse::jobs::parallel_for(0u, shapeCount, kBuildGrainSize, [&](u32 shapeIndex) {
        populateShapeCells(shapeIndex, bodies, shapes, normalizedParams, use2D, cells);
        populateShapeCells(shapeIndex, bodies, shapes, safeParams, use2D, cells);
    });

    std::vector<u32> cellSlotOffsets(tableSize, 0u);
    u32 totalCellSlots = 0u;
    for (u32 cellIndex = 0; cellIndex < tableSize; ++cellIndex) {
        cellSlotOffsets[cellIndex] = totalCellSlots;
        totalCellSlots += countPairsForCell(cells.buckets[cellIndex]);
    }

    buffer.preparePairSlots(totalCellSlots);
    if (!shouldRunBroadphaseCellPairGen(totalCellSlots)) {
    const PairSlotPreflight slotPreflight = preflightPairSlots(totalCellSlots, buffer);
    if (slotPreflight.skipped) {
        return;
    }

    buffer.preparePairSlots(totalCellSlots);

    fuse::jobs::parallel_for(0u, tableSize, kCellGrainSize, [&](u32 cellIndex) {
        if (cells.buckets[cellIndex].empty()) {
            return;
        }
        writePairsForCellSlots(cells.buckets[cellIndex], cellSlotOffsets[cellIndex], bodyCount, buffer);
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

    const BroadphaseMergePreflight mergePreflight = preflightBroadphaseMerge(bodies, shapes);
    if (shouldRunBroadphaseMerge(bodies, shapes)) {
    if (!canSkipBroadphaseMerge(bodies, shapes)) {
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

    if (shouldRunPairBufferClamp(buffer)) {
        buffer.applyMaxCapacityClamp();
    }
}

void refineBroadphasePairsParallelImpl(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    PairBufferSoA& buffer) {
    if (!shouldRunRefineBroadphase(bodies, shapes, buffer)) {
    if (buffer.canSkipRefine() || bodies.count() == 0 || shapes.count() == 0) {
    if (shouldSkipBroadphaseRefine(buffer.activeCount, buffer.pairSlotCount, bodies.count(), shapes.count())) {
    if (buffer.canSkipRefine() || !buffer.hasValidPairs() || canSkipBroadphase(bodies, shapes)) {
    if (canSkipBroadphaseRefine(bodies, shapes, buffer)) {
    const BroadphaseRefinePreflight preflight = preflight_broadphase_refine(bodies, shapes, buffer);
    if (!preflight.can_refine()) {
    if (should_skip_refine_broadphase(bodies, shapes, buffer)) {
    if (canSkipRefineBroadphasePairs(buffer, bodies, shapes)) {
    const BroadphaseRefinePreflight refinePreflight = preflight_broadphase_refine(buffer, bodies, shapes);
    if (!refinePreflight.can_refine()) {
    if (canSkipRefineBroadphase(bodies, shapes, buffer)) {
    if (should_skip_refine_broadphase_pairs(bodies, shapes, buffer)) {
    if (should_skip_broadphase_refine(bodies, shapes, buffer)) {
        return;
    }

    const u32 pairCount = buffer.activeCount;
    fuse::jobs::parallel_for(0u, pairCount, kPlanePairGrainSize, [&](u32 pairIndex) {
        if (pairIndex >= buffer.activeCount || !buffer.slotIsValid(pairIndex)) {
            return;
        }

        const u32 bodyA = buffer.bodyA[pairIndex];
        const u32 bodyB = buffer.bodyB[pairIndex];
        const CandidateRejectReason rejectReason =
            candidatePairRejectReasonImpl({bodyA, bodyB}, bodies, shapes);
        if (rejectReason != CandidateRejectReason::None) {
        const CandidatePairRejectReason rejectReason = [&]() {
            const CandidatePairRejectReason indexReason =
                candidatePairRejectReason(bodyA, bodyB, bodies.count());
            if (indexReason != CandidatePairRejectReason::None) {
                return indexReason;
            }
            if (!pairPassesAabbRefine(bodyA, bodyB, bodies, shapes)) {
                return CandidatePairRejectReason::AabbSeparated;
            return CandidatePairRejectReason::None;
        }();
        if (rejectReason != CandidatePairRejectReason::None) {
            buffer.lastRejectReason = rejectReason;
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

bool canSkipCellPairGeneration(const std::vector<u32>& occupants) {
    return !preflightCellPairGeneration(occupants).canGenerate();

bool shouldRunCellPairGeneration(const std::vector<u32>& occupants) {
    return preflightCellPairGeneration(occupants).canGenerate();

ShapeCellInsertRejectReason shapeCellInsertRejectReason(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {
    return shapeCellInsertRejectReasonImpl(shapeIndex, bodies, shapes, params, use2D);

ShapeCellInsertPreflight preflightShapeCellInsert(
    return preflightShapeCellInsertImpl(shapeIndex, bodies, shapes, params, use2D);

bool shapeCellInsertRejectsForReason(
    bool use2D,
    ShapeCellInsertRejectReason expected) {
    return shapeCellInsertRejectReason(shapeIndex, bodies, shapes, params, use2D) == expected;

bool canSkipShapeCellInsert(
    return !preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();

bool shouldRunShapeCellInsert(
    return preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();

BroadphaseCellPairGenRejectReason broadphaseCellPairGenRejectReason(u32 totalCellSlots) {
    if (totalCellSlots == 0u) {
        return BroadphaseCellPairGenRejectReason::EmptyCells;
    return BroadphaseCellPairGenRejectReason::None;

bool broadphaseCellPairGenRejectsForReason(u32 totalCellSlots, BroadphaseCellPairGenRejectReason expected) {
    return broadphaseCellPairGenRejectReason(totalCellSlots) == expected;

BroadphaseCellPairGenPreflight preflightBroadphaseCellPairGen(u32 totalCellSlots) {
    BroadphaseCellPairGenPreflight preflight{};
    preflight.reason = broadphaseCellPairGenRejectReason(totalCellSlots);
    preflight.emptyCells = preflight.reason == BroadphaseCellPairGenRejectReason::EmptyCells;
    return preflight;

bool canSkipBroadphaseCellPairGen(u32 totalCellSlots) {
    return !preflightBroadphaseCellPairGen(totalCellSlots).canGenerate();

bool shouldRunBroadphaseCellPairGen(u32 totalCellSlots) {
    return preflightBroadphaseCellPairGen(totalCellSlots).canGenerate();

RefineBroadphaseRejectReason refineBroadphaseRejectReason(
    const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return RefineBroadphaseRejectReason::EmptyBuffer;
    if (canSkipBroadphase(bodies, shapes)) {
        return RefineBroadphaseRejectReason::EmptyInput;
    if (!buffer.hasValidPairs()) {
        return RefineBroadphaseRejectReason::NoValidPairs;
    return RefineBroadphaseRejectReason::None;

bool refineBroadphaseRejectsForReason(
    const PairBufferSoA& buffer,
    RefineBroadphaseRejectReason expected) {
    return refineBroadphaseRejectReason(bodies, shapes, buffer) == expected;

RefineBroadphasePreflight preflightRefineBroadphase(
    RefineBroadphasePreflight preflight{};
    preflight.emptyBuffer = buffer.canSkipSoAIteration();
    preflight.noValidPairs = !buffer.hasValidPairs();
    preflight.emptyInput = canSkipBroadphase(bodies, shapes);
    preflight.reason = refineBroadphaseRejectReason(bodies, shapes, buffer);

bool canSkipRefineBroadphase(
    return !preflightRefineBroadphase(bodies, shapes, buffer).canRefine();

bool shouldRunRefineBroadphase(
    return preflightRefineBroadphase(bodies, shapes, buffer).canRefine();

bool wouldSkipRefineBroadphase(
    RefineBroadphaseRejectReason* reason) {
    const RefineBroadphaseRejectReason reject = refineBroadphaseRejectReason(bodies, shapes, buffer);
    if (reason != nullptr) {
        *reason = reject;
    return reject != RefineBroadphaseRejectReason::None;

bool shouldRunRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return preflightRefineBroadphase(bodies, shapes, buffer).canRefine();
}

bool shouldRunRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return !canSkipRefineBroadphase(bodies, shapes, buffer);
}

DedupeBroadphaseRejectReason dedupeBroadphaseRejectReason(const PairBufferSoA& buffer) {
        return DedupeBroadphaseRejectReason::EmptyBuffer;
    if (buffer.activeCount <= 1u) {
        return DedupeBroadphaseRejectReason::SinglePair;
    return DedupeBroadphaseRejectReason::None;

bool dedupeBroadphaseRejectsForReason(const PairBufferSoA& buffer, DedupeBroadphaseRejectReason expected) {
    return dedupeBroadphaseRejectReason(buffer) == expected;

DedupeBroadphasePreflight preflightDedupeBroadphase(const PairBufferSoA& buffer) {
    DedupeBroadphasePreflight preflight{};
    preflight.singlePair = !preflight.emptyBuffer && buffer.activeCount <= 1u;
    preflight.reason = dedupeBroadphaseRejectReason(buffer);

bool shouldRunDedupeBroadphase(const PairBufferSoA& buffer) {
    return preflightDedupeBroadphase(buffer).canDedupe();

bool canSkipDedupeBroadphase(const PairBufferSoA& buffer) {
    return !shouldRunDedupeBroadphase(buffer);

bool wouldSkipDedupeBroadphase(const PairBufferSoA& buffer, DedupeBroadphaseRejectReason* reason) {
    const DedupeBroadphaseRejectReason reject = dedupeBroadphaseRejectReason(buffer);
    return reject != DedupeBroadphaseRejectReason::None;

BroadphaseMergeRejectReason broadphaseMergeRejectReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    const BroadphaseMergePreflight preflight = preflightBroadphaseMerge(bodies, shapes);
    return preflight.reason;
}

bool broadphaseMergeRejectsForReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    BroadphaseMergeRejectReason expected) {
    return broadphaseMergeRejectReason(bodies, shapes) == expected;
}

BroadphaseMergePreflight preflightBroadphaseMerge(
    const CollisionShapeSoA& shapes) {
    BroadphaseMergePreflight preflight{};
namespace {

struct BroadphaseMergeScan {
    bool hasPlaneBodies = false;
    bool hasDynamicBodies = false;
};

BroadphaseMergeScan scanBroadphaseMergeBodies(
    const RigidBodySoA& bodies,
    BroadphaseMergeScan scan{};
    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        const u32 bodyIndex = shapes.bodyIndices[shapeIndex];
        if (bodyIndex >= bodies.count()) {
            continue;
        const CollisionShapeType type = static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
        if (type == CollisionShapeType::Plane) {
            scan.hasPlaneBodies = true;
        } else if ((bodies.flags[bodyIndex] & RB_STATIC) == 0) {
            hasDynamicBodies = true;
        if (hasPlaneBodies && hasDynamicBodies) {
            break;

    preflight.emptyPlaneBodies = !hasPlaneBodies;
    preflight.emptyDynamicBodies = !hasDynamicBodies;
    if (preflight.emptyPlaneBodies) {
        preflight.reason = BroadphaseMergeRejectReason::EmptyPlaneBodies;
    } else if (preflight.emptyDynamicBodies) {
        preflight.reason = BroadphaseMergeRejectReason::EmptyDynamicBodies;
    } else {
        preflight.reason = BroadphaseMergeRejectReason::None;

BroadphaseMergeRejectReason mergeBroadphaseRejectReason(
    return preflightBroadphaseMerge(bodies, shapes).reason;

bool mergeBroadphaseRejectsForReason(
    BroadphaseMergeRejectReason expected) {
    return mergeBroadphaseRejectReason(bodies, shapes) == expected;

bool canSkipBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    return !preflightBroadphaseMerge(bodies, shapes).canMerge();

bool shouldRunBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    return preflightBroadphaseMerge(bodies, shapes).canMerge();

bool wouldSkipBroadphaseMerge(
    BroadphaseMergeRejectReason* reason) {
    const BroadphaseMergeRejectReason reject = mergeBroadphaseRejectReason(bodies, shapes);
    return reject != BroadphaseMergeRejectReason::None;

const char* mergePairsIntoBufferRejectReasonName(MergePairsIntoBufferRejectReason reason) {
    switch (reason) {
    case MergePairsIntoBufferRejectReason::None:
        return "None";
    case MergePairsIntoBufferRejectReason::EmptyPairs:
        return "EmptyPairs";
    case MergePairsIntoBufferRejectReason::BufferFull:
        return "BufferFull";
    return "Unknown";

MergePairsIntoBufferRejectReason mergePairsIntoBufferRejectReason(
    const std::vector<CandidatePair>& pairs,
    if (pairs.empty()) {
        return MergePairsIntoBufferRejectReason::EmptyPairs;
    if (buffer.isFull()) {
        return MergePairsIntoBufferRejectReason::BufferFull;
    return MergePairsIntoBufferRejectReason::None;

bool mergePairsIntoBufferRejectsForReason(
    MergePairsIntoBufferRejectReason expected) {
    return mergePairsIntoBufferRejectReason(pairs, buffer) == expected;

MergePairsIntoBufferPreflight preflightMergePairsIntoBuffer(
    MergePairsIntoBufferPreflight preflight{};
    preflight.reason = mergePairsIntoBufferRejectReason(pairs, buffer);
    preflight.emptyPairs = preflight.reason == MergePairsIntoBufferRejectReason::EmptyPairs;
    preflight.bufferFull = preflight.reason == MergePairsIntoBufferRejectReason::BufferFull;

bool canSkipMergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, const PairBufferSoA& buffer) {
    return !preflightMergePairsIntoBuffer(pairs, buffer).canMerge();

bool shouldRunMergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, const PairBufferSoA& buffer) {
    return preflightMergePairsIntoBuffer(pairs, buffer).canMerge();

bool wouldSkipMergePairsIntoBuffer(
    MergePairsIntoBufferRejectReason* reason) {
    const MergePairsIntoBufferRejectReason reject = mergePairsIntoBufferRejectReason(pairs, buffer);
    return reject != MergePairsIntoBufferRejectReason::None;

f32 bodyShapeRadiusForReject(const CollisionShapeSoA& shapes, u32 bodyIndex) {
        if (shapes.bodyIndices[shapeIndex] == bodyIndex) {
            return shapes.params[shapeIndex].x;
    return 0.5f;

} // namespace

CandidateRejectReason candidatePairRejectReason(
    const CandidatePair& pair,
    const CandidateRejectReason indexReason = candidatePairRejectReason(pair, bodies.count());
    if (indexReason != CandidateRejectReason::None) {
        return indexReason;

    const vec3 posA = bodies.positions[pair.bodyA];
    const vec3 posB = bodies.positions[pair.bodyB];
    const f32 radiusA = bodyShapeRadiusForReject(shapes, pair.bodyA);
    const f32 radiusB = bodyShapeRadiusForReject(shapes, pair.bodyB);
    if (!sphereAabbOverlap(posA, radiusA, posB, radiusB)) {
        return CandidateRejectReason::AabbSeparated;
    return CandidateRejectReason::None;
CandidatePairRejectReason candidatePairRejectReason(
    const CandidatePairRejectReason indexReason = candidatePairRejectReason(pair, bodies.count());
    if (indexReason != CandidatePairRejectReason::None) {

    f32 radiusA = 0.5f;
    f32 radiusB = 0.5f;
        if (shapes.bodyIndices[shapeIndex] == pair.bodyA) {
            radiusA = shapes.params[shapeIndex].x;
        if (shapes.bodyIndices[shapeIndex] == pair.bodyB) {
            radiusB = shapes.params[shapeIndex].x;
        return CandidatePairRejectReason::AabbSeparated;

    return CandidatePairRejectReason::None;
    return preflight;
}

    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {

    preflight.emptyBuffer = buffer.canSkipSoAIteration();

    preflight.reason = broadphaseMergeRejectReason(bodies, shapes);

const char* broadphaseMergeRejectReasonName(BroadphaseMergeRejectReason reason) {
    case BroadphaseMergeRejectReason::None:
    case BroadphaseMergeRejectReason::EmptyPlaneBodies:
        return "EmptyPlaneBodies";
    case BroadphaseMergeRejectReason::EmptyDynamicBodies:
        return "EmptyDynamicBodies";

BroadphaseMergeRejectReason broadphaseMergeRejectReason(
    bool hasPlaneBodies = false;
    bool hasDynamicBodies = false;

    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        const u32 bodyIndex = shapes.bodyIndices[shapeIndex];
        if (bodyIndex >= bodies.count()) {
            continue;
        const CollisionShapeType type = static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
        if (type == CollisionShapeType::Plane) {
            hasPlaneBodies = true;
        } else if ((bodies.flags[bodyIndex] & RB_STATIC) == 0) {
            return BroadphaseMergeRejectReason::None;

    if (!hasPlaneBodies) {
        return BroadphaseMergeRejectReason::EmptyPlaneBodies;
    return BroadphaseMergeRejectReason::EmptyDynamicBodies;

bool broadphaseMergeRejectsForReason(
    return broadphaseMergeRejectReason(bodies, shapes) == expected;

bool canSkipBroadphaseMerge(

bool shouldRunBroadphaseMerge(

            scan.hasDynamicBodies = true;
        if (scan.hasPlaneBodies && scan.hasDynamicBodies) {
    return scan;


    const BroadphaseMergeScan scan = scanBroadphaseMergeBodies(bodies, shapes);
    if (!scan.hasPlaneBodies) {
    if (!scan.hasDynamicBodies) {


BroadphaseMergePreflight preflightBroadphaseMerge(
    BroadphaseMergePreflight preflight{};
    preflight.emptyPlaneBodies = !scan.hasPlaneBodies;
    preflight.emptyDynamicBodies = !scan.hasDynamicBodies;


    return !shouldRunBroadphaseMerge(bodies, shapes);
    }
    if (!hasDynamicBodies) {
    return BroadphaseMergeRejectReason::None;

    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    BroadphaseMergeRejectReason expected) {

    const CollisionShapeSoA& shapes) {
    preflight.reason = broadphaseMergeRejectReason(bodies, shapes);

    bool hasPlaneBodies = false;
    bool hasDynamicBodies = false;
    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        const u32 bodyIndex = shapes.bodyIndices[shapeIndex];
        if (bodyIndex >= bodies.count()) {
            continue;
        const CollisionShapeType type = static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
        if (type == CollisionShapeType::Plane) {
            hasPlaneBodies = true;
        } else if ((bodies.flags[bodyIndex] & RB_STATIC) == 0) {
            hasDynamicBodies = true;

    preflight.emptyPlaneBodies = !hasPlaneBodies;
    preflight.emptyDynamicBodies = !hasDynamicBodies;
    return preflight;
}

bool canSkipBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    return !preflightBroadphaseMerge(bodies, shapes).canMerge();
}

bool shouldRunBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    return preflightBroadphaseMerge(bodies, shapes).canMerge();
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

} // namespace fuse::physics::broadphase
