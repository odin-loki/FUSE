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

bool shouldRunCellPairGeneration(const std::vector<u32>& occupants);
u32 countPairsForCell(const std::vector<u32>& occupants);
ShapeCellInsertPreflight preflightShapeCellInsert(
    u32 bodyIndex,
    u32 bodyCount,
    const CellRange3& range,
    u32 maxOccupancy);
    const CellRange2& range,

namespace {

u32 uniqueOccupantCountForCellPairGen(const std::vector<u32>& occupants) {
    if (occupants.size() < 2u) {
        return static_cast<u32>(occupants.size());
    std::vector<u32> uniqueBodies = occupants;
    std::sort(uniqueBodies.begin(), uniqueBodies.end());
    uniqueBodies.erase(std::unique(uniqueBodies.begin(), uniqueBodies.end()), uniqueBodies.end());
    return static_cast<u32>(uniqueBodies.size());

} // namespace

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

const char* cellSpanRejectReasonName(CellSpanRejectReason reason) {
    switch (reason) {
    case CellSpanRejectReason::None:
        return "None";
    case CellSpanRejectReason::EmptyRange:
        return "EmptyRange";
    case CellSpanRejectReason::ExceedsSpanClamp:
        return "ExceedsSpanClamp";
    }
    return "Unknown";
}

const char* cellSpanRejectReasonName(CellSpanRejectReason reason) {
    switch (reason) {
    case CellSpanRejectReason::None:
        return "None";
    case CellSpanRejectReason::EmptyRange:
        return "EmptyRange";
    case CellSpanRejectReason::ExceedsSpanClamp:
        return "ExceedsSpanClamp";
    }
    return "Unknown";
}

const char* cellSpanClampRejectReasonName(CellSpanClampRejectReason reason) {
    switch (reason) {
    case CellSpanClampRejectReason::None:
        return "None";
    case CellSpanClampRejectReason::EmptyRange:
        return "EmptyRange";
    case CellSpanClampRejectReason::ExceedsSpanPerAxis:
        return "ExceedsSpanPerAxis";
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

const char* cellSpanRejectReasonName(CellSpanRejectReason reason) {
    switch (reason) {
    case CellSpanRejectReason::None:
        return "None";
    case CellSpanRejectReason::EmptyRange:
        return "EmptyRange";
    case CellSpanRejectReason::ExceedsMaxSpan:
        return "ExceedsMaxSpan";
const char* cellSpanClampRejectReasonName(CellSpanClampRejectReason reason) {
    case CellSpanClampRejectReason::None:
    case CellSpanClampRejectReason::ExceedsSpanPerAxis:
        return "ExceedsSpanPerAxis";
    case CellSpanRejectReason::ExceedsSpanLimit:
        return "ExceedsSpanLimit";
    }
    return "Unknown";

const char* shapeCellInsertRejectReasonName(ShapeCellInsertRejectReason reason) {
    case ShapeCellInsertRejectReason::None:
    case ShapeCellInsertRejectReason::EmptyRange:
    case ShapeCellInsertRejectReason::ExceedsBudget:
        return "ExceedsBudget";

const char* cellPairGenRejectReasonName(CellPairGenRejectReason reason) {
    case CellPairGenRejectReason::None:

    case CellPairGenRejectReason::EmptyOccupants:
        return "EmptyOccupants";
    case CellPairGenRejectReason::SingleOccupant:
        return "SingleOccupant";

const char* cellCapacityInsertRejectReasonName(CellCapacityInsertRejectReason reason) {
    case CellCapacityInsertRejectReason::None:
    case CellCapacityInsertRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    case CellCapacityInsertRejectReason::ExceedsOccupancy:
        return "ExceedsOccupancy";
    case CellPairGenRejectReason::SingletonOccupant:
        return "SingletonOccupant";
    case CellCapacityInsertRejectReason::EmptyRange:
    case CellCapacityInsertRejectReason::ExceedsBudget:

    case CellPairGenRejectReason::TooFewOccupants:
        return "TooFewOccupants";
    case CellPairGenRejectReason::SingleUniqueBody:
        return "SingleUniqueBody";

    case ShapeCellInsertRejectReason::OutOfRangeBody:
    case ShapeCellInsertRejectReason::OccupancyRejected:
        return "OccupancyRejected";

u32 countUniqueCellOccupants(const std::vector<u32>& occupants) {
    if (occupants.empty()) {
        return 0u;
    std::vector<u32> uniqueBodies = occupants;
    std::sort(uniqueBodies.begin(), uniqueBodies.end());
    uniqueBodies.erase(std::unique(uniqueBodies.begin(), uniqueBodies.end()), uniqueBodies.end());
    return static_cast<u32>(uniqueBodies.size());

u32 countPairsForCellOccupants(const std::vector<u32>& occupants) {
    if (cellPairGenRejectReason(occupants) != CellPairGenRejectReason::None) {
    return estimatePairCountForUniqueBodies(countUniqueCellOccupants(occupants));

CellPairGenRejectReason cellPairGenRejectReason(const std::vector<u32>& occupants) {
    if (occupants.size() < 2u) {
        return CellPairGenRejectReason::TooFewOccupants;
    if (countUniqueCellOccupants(occupants) < 2u) {
        return CellPairGenRejectReason::SingleUniqueBody;
    return CellPairGenRejectReason::None;

bool cellPairGenRejectsForReason(const std::vector<u32>& occupants, CellPairGenRejectReason expected) {
    return cellPairGenRejectReason(occupants) == expected;

CellPairGenPreflight preflightCellPairGeneration(const std::vector<u32>& occupants) {
    CellPairGenPreflight preflight{};
    preflight.reason = cellPairGenRejectReason(occupants);
    preflight.tooFewOccupants = preflight.reason == CellPairGenRejectReason::TooFewOccupants;
    preflight.singleUniqueBody = preflight.reason == CellPairGenRejectReason::SingleUniqueBody;
    preflight.uniqueBodyCount = countUniqueCellOccupants(occupants);
    preflight.pairCount = preflight.canGenerate()
        ? estimatePairCountForUniqueBodies(preflight.uniqueBodyCount)
        : 0u;
    return preflight;

bool canSkipCellPairGeneration(const std::vector<u32>& occupants) {
    return !preflightCellPairGeneration(occupants).canGenerate();

bool shouldRunCellPairGeneration(const std::vector<u32>& occupants) {
    return preflightCellPairGeneration(occupants).canGenerate();

ShapeCellInsertRejectReason shapeCellInsertRejectReason(
    u32 bodyIndex,
    u32 bodyCount,
    const CellRange3& range,
    u32 maxOccupancy) {
    if (bodyCount > 0u && bodyIndex >= bodyCount) {
        return ShapeCellInsertRejectReason::OutOfRangeBody;
    if (canSkipCellOccupancyIteration(range, maxOccupancy)) {
        return ShapeCellInsertRejectReason::OccupancyRejected;
    return ShapeCellInsertRejectReason::None;

    const CellRange2& range,

bool shapeCellInsertRejectsForReason(
    u32 maxOccupancy,
    ShapeCellInsertRejectReason expected) {
    return shapeCellInsertRejectReason(bodyIndex, bodyCount, range, maxOccupancy) == expected;


ShapeCellInsertPreflight preflightShapeCellInsert(
    ShapeCellInsertPreflight preflight{};
    preflight.reason = shapeCellInsertRejectReason(bodyIndex, bodyCount, range, maxOccupancy);
    preflight.outOfRangeBody = preflight.reason == ShapeCellInsertRejectReason::OutOfRangeBody;
    preflight.occupancyRejected = preflight.reason == ShapeCellInsertRejectReason::OccupancyRejected;


bool canSkipShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange3& range, u32 maxOccupancy) {
    return !preflightShapeCellInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert();

bool canSkipShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange2& range, u32 maxOccupancy) {

bool shouldRunShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange3& range, u32 maxOccupancy) {
    return preflightShapeCellInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert();

bool shouldRunShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange2& range, u32 maxOccupancy) {

    switch (reason) {
        return "None";
        return "EmptyRange";

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
    case RefineBroadphaseRejectReason::AllSlotsInvalid:
        return "AllSlotsInvalid";
    }
    return "Unknown";

const char* dedupeBroadphaseRejectReasonName(DedupeBroadphaseRejectReason reason) {
    switch (reason) {
    case DedupeBroadphaseRejectReason::None:
        return "None";
    case DedupeBroadphaseRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case DedupeBroadphaseRejectReason::SinglePair:
        return "SinglePair";
    case DedupeBroadphaseRejectReason::AlreadyUnique:
        return "AlreadyUnique";

const char* cellSpanClampRejectReasonName(CellSpanClampRejectReason reason) {
    switch (reason) {
    case CellSpanClampRejectReason::None:
        return "None";
    case CellSpanClampRejectReason::EmptyRange:
        return "EmptyRange";
    case CellSpanClampRejectReason::WithinSpan:
        return "WithinSpan";
    case CellSpanClampRejectReason::UnlimitedSpan:
        return "UnlimitedSpan";
    }
    return "Unknown";
}

const char* broadphasePairSlotRejectReasonName(BroadphasePairSlotRejectReason reason) {
    switch (reason) {
    case BroadphasePairSlotRejectReason::None:
        return "None";
    case BroadphasePairSlotRejectReason::ZeroPairSlots:
        return "ZeroPairSlots";
    }
    return "Unknown";
}

const char* cellRangeSpanClampRejectReasonName(CellRangeSpanClampRejectReason reason) {
    switch (reason) {
    case CellRangeSpanClampRejectReason::None:
        return "None";
    case CellRangeSpanClampRejectReason::EmptyRange:
        return "EmptyRange";
    case CellRangeSpanClampRejectReason::UnlimitedSpan:
        return "UnlimitedSpan";
    }
    return "Unknown";
}

const char* broadphaseCellPairBuildRejectReasonName(BroadphaseCellPairBuildRejectReason reason) {
    switch (reason) {
    case BroadphaseCellPairBuildRejectReason::None:
        return "None";
    case BroadphaseCellPairBuildRejectReason::NoCellSlots:
        return "NoCellSlots";
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

const char* cellPairGenRejectReasonName(CellPairGenRejectReason reason) {
    switch (reason) {
    case CellPairGenRejectReason::None:
        return "None";
    case CellPairGenRejectReason::EmptyOccupants:
        return "EmptyOccupants";
    case CellPairGenRejectReason::SingletonOccupant:
        return "SingletonOccupant";
    }
    return "Unknown";
}

const char* cellCapacityInsertRejectReasonName(CellCapacityInsertRejectReason reason) {
    switch (reason) {
    case CellCapacityInsertRejectReason::None:
        return "None";
    case CellCapacityInsertRejectReason::EmptyRange:
        return "EmptyRange";
    case CellCapacityInsertRejectReason::ExceedsBudget:
        return "ExceedsBudget";
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

const char* broadphaseMergeRejectReasonName(BroadphaseMergeRejectReason reason) {
    switch (reason) {
    case BroadphaseMergeRejectReason::None:
        return "None";
    case BroadphaseMergeRejectReason::NoPlaneBodies:
        return "NoPlaneBodies";
    case BroadphaseMergeRejectReason::NoDynamicBodies:
        return "NoDynamicBodies";
const char* cellSpanRejectReasonName(CellSpanRejectReason reason) {
    case CellSpanRejectReason::None:
    case CellSpanRejectReason::EmptyRange:
        return "EmptyRange";
    case CellSpanRejectReason::Unbounded:
        return "Unbounded";
    case CellSpanRejectReason::WithinSpanLimit:
        return "WithinSpanLimit";
const char* cellSpanClampRejectReasonName(CellSpanClampRejectReason reason) {
    case CellSpanClampRejectReason::None:
    case CellSpanClampRejectReason::EmptyRange:
    case CellSpanClampRejectReason::WithinSpanLimit:
    case CellSpanClampRejectReason::UnlimitedSpan:
        return "UnlimitedSpan";
    case CellSpanClampRejectReason::UnboundedSpan:
        return "UnboundedSpan";
    case CellSpanRejectReason::ExceedsSpan:
        return "ExceedsSpan";
const char* cellPairGenRejectReasonName(CellPairGenRejectReason reason) {
    case CellPairGenRejectReason::None:
    case CellPairGenRejectReason::EmptyCell:
        return "EmptyCell";
    case CellPairGenRejectReason::InsufficientOccupants:
        return "InsufficientOccupants";
    }
    return "Unknown";

    case BroadphaseMergeRejectReason::EmptyPlaneBodies:
        return "EmptyPlaneBodies";
    case BroadphaseMergeRejectReason::EmptyDynamicBodies:
        return "EmptyDynamicBodies";
const char* mergeBroadphaseBufferRejectReasonName(BroadphaseMergeBufferRejectReason reason) {
    case BroadphaseMergeBufferRejectReason::None:
    case BroadphaseMergeBufferRejectReason::SceneNotMergeable:
        return "SceneNotMergeable";
    case BroadphaseMergeBufferRejectReason::SceneRejected:
        return "SceneRejected";
    case BroadphaseMergeBufferRejectReason::BufferAtCapacity:
        return "BufferAtCapacity";



const char* dedupeBroadphaseRejectReasonName(DedupeBroadphaseRejectReason reason) {
    case DedupeBroadphaseRejectReason::None:
    case DedupeBroadphaseRejectReason::EmptyBuffer:
    case DedupeBroadphaseRejectReason::SinglePair:
        return "SinglePair";



const char* mergeBroadphaseRejectReasonName(MergeBroadphaseRejectReason reason) {
    case MergeBroadphaseRejectReason::None:
    case MergeBroadphaseRejectReason::EmptyPlaneBodies:
    case MergeBroadphaseRejectReason::EmptyDynamicBodies:

CellOccupancyPreflight preflightCellOccupancy(const CellRange3& range, u32 maxCells) {
    CellOccupancyPreflight preflight{};
    preflight.reason = cellOccupancyRejectReason(range, maxCells);
    preflight.emptyRange = preflight.reason == CellOccupancyRejectReason::EmptyRange;
    preflight.occupancyCount = estimateCellOccupancyCount(range);
    preflight.exceedsBudget = preflight.reason == CellOccupancyRejectReason::ExceedsBudget;
    return preflight;

CellOccupancyPreflight preflightCellOccupancy(const CellRange2& range, u32 maxCells) {
const char* mergeIntoBufferBroadphaseRejectReasonName(BroadphaseMergeIntoBufferRejectReason reason) {
    case BroadphaseMergeIntoBufferRejectReason::None:
    case BroadphaseMergeIntoBufferRejectReason::SceneNotMergeable:
    case BroadphaseMergeIntoBufferRejectReason::BufferFull:
        return "BufferFull";
const char* shapeCellInsertRejectReasonName(ShapeCellInsertRejectReason reason) {
    case ShapeCellInsertRejectReason::None:
    case ShapeCellInsertRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    case ShapeCellInsertRejectReason::ExceedsOccupancyBudget:
        return "ExceedsOccupancyBudget";

namespace {

constexpr u32 kBuildGrainSize = 8u;
constexpr u32 kCellGrainSize = 4u;
constexpr u32 kPlanePairGrainSize = 16u;
namespace detail {

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

} // namespace detail

namespace {

constexpr u32 kBuildGrainSize = 8u;
constexpr u32 kCellGrainSize = 4u;
constexpr u32 kPlanePairGrainSize = 16u;

using detail::shapeBodyIndex;
using detail::shapeRadius;
using detail::shapeType;

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
    if (canSkipCellPairGen(occupants.size())) {
        return 0u;
    }
    return static_cast<u32>(uniqueOccupants(occupants).size());

CellPairGenPreflight preflightCellPairGenerationImpl(const std::vector<u32>& occupants) {
    CellPairGenPreflight preflight{};
    preflight.uniqueOccupantCount = countUniqueCellOccupantsImpl(occupants);
    preflight.reason = cellPairGenRejectReason(preflight.uniqueOccupantCount);
    preflight.emptyCell = preflight.reason == CellPairGenRejectReason::EmptyCell;
    preflight.singletonOccupant = preflight.reason == CellPairGenRejectReason::SingletonOccupant;
    preflight.pairCount = estimateCellPairCount(preflight.uniqueOccupantCount);
    return preflight;

    const CellPairGenPreflight preflight = preflightCellPairGenerationImpl(occupants);
    if (!preflight.canGenerate()) {
    return preflight.pairCount;
    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    const u32 bodyCount = static_cast<u32>(uniqueBodies.size());
    return preflightCellPairGen(bodyCount).pairCount;
}

u32 countPairsForCell(const std::vector<u32>& occupants) {
        return 0u;
}

void generatePairsForCell(const std::vector<u32>& occupants, std::vector<CandidatePair>& out) {
    if (!preflightCellPairGenerationImpl(occupants).canGenerate()) {
    return estimateCellPairCount(occupants);

    if (!shouldRunCellPairGeneration(occupants)) {
    const fuse::physics::broadphase::CellPairGenPreflight preflight =
        fuse::physics::broadphase::preflightCellPairGen(occupants);


    if (!fuse::physics::broadphase::shouldRunCellPairGen(occupants)) {
u32 cellPairCountForOccupants(const std::vector<u32>& occupants) {
    const u32 uniqueCount = static_cast<u32>(uniqueOccupants(occupants).size());
    return estimatePairCountForUniqueBodies(uniqueCount);

    return cellPairCountForOccupants(occupants);

u32 countPairsForOccupantsInternal(const std::vector<u32>& occupants) {
    if (occupants.size() < 2u) {
    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    return estimatePairCountForUniqueBodies(static_cast<u32>(uniqueBodies.size()));

    return countPairsForOccupantsInternal(occupants);

    if (!shouldRunCellPairGen(occupants)) {

    if (canSkipCellPairGeneration(occupants)) {
    return countPairsForOccupants(occupants);



    if (!shouldRunCellPairGeneration(static_cast<u32>(occupants.size()))) {
    return countCellPairSlots(occupants);

    return estimatePairCountForCell(occupants);

    if (!shouldRunCellPairGen(occupants.size())) {

    const u32 occupantCount = static_cast<u32>(occupants.size());
    const u32 uniqueCount = static_cast<u32>(uniqueBodies.size());
    if (!shouldRunCellPairGeneration(occupantCount, uniqueCount)) {
    return estimateCellPairCount(uniqueCount);



    return countPairsForCellOccupants(occupants);





    const CellPairGenPreflight preflight = preflightCellPairGen(occupants);

        return;
    }
    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    if (!shouldRunCellPairGeneration(static_cast<u32>(uniqueBodies.size()))) {
        return;
    }
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
    if (!shouldRunCellPairGeneration(occupants)) {
    if (!fuse::physics::broadphase::shouldRunCellPairGen(occupants)) {
    if (!shouldRunCellPairGen(occupants)) {
    if (canSkipCellPairGeneration(occupants)) {
    if (!shouldRunCellPairGeneration(static_cast<u32>(occupants.size()))) {
    if (!shouldRunCellPairGen(occupants.size())) {
    const u32 occupantCount = static_cast<u32>(occupants.size());
    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    const u32 uniqueCount = static_cast<u32>(uniqueBodies.size());
    if (!shouldRunCellPairGeneration(occupantCount, uniqueCount)) {
        return;
    }
    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    if (!shouldRunCellPairGeneration(static_cast<u32>(uniqueBodies.size()))) {
        return;
    }
    u32 slot = slotStart;
    for (usize i = 0; i < uniqueBodies.size(); ++i) {
        for (usize j = i + 1; j < uniqueBodies.size(); ++j) {
            buffer.writeSlot(slot++, uniqueBodies[i], uniqueBodies[j], bodyCount);
            if (!shouldRunPairBufferWriteSlot(buffer, slot, uniqueBodies[i], uniqueBodies[j])) {
            if (!shouldRunPairBufferWrite(buffer, slot, uniqueBodies[i], uniqueBodies[j])) {
                ++slot;
                continue;
            }
            buffer.writeSlot(slot++, uniqueBodies[i], uniqueBodies[j]);
            if (shouldRunPairBufferWriteSlot(buffer, slot, uniqueBodies[i], uniqueBodies[j])) {
            } else {
            if (fuse::physics::broadphase::shouldRunPairBufferWriteSlot(
                    buffer, slot, uniqueBodies[i], uniqueBodies[j])) {
                buffer.writeSlot(slot, uniqueBodies[i], uniqueBodies[j]);
        }
    }
}

ShapeCellInsertRejectReason shapeCellInsertRejectReasonImpl(
CellRange3 shapeCellRange3(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params) {
    const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
    if (bodyIndex >= bodies.count()) {
        return {};
bool shapeCellRangeForInsert(
    const SpatialHashParams& params,
    bool use2D,
    CellRange3& range3,
    CellRange2& range2) {
    if (shapeIndex >= shapes.count()) {
        return false;
    }


    const vec3 position = bodies.positions[bodyIndex];
    const f32 cellSize = clampCellSize(params.cellSize);
    const u32 maxSpan = params.maxCellSpanPerAxis;
    const CollisionShapeType type = shapeType(shapes, shapeIndex);

    if (use2D) {
        if (type == CollisionShapeType::Box) {
            const vec3 halfExtents = shapes.params[shapeIndex];
            const aabb bounds = aabbFromBox(position, halfExtents);
            range2 = cellRangeFromAabb2D(bounds, cellSize, maxSpan);
        } else {
            const f32 radius = shapeRadius(shapes, shapeIndex);
            range2 = cellRangeFromSphere2D({position.x, position.y}, radius, cellSize, maxSpan);
        return true;

        range3 = cellRangeFromBox(position, halfExtents, cellSize, maxSpan);
        range3 = cellRangeFromSphere(position, radius, cellSize, maxSpan);

void populateShapeCells(
    CellBuckets& cells) {
    if (!shouldRunShapeCellInsert(shapeIndex, bodies, shapes, params, use2D)) {
        return;
    }

    const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
    const f32 cellSize = clampCellSize(params.cellSize);
    const u32 maxSpan = params.maxCellSpanPerAxis;
    const CollisionShapeType type = shapeType(shapes, shapeIndex);

    if (type == CollisionShapeType::Box) {
        const vec3 halfExtents = shapes.params[shapeIndex];
        return cellRangeFromBox(position, halfExtents, cellSize, maxSpan);

    const f32 radius = shapeRadius(shapes, shapeIndex);
    return cellRangeFromSphere(position, radius, cellSize, maxSpan);

CellRange2 shapeCellRange2(


        const aabb bounds = aabbFromBox(position, halfExtents);
        return cellRangeFromAabb2D(bounds, cellSize, maxSpan);

    return cellRangeFromSphere2D({position.x, position.y}, radius, cellSize, maxSpan);

void populateShapeCells(
ShapeCellInsertPreflight preflightShapeCellInsertImpl(
    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {
    const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
    if (bodyIndex >= bodies.count()) {
        return ShapeCellInsertRejectReason::OutOfRangeBody;
    bool use2D,
    CellBuckets& cells) {
    if (!shouldRunBroadphaseShapeInsert(shapeIndex, bodies, shapes, params, use2D)) {
    if (!shouldRunShapeCellInsert(shapeIndex, bodies, shapes, params, use2D)) {
    const u32 bodyCount = bodies.count();
    if (bodyCount > 0u && bodyIndex >= bodyCount) {
    if (bodyIndex >= bodyCount) {
    if (!shouldRunCellCapacityInsert(shapeIndex, bodies, shapes, params, use2D)) {
        return;
    ShapeCellInsertPreflight preflight{};
        preflight.reason = ShapeCellInsertRejectReason::OutOfRangeBody;
        preflight.outOfRangeBody = true;
        return preflight;
    }


    const vec3 position = bodies.positions[bodyIndex];
    const f32 cellSize = clampCellSize(params.cellSize);
    const u32 maxOccupancy = params.maxCellOccupancy;
    const u32 maxOccupancy = params.maxCellOccupancyCount;
    const u32 occupancyBudget = use2D ? cellOccupancyBudgetFromSpan2D(maxSpan) : cellOccupancyBudgetFromSpan(maxSpan);
    const u32 occupancyBudget = estimateMaxCellOccupancyBudget(maxSpan, !use2D);
    const u32 tableSize = clampTableSize(params.tableSize);
    const u32 maxSpan = params.maxCellSpanPerAxis;
    const CollisionShapeType type = shapeType(shapes, shapeIndex);

    if (use2D) {
        CellRange2 range = {};
            range = cellRangeFromAabb2D(bounds, cellSize, maxSpan);
        } else {
            range = cellRangeFromSphere2D({position.x, position.y}, radius, cellSize, maxSpan);
        if (canSkipCellOccupancyIteration(range, maxOccupancy)) {
            return ShapeCellInsertRejectReason::OccupancySkipped;
        if (!shouldRunCellOccupancyIteration(range, maxOccupancy)) {
        if (canSkipShapeCellInsert(range, maxOccupancy)) {
        if (canSkipCellShapeInsert(bodyIndex, bodyCount, range, maxOccupancy)) {
        if (canSkipCellCapacityInsert(range, maxOccupancy, bodyIndex, bodyCount)) {
        if (!shouldRunShapeCellInsert(bodyIndex, bodyCount, range, maxOccupancy)) {
        if (canSkipCellCapacityInsert(bodyIndex, bodies.count(), range, maxOccupancy)) {
        if (!shouldRunCellCapacityInsert(bodyIndex, bodies.count(), range, maxOccupancy)) {
        if (!shouldRunCellCapacityInsert(bodyIndex, bodyCount, range, maxOccupancy)) {
        if (canSkipCellCapacityInsert(range, maxOccupancy)) {
        if (!shouldRunCellCapacityInsert(range, maxOccupancy)) {
        if (!preflightShapeCellInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert()) {
        if (canSkipShapeCellInsert(bodyIndex, bodies.count(), range, maxOccupancy)) {
        if (canSkipShapeCellOccupancyIteration(range, params)) {
        return ShapeCellInsertRejectReason::None;

    CellRange3 range = {};
        range = cellRangeFromBox(position, halfExtents, cellSize, maxSpan);
        range = cellRangeFromSphere(position, radius, cellSize, maxSpan);
        const CellOccupancyPreflight occupancyPreflight = preflightCellOccupancy(range, maxOccupancy);
        preflight.occupancyCount = occupancyPreflight.occupancyCount;
        if (occupancyPreflight.emptyRange) {
            preflight.reason = ShapeCellInsertRejectReason::EmptyRange;
            preflight.emptyRange = true;
        } else if (occupancyPreflight.exceedsBudget) {
            preflight.reason = ShapeCellInsertRejectReason::ExceedsBudget;
            preflight.exceedsBudget = true;



    const u32 tableSize = clampTableSize(params.tableSize);



    preflight.reason = shapeCellInsertRejectReasonImpl(shapeIndex, bodies, shapes, params, use2D);
    preflight.outOfRangeBody = preflight.reason == ShapeCellInsertRejectReason::OutOfRangeBody;
    preflight.occupancySkipped = preflight.reason == ShapeCellInsertRejectReason::OccupancySkipped;

    if (preflight.reason != ShapeCellInsertRejectReason::OutOfRangeBody) {
            preflight.occupancyCount = estimateCellOccupancyCount(range);


    const ShapeCellInsertPreflight insertPreflight =
        preflightShapeCellInsertImpl(shapeIndex, bodies, shapes, params, use2D);
    if (!insertPreflight.canInsert()) {







































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
        if (canSkipCellOccupancyIteration(preflightCellOccupancy(range, maxOccupancy))) {
        if (params.maxCellOccupancyPerShape > 0u) {
            range = shrinkCellRangeToOccupancyBudget(range, params.maxCellOccupancyPerShape);

        const CellRange2 range = shapeCellRange2(shapeIndex, bodies, shapes, params);


        }
        if (canSkipShapeCellInsertion(range, params)) {
        if (canSkipShapeCellInsertion(range, maxSpan, maxOccupancy)) {
            return;
        }
            return;
        for (s32 cy = range.minCell.y; cy <= range.maxCell.y; ++cy) {
            for (s32 cx = range.minCell.x; cx <= range.maxCell.x; ++cx) {

    CellRange3 range3 = {};
    CellRange2 range2 = {};
    if (!shapeCellRangeForInsert(shapeIndex, bodies, shapes, params, use2D, range3, range2)) {

        for (s32 cy = range2.minCell.y; cy <= range2.maxCell.y; ++cy) {
            for (s32 cx = range2.minCell.x; cx <= range2.maxCell.x; ++cx) {
                const u32 key = spatialHash2D(cx, cy, tableSize);
                cells.insert(key, bodyIndex);
        if (shouldRunCellOccupancyIteration(range, maxOccupancy)) {
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
    if (canSkipCellOccupancyIteration(preflightCellOccupancy(range, maxOccupancy))) {
    if (!shouldRunCellOccupancyIteration(range, maxOccupancy)) {
    if (canSkipShapeCellInsert(range, maxOccupancy)) {
    if (canSkipCellShapeInsert(bodyIndex, bodyCount, range, maxOccupancy)) {
    if (canSkipCellCapacityInsert(range, maxOccupancy, bodyIndex, bodyCount)) {
    if (!shouldRunShapeCellInsert(bodyIndex, bodyCount, range, maxOccupancy)) {
    if (canSkipCellCapacityInsert(bodyIndex, bodies.count(), range, maxOccupancy)) {
    if (!shouldRunCellCapacityInsert(bodyIndex, bodies.count(), range, maxOccupancy)) {
    if (!shouldRunCellCapacityInsert(bodyIndex, bodyCount, range, maxOccupancy)) {
    if (canSkipCellCapacityInsert(range, maxOccupancy)) {
    if (!shouldRunCellCapacityInsert(range, maxOccupancy)) {
    if (!preflightShapeCellInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert()) {
    if (canSkipShapeCellInsert(bodyIndex, bodies.count(), range, maxOccupancy)) {
    if (canSkipShapeCellOccupancyIteration(range, params)) {
    if (canSkipShapeCellInsertion(range, params)) {
    if (canSkipShapeCellInsertion(range, maxSpan, maxOccupancy)) {
        return;
    if (isEmptyCellRange(range)) {
    if (params.maxCellOccupancyPerShape > 0u) {
        range = shrinkCellRangeToOccupancyBudget(range, params.maxCellOccupancyPerShape);
    const CellRange3 range = shapeCellRange3(shapeIndex, bodies, shapes, params);
    const CellRange3& range = range3;
    for (s32 cz = range.minCell.z; cz <= range.maxCell.z; ++cz) {
        for (s32 cy = range.minCell.y; cy <= range.maxCell.y; ++cy) {
            for (s32 cx = range.minCell.x; cx <= range.maxCell.x; ++cx) {
                const u32 key = spatialHash(cx, cy, cz, tableSize);
                cells.insert(key, bodyIndex);
    if (shouldRunCellOccupancyIteration(range, maxOccupancy)) {
            }
        }
    }
}

void mergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, PairBufferSoA& buffer) {
    if (!shouldRunMergePairsIntoBuffer(pairs, buffer)) {
    const PairBufferMergePreflight mergePreflight =
        preflightPairBufferMerge(buffer, static_cast<u32>(pairs.size()));
    if (!mergePreflight.canMergeAny()) {
    if (!shouldRunPairBufferMergeInto(buffer, static_cast<u32>(pairs.size()))) {
    if (!shouldRunPairBufferMerge(buffer, static_cast<u32>(pairs.size()))) {
        return;
    }

    u32 accepted = 0u;
    for (const CandidatePair& pair : pairs) {
        if (!preflightPairBufferPush(buffer, pair.bodyA, pair.bodyB).canPush()) {
        if (accepted >= mergePreflight.acceptedCount) {
        if (canSkipPairBufferPush(buffer, pair.bodyA, pair.bodyB)) {
        if (!shouldRunPairBufferPush(buffer, pair.bodyA, pair.bodyB)) {
        if (!preflightMergePairPush(buffer, pair.bodyA, pair.bodyB).canPush()) {
        if (!preflightMergePairIntoBuffer(buffer, pair.bodyA, pair.bodyB).canMerge()) {
            break;
        if (!buffer.canAcceptPairs(1u)) {
        if (buffer.push(pair.bodyA, pair.bodyB)) {
            ++accepted;

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
    if (canSkipDedupeBroadphase(buffer) || canSkipPairBufferDedupe(buffer)) {
    if (!shouldRunPairBufferDedupe(buffer)) {
    if (canSkipDedupeBroadphase(buffer)) {
        return;
    }
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
    if (!shouldRunBroadphase(bodies, shapes)) {
    if (!shouldRunBroadphasePairGeneration(bodies, shapes)) {
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
    if (!shouldRunBroadphaseCellPairGeneration(totalCellSlots)) {
    if (!shouldRunBroadphasePairSlotWrite(totalCellSlots)) {
    if (!preflightBroadphaseCellPairGeneration(totalCellSlots).canDispatch()) {
    if (!shouldRunBroadphaseCellPairBuild(totalCellSlots)) {
        return;
    }

    buffer.preparePairSlots(totalCellSlots);

    fuse::jobs::parallel_for(0u, tableSize, kCellGrainSize, [&](u32 cellIndex) {
        const std::vector<u32>& occupants = cells.buckets[cellIndex];
        if (canSkipCellPairGeneration(static_cast<u32>(occupants.size()))) {
            return;
        }
        writePairsForCellSlots(cells.buckets[cellIndex], cellSlotOffsets[cellIndex], bodyCount, buffer);
        writePairsForCellSlots(occupants, cellSlotOffsets[cellIndex], buffer);
    });
    if (shouldRunPairBufferCompaction(buffer)) {
        buffer.compact();
    }
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
    if (shouldRunBroadphaseMergeIntoBuffer(bodies, shapes, buffer)) {
    if (shouldRunBroadphaseMergeLaunch(bodies, shapes)) {
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
            if (shouldRunMergePairsIntoBuffer(bucketPairs, buffer)) {
                mergePairsIntoBuffer(bucketPairs, buffer);
            }
        }
        dedupeBuffer(buffer);
    }

    if (shouldRunPairBufferClamp(buffer)) {
    if (!canSkipPairBufferClamp(buffer)) {
    if (preflightPairBufferClamp(buffer).needsClamp()) {
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
    const u32 bodyCount = bodies.count();
    fuse::jobs::parallel_for(0u, pairCount, kPlanePairGrainSize, [&](u32 pairIndex) {
        const RefinePairPreflight pairPreflight = preflightRefinePair(buffer, pairIndex, bodyCount);
        if (!pairPreflight.canRefine()) {
            if (pairPreflight.reason == RefinePairRejectReason::InvalidPair) {
                buffer.invalidateSlot(pairIndex);
            }
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
        if (!shouldRunPairBufferInvalidateSlot(buffer, pairIndex)) {
            return;
        if (!isValidCandidatePair(bodyA, bodyB, bodies.count())) {
            buffer.invalidateSlot(pairIndex);
            if (shouldRunPairBufferInvalidateSlot(buffer, pairIndex)) {
            if (shouldRunPairBufferInvalidate(buffer, pairIndex)) {
        if (!pairPassesAabbRefine(bodyA, bodyB, bodies, shapes)) {
        if (!pairPassesAabbRefine(bodyA, bodyB, bodies, shapes) &&
            shouldRunPairBufferInvalidateSlot(buffer, pairIndex)) {
            }
            return;
            shouldRunPairBufferInvalidate(buffer, pairIndex)) {
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
const char* broadphaseCellPairRejectReasonName(BroadphaseCellPairRejectReason reason) {
    switch (reason) {
    case BroadphaseCellPairRejectReason::None:
        return "None";
    case BroadphaseCellPairRejectReason::ZeroSlots:
        return "ZeroSlots";
    return "Unknown";

BroadphaseCellPairRejectReason broadphaseCellPairRejectReason(u32 totalCellSlots) {
    if (totalCellSlots == 0u) {
        return BroadphaseCellPairRejectReason::ZeroSlots;
    return BroadphaseCellPairRejectReason::None;

bool broadphaseCellPairRejectsForReason(u32 totalCellSlots, BroadphaseCellPairRejectReason expected) {
    return broadphaseCellPairRejectReason(totalCellSlots) == expected;

BroadphaseCellPairPreflight preflightBroadphaseCellPairs(u32 totalCellSlots) {
    BroadphaseCellPairPreflight preflight{};
    preflight.totalCellSlots = totalCellSlots;
    preflight.reason = broadphaseCellPairRejectReason(totalCellSlots);
    preflight.zeroSlots = preflight.reason == BroadphaseCellPairRejectReason::ZeroSlots;
    return preflight;

bool canSkipBroadphaseCellPairGeneration(u32 totalCellSlots) {
    return !preflightBroadphaseCellPairs(totalCellSlots).canGenerate();

bool shouldRunBroadphaseCellPairGeneration(u32 totalCellSlots) {
    return preflightBroadphaseCellPairs(totalCellSlots).canGenerate();

const char* cellSpanClampRejectReasonName(CellSpanClampRejectReason reason) {
    case CellSpanClampRejectReason::None:
    case CellSpanClampRejectReason::EmptyRange:
        return "EmptyRange";
    case CellSpanClampRejectReason::UnlimitedSpan:
        return "UnlimitedSpan";
const char* cellSpanRejectReasonName(CellSpanRejectReason reason) {
    case CellSpanRejectReason::None:
    case CellSpanRejectReason::EmptyRange:
    case CellSpanRejectReason::ExceedsSpanBudget:
        return "ExceedsSpanBudget";

const char* broadphaseShapeInsertRejectReasonName(BroadphaseShapeInsertRejectReason reason) {
    case BroadphaseShapeInsertRejectReason::None:
    case BroadphaseShapeInsertRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    case BroadphaseShapeInsertRejectReason::EmptyCellRange:
        return "EmptyCellRange";
    case BroadphaseShapeInsertRejectReason::ExceedsOccupancyBudget:
        return "ExceedsOccupancyBudget";

BroadphaseShapeInsertRejectReason broadphaseShapeInsertRejectReason(
const char* shapeCellInsertRejectReasonName(ShapeCellInsertRejectReason reason) {
    case ShapeCellInsertRejectReason::None:
    case ShapeCellInsertRejectReason::OutOfRangeBody:
    case ShapeCellInsertRejectReason::OccupancyRejected:
        return "OccupancyRejected";







const char* cellPairGenRejectReasonName(CellPairGenRejectReason reason) {
    case CellPairGenRejectReason::None:
namespace {

u32 uniqueOccupantCount(const std::vector<u32>& occupants) {
    std::vector<u32> uniqueBodies = occupants;
    std::sort(uniqueBodies.begin(), uniqueBodies.end());
    uniqueBodies.erase(std::unique(uniqueBodies.begin(), uniqueBodies.end()), uniqueBodies.end());
    return static_cast<u32>(uniqueBodies.size());

} // namespace

    case CellPairGenRejectReason::EmptyOccupants:
        return "EmptyOccupants";
    case CellPairGenRejectReason::SingleOccupant:
        return "SingleOccupant";

CellPairGenRejectReason cellPairGenRejectReason(u32 occupantCount) {
    if (occupantCount == 0u) {
        return CellPairGenRejectReason::EmptyOccupants;
    if (occupantCount < 2u) {
        return CellPairGenRejectReason::SingleOccupant;
    return CellPairGenRejectReason::None;

bool cellPairGenRejectsForReason(u32 occupantCount, CellPairGenRejectReason expected) {
    return cellPairGenRejectReason(occupantCount) == expected;

CellPairGenPreflight preflightCellPairGen(u32 occupantCount) {
    CellPairGenPreflight preflight{};
    preflight.occupantCount = occupantCount;
    preflight.reason = cellPairGenRejectReason(occupantCount);
    preflight.emptyOccupants = preflight.reason == CellPairGenRejectReason::EmptyOccupants;
    preflight.singleOccupant = preflight.reason == CellPairGenRejectReason::SingleOccupant;

bool canSkipCellPairGeneration(u32 occupantCount) {
    return !preflightCellPairGen(occupantCount).canGenerate();

bool shouldRunCellPairGeneration(u32 occupantCount) {
    return preflightCellPairGen(occupantCount).canGenerate();

const char* cellShapeInsertRejectReasonName(CellShapeInsertRejectReason reason) {
    case CellShapeInsertRejectReason::None:
    case CellShapeInsertRejectReason::OutOfRangeBody:
    case CellShapeInsertRejectReason::OccupancyRejected:

CellShapeInsertRejectReason cellShapeInsertRejectReason(









}



    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {
    if (shapeIndex >= shapes.count()) {
        return BroadphaseShapeInsertRejectReason::OutOfRangeBody;

    const u32 bodyIndex = shapes.bodyIndices[shapeIndex];
    if (bodyIndex >= bodies.count()) {
    const u32 bodyIndex = detail::shapeBodyIndex(shapes, shapeIndex);
        return CellShapeInsertRejectReason::OutOfRangeBody;

    const vec3 position = bodies.positions[bodyIndex];
    const f32 cellSize = clampCellSize(params.cellSize);
    const u32 maxSpan = params.maxCellSpanPerAxis;
    const u32 maxOccupancy = params.maxCellOccupancy;
    const CollisionShapeType type = static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
    const CollisionShapeType type = detail::shapeType(shapes, shapeIndex);

    if (use2D) {
        CellRange2 range = {};
        if (type == CollisionShapeType::Box) {
            const vec3 halfExtents = shapes.params[shapeIndex];
            const aabb bounds = aabbFromBox(position, halfExtents);
            range = cellRangeFromAabb2D(bounds, cellSize, maxSpan);
        } else {
            const f32 radius = shapes.params[shapeIndex].x;
            range = cellRangeFromSphere2D({position.x, position.y}, radius, cellSize, maxSpan);
        if (isEmptyCellRange(range)) {
            return BroadphaseShapeInsertRejectReason::EmptyCellRange;
        if (canSkipCellOccupancyIteration(range, maxOccupancy)) {
            return BroadphaseShapeInsertRejectReason::ExceedsOccupancyBudget;
        return BroadphaseShapeInsertRejectReason::None;

    CellRange3 range = {};
        range = cellRangeFromBox(position, halfExtents, cellSize, maxSpan);
        range = cellRangeFromSphere(position, radius, cellSize, maxSpan);

bool broadphaseShapeInsertRejectsForReason(
    bool use2D,
    BroadphaseShapeInsertRejectReason expected) {
    return broadphaseShapeInsertRejectReason(shapeIndex, bodies, shapes, params, use2D) == expected;

BroadphaseShapeInsertPreflight preflightBroadphaseShapeInsert(
    BroadphaseShapeInsertPreflight preflight{};
    preflight.reason = broadphaseShapeInsertRejectReason(shapeIndex, bodies, shapes, params, use2D);
    preflight.outOfRangeBody = preflight.reason == BroadphaseShapeInsertRejectReason::OutOfRangeBody;
    preflight.emptyCellRange = preflight.reason == BroadphaseShapeInsertRejectReason::EmptyCellRange;
    preflight.exceedsOccupancyBudget =
        preflight.reason == BroadphaseShapeInsertRejectReason::ExceedsOccupancyBudget;

bool canSkipBroadphaseShapeInsert(
    return !preflightBroadphaseShapeInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();

bool shouldRunBroadphaseShapeInsert(
    return preflightBroadphaseShapeInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();
const char* broadphaseCellSlotRejectReasonName(BroadphaseCellSlotRejectReason reason) {
    case BroadphaseCellSlotRejectReason::None:
    case BroadphaseCellSlotRejectReason::ZeroSlots:

BroadphaseCellSlotRejectReason broadphaseCellSlotRejectReason(u32 totalCellSlots) {
        return BroadphaseCellSlotRejectReason::ZeroSlots;
    return BroadphaseCellSlotRejectReason::None;

bool broadphaseCellSlotRejectsForReason(u32 totalCellSlots, BroadphaseCellSlotRejectReason expected) {
    return broadphaseCellSlotRejectReason(totalCellSlots) == expected;

BroadphaseCellSlotPreflight preflightBroadphaseCellSlots(u32 totalCellSlots) {
    BroadphaseCellSlotPreflight preflight{};
    preflight.reason = broadphaseCellSlotRejectReason(totalCellSlots);
    preflight.zeroSlots = preflight.reason == BroadphaseCellSlotRejectReason::ZeroSlots;

    return !preflightBroadphaseCellSlots(totalCellSlots).canGenerate();

    return preflightBroadphaseCellSlots(totalCellSlots).canGenerate();
        return ShapeCellInsertRejectReason::OutOfRangeBody;

    const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);

        const CellRange2 range = shapeCellRange2(shapeIndex, bodies, shapes, params);
            return ShapeCellInsertRejectReason::OccupancyRejected;
        return ShapeCellInsertRejectReason::None;

    const CellRange3 range = shapeCellRange3(shapeIndex, bodies, shapes, params);
    return shapeCellInsertRejectReasonImpl(shapeIndex, bodies, shapes, params, use2D);

ShapeCellInsertPreflight preflightShapeCellInsert(
    return preflightShapeCellInsertImpl(shapeIndex, bodies, shapes, params, use2D);

bool shapeCellInsertRejectsForReason(
            const f32 radius = detail::shapeRadius(shapes, shapeIndex);
            return CellShapeInsertRejectReason::OccupancyRejected;
        return CellShapeInsertRejectReason::None;


bool cellShapeInsertRejectsForReason(
    ShapeCellInsertRejectReason expected) {
    return shapeCellInsertRejectReason(shapeIndex, bodies, shapes, params, use2D) == expected;

    ShapeCellInsertPreflight preflight{};
    preflight.reason = shapeCellInsertRejectReason(shapeIndex, bodies, shapes, params, use2D);
    preflight.outOfRangeBody = preflight.reason == ShapeCellInsertRejectReason::OutOfRangeBody;
    preflight.occupancyRejected = preflight.reason == ShapeCellInsertRejectReason::OccupancyRejected;

bool canSkipShapeCellInsert(
    return !preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();

bool shouldRunShapeCellInsert(
    return preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();

const char* broadphaseCellPairGenRejectReasonName(BroadphaseCellPairGenRejectReason reason) {
    case BroadphaseCellPairGenRejectReason::None:
    case BroadphaseCellPairGenRejectReason::EmptyCells:
        return "EmptyCells";

BroadphaseCellPairGenRejectReason broadphaseCellPairGenRejectReason(u32 totalCellSlots) {
        return BroadphaseCellPairGenRejectReason::EmptyCells;
    return BroadphaseCellPairGenRejectReason::None;

bool broadphaseCellPairGenRejectsForReason(u32 totalCellSlots, BroadphaseCellPairGenRejectReason expected) {
    return broadphaseCellPairGenRejectReason(totalCellSlots) == expected;

BroadphaseCellPairGenPreflight preflightBroadphaseCellPairGen(u32 totalCellSlots) {
    BroadphaseCellPairGenPreflight preflight{};
    preflight.reason = broadphaseCellPairGenRejectReason(totalCellSlots);
    preflight.emptyCells = preflight.reason == BroadphaseCellPairGenRejectReason::EmptyCells;

bool canSkipBroadphaseCellPairGen(u32 totalCellSlots) {
    return !preflightBroadphaseCellPairGen(totalCellSlots).canGenerate();

bool shouldRunBroadphaseCellPairGen(u32 totalCellSlots) {
    return preflightBroadphaseCellPairGen(totalCellSlots).canGenerate();


    CellShapeInsertRejectReason expected) {
    return cellShapeInsertRejectReason(shapeIndex, bodies, shapes, params, use2D) == expected;

CellShapeInsertPreflight preflightShapeCellInsert(
    CellShapeInsertPreflight preflight{};
    preflight.reason = cellShapeInsertRejectReason(shapeIndex, bodies, shapes, params, use2D);
    preflight.outOfRangeBody = preflight.reason == CellShapeInsertRejectReason::OutOfRangeBody;
    preflight.occupancyRejected = preflight.reason == CellShapeInsertRejectReason::OccupancyRejected;




CellPairGenRejectReason cellPairGenRejectReason(const std::vector<u32>& occupants) {
    if (occupants.empty()) {
    if (occupants.size() < 2u) {
    if (uniqueOccupantCount(occupants) < 2u) {

bool cellPairGenRejectsForReason(const std::vector<u32>& occupants, CellPairGenRejectReason expected) {
    return cellPairGenRejectReason(occupants) == expected;

    preflight.reason = cellPairGenRejectReason(occupants);
    if (preflight.canGenerate()) {
        preflight.uniqueBodyCount = uniqueOccupantCount(occupants);
        preflight.pairSlotCount = estimatePairCountForUniqueBodies(preflight.uniqueBodyCount);



u32 countCellPairSlots(const std::vector<u32>& occupants) {
    return preflightCellPairGeneration(occupants).pairSlotCount;

const char* cellCapacityInsertRejectReasonName(CellCapacityInsertRejectReason reason) {
    case CellCapacityInsertRejectReason::None:
    case CellCapacityInsertRejectReason::OutOfRangeBody:
    case CellCapacityInsertRejectReason::EmptyRange:
    case CellCapacityInsertRejectReason::ExceedsBudget:
        return "ExceedsBudget";

    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    if (uniqueBodies.size() < 2u) {


    if (!preflight.emptyOccupants) {
        preflight.uniqueBodyCount = static_cast<u32>(uniqueBodies.size());
        preflight.pairCount = estimatePairCountForUniqueBodies(preflight.uniqueBodyCount);



u32 estimatePairCountForCell(const std::vector<u32>& occupants) {
    const CellPairGenPreflight preflight = preflightCellPairGeneration(occupants);
    return preflight.canGenerate() ? preflight.pairCount : 0u;

    case CellCapacityInsertRejectReason::ExceedsOccupancyBudget:

CellCapacityInsertRejectReason cellCapacityInsertRejectReason(
u32 countPairsForCell(const std::vector<u32>& occupants) {
    if (!shouldRunCellPairGeneration(occupants)) {
        return 0u;
    const u32 bodyCount = static_cast<u32>(uniqueBodies.size());
    return bodyCount > 1u ? bodyCount * (bodyCount - 1u) / 2u : 0u;

    case CellPairGenRejectReason::InsufficientOccupants:
        return "InsufficientOccupants";

    if (occupants.size() < 2u || uniqueOccupantCountForCellPairGen(occupants) < 2u) {
        return CellPairGenRejectReason::InsufficientOccupants;


CellPairGenPreflight preflightCellPairGen(const std::vector<u32>& occupants) {
    preflight.insufficientOccupants =
        preflight.reason == CellPairGenRejectReason::InsufficientOccupants;
    preflight.uniqueBodyCount = uniqueOccupantCountForCellPairGen(occupants);

    return !preflightCellPairGen(occupants).canGenerate();

    return preflightCellPairGen(occupants).canGenerate();

    case ShapeCellInsertRejectReason::EmptyRange:
    case ShapeCellInsertRejectReason::ExceedsBudget:

    u32 bodyIndex,
    u32 bodyCount,
    const CellRange3& range,
    u32 maxOccupancy) {
    if (bodyCount > 0u && bodyIndex >= bodyCount) {
        return CellCapacityInsertRejectReason::OutOfRangeBody;
        return CellCapacityInsertRejectReason::EmptyRange;
    if (exceedsCellOccupancyBudget(range, maxOccupancy)) {
        return CellCapacityInsertRejectReason::ExceedsBudget;
    return CellCapacityInsertRejectReason::None;

    const CellRange2& range,

bool cellCapacityInsertRejectsForReason(
    u32 maxOccupancy,
    CellCapacityInsertRejectReason expected) {
    return cellCapacityInsertRejectReason(bodyIndex, bodyCount, range, maxOccupancy) == expected;


CellCapacityInsertPreflight preflightCellCapacityInsert(
    CellCapacityInsertPreflight preflight{};
    preflight.reason = cellCapacityInsertRejectReason(bodyIndex, bodyCount, range, maxOccupancy);
    preflight.outOfRangeBody = preflight.reason == CellCapacityInsertRejectReason::OutOfRangeBody;
    preflight.emptyRange = preflight.reason == CellCapacityInsertRejectReason::EmptyRange;
    preflight.exceedsBudget = preflight.reason == CellCapacityInsertRejectReason::ExceedsBudget;


bool canSkipCellCapacityInsert(u32 bodyIndex, u32 bodyCount, const CellRange3& range, u32 maxOccupancy) {
    return !preflightCellCapacityInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert();

bool canSkipCellCapacityInsert(u32 bodyIndex, u32 bodyCount, const CellRange2& range, u32 maxOccupancy) {

bool shouldRunCellCapacityInsert(u32 bodyIndex, u32 bodyCount, const CellRange3& range, u32 maxOccupancy) {
    return preflightCellCapacityInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert();

bool shouldRunCellCapacityInsert(u32 bodyIndex, u32 bodyCount, const CellRange2& range, u32 maxOccupancy) {
        return CellCapacityInsertRejectReason::ExceedsOccupancyBudget;




        preflight.reason == CellCapacityInsertRejectReason::ExceedsOccupancyBudget;








}

    u32 shapeIndex,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
    bool use2D) {



    return preflightShapeCellInsertImpl(shapeIndex, bodies, shapes, params, use2D);
}

bool shapeCellInsertRejectsForReason(
    u32 shapeIndex,
    bool use2D,
    ShapeCellInsertRejectReason expected) {
    return shapeCellInsertRejectReason(shapeIndex, bodies, shapes, params, use2D) == expected;

bool canSkipShapeCellInsert(
    return !preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();

bool shouldRunShapeCellInsert(


    return preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();
        return ShapeCellInsertRejectReason::OutOfRangeBody;
    const CellOccupancyRejectReason occupancyReason = cellOccupancyRejectReason(range, maxOccupancy);
    if (occupancyReason == CellOccupancyRejectReason::EmptyRange) {
        return ShapeCellInsertRejectReason::EmptyRange;
    if (occupancyReason == CellOccupancyRejectReason::ExceedsBudget) {
        return ShapeCellInsertRejectReason::ExceedsBudget;
    return ShapeCellInsertRejectReason::None;

ShapeCellInsertRejectReason shapeCellInsertRejectReason(
    u32 bodyIndex,
    u32 bodyCount,
    u32 maxOccupancy) {
    if (bodyCount > 0u && bodyIndex >= bodyCount) {

    const CellRange3& range,
    return shapeCellInsertRejectReason(bodyIndex, bodyCount, range, maxOccupancy) == expected;


ShapeCellInsertPreflight preflightShapeCellInsert(
    ShapeCellInsertPreflight preflight{};
    preflight.reason = shapeCellInsertRejectReason(bodyIndex, bodyCount, range, maxOccupancy);
    preflight.outOfRangeBody = preflight.reason == ShapeCellInsertRejectReason::OutOfRangeBody;
    preflight.emptyRange = preflight.reason == ShapeCellInsertRejectReason::EmptyRange;
    preflight.exceedsBudget = preflight.reason == ShapeCellInsertRejectReason::ExceedsBudget;
    return preflight;


bool canSkipShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange3& range, u32 maxOccupancy) {
    return !preflightShapeCellInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert();

bool canSkipShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange2& range, u32 maxOccupancy) {

bool shouldRunShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange3& range, u32 maxOccupancy) {
    return preflightShapeCellInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert();

bool shouldRunShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange2& range, u32 maxOccupancy) {
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SpatialHashParams& params,
}

    u32 shapeIndex,
    bool use2D) {

}

RefineBroadphaseRejectReason refineBroadphaseRejectReason(
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
    }
    if (!preflightBroadphase(bodies, shapes).canRun()) {
        return RefineBroadphaseRejectReason::EmptyInput;
    }
    if (buffer.pairSlotCount > 0u && buffer.countValidSlots() == 0u) {
        return RefineBroadphaseRejectReason::AllSlotsInvalid;
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
    preflight.validPairCount = buffer.countValidSlots();
    preflight.activePairCount = buffer.activeCount;
    preflight.allSlotsInvalid = buffer.pairSlotCount > 0u && buffer.countValidSlots() == 0u;
    preflight.emptyInput = !preflightBroadphase(bodies, shapes).canRun();
    preflight.pairCount = buffer.activeCount;
    preflight.reason = refineBroadphaseRejectReason(bodies, shapes, buffer);

bool refineBroadphasePreflightRejectsForReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer,
    RefineBroadphaseRejectReason expected) {
    return preflightRefineBroadphase(bodies, shapes, buffer).reason == expected;
}

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
    return preflightRefineBroadphase(bodies, shapes, buffer).canRefine();
}

bool shouldRunRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return !canSkipRefineBroadphase(bodies, shapes, buffer);
}

bool shouldRunRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return !canSkipRefineBroadphase(bodies, shapes, buffer);
}

bool shouldRunRefineBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return !canSkipRefineBroadphase(bodies, shapes, buffer);
}

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
    }
    if (!buffer.hasDuplicateCanonicalPairs()) {
        return DedupeBroadphaseRejectReason::AlreadyUnique;
    return DedupeBroadphaseRejectReason::None;

bool dedupeBroadphaseRejectsForReason(const PairBufferSoA& buffer, DedupeBroadphaseRejectReason expected) {
    return dedupeBroadphaseRejectReason(buffer) == expected;

DedupeBroadphasePreflight preflightDedupeBroadphase(const PairBufferSoA& buffer) {
    DedupeBroadphasePreflight preflight{};
    preflight.singlePair = !preflight.emptyBuffer && buffer.activeCount <= 1u;
    preflight.pairCount = buffer.activeCount;
    preflight.reason = dedupeBroadphaseRejectReason(buffer);
    preflight.alreadyUnique = preflight.reason == DedupeBroadphaseRejectReason::AlreadyUnique;
    return preflight;
}

bool dedupeBroadphasePreflightRejectsForReason(
    const PairBufferSoA& buffer,
    DedupeBroadphaseRejectReason expected) {
    return preflightDedupeBroadphase(buffer).reason == expected;
}

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

bool dedupeBroadphaseWouldReduceCount(const PairBufferSoA& buffer) {
    return bufferHasDuplicateCanonicalPairs(buffer);
}

bool bufferHasDuplicateCanonicalPairs(const PairBufferSoA& buffer) {
    return buffer.hasDuplicateCanonicalPairs();

RefineDedupeBroadphasePreflight preflightRefineDedupeBroadphase(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    RefineDedupeBroadphasePreflight preflight{};
    preflight.refine = preflightRefineBroadphase(bodies, shapes, buffer);
    preflight.dedupe = preflightDedupeBroadphase(buffer);
    preflight.hasDuplicatePairs = bufferHasDuplicateCanonicalPairs(buffer);
    return preflight;

bool canSkipRefineDedupeBroadphase(
    const RefineDedupeBroadphasePreflight preflight = preflightRefineDedupeBroadphase(bodies, shapes, buffer);
    return !preflight.canRefine() && !preflight.needsDedupe();

BroadphaseMergeRejectReason broadphaseMergeRejectReason(
    const CollisionShapeSoA& shapes) {
    return preflightBroadphaseMerge(bodies, shapes).reason;
bool shouldRunRefineBroadphase(
    return !canSkipRefineBroadphase(bodies, shapes, buffer);

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
        if (hasPlaneBodies && hasDynamicBodies) {
            return BroadphaseMergeRejectReason::None;

    if (!hasPlaneBodies) {
        return BroadphaseMergeRejectReason::EmptyPlaneBodies;
    return BroadphaseMergeRejectReason::EmptyDynamicBodies;
    const BroadphaseMergePreflight preflight = preflightBroadphaseMerge(bodies, shapes);
    return preflight.reason;

bool broadphaseMergeRejectsForReason(
    BroadphaseMergeRejectReason expected) {
    return broadphaseMergeRejectReason(bodies, shapes) == expected;









const char* broadphaseMergeRejectReasonName(BroadphaseMergeRejectReason reason) {
    switch (reason) {
    case BroadphaseMergeRejectReason::None:
        return "None";
    case BroadphaseMergeRejectReason::EmptyPlaneBodies:
        return "EmptyPlaneBodies";
    case BroadphaseMergeRejectReason::EmptyDynamicBodies:
        return "EmptyDynamicBodies";
    return "Unknown";

            break;

    if (!hasDynamicBodies) {

MergeBroadphaseRejectReason mergeBroadphaseRejectReason(

bool mergeBroadphaseRejectsForReason(
    MergeBroadphaseRejectReason expected) {
    return mergeBroadphaseRejectReason(bodies, shapes) == expected;
}

    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return !preflightRefineDedupeBroadphase(bodies, shapes, buffer).canRefineDedupe();

bool shouldRunRefineDedupeBroadphase(
    return preflightRefineDedupeBroadphase(bodies, shapes, buffer).canRefineDedupe();

BroadphaseMergePreflight preflightBroadphaseMerge(
    const RigidBodySoA& bodies,
    BroadphaseMergePreflight preflight{};
    std::vector<u32> planeBodies;
    std::vector<u32> dynamicBodies;
    u32 planeBodyCount = 0u;
    u32 dynamicBodyCount = 0u;
    std::unordered_set<u32> planeBodies;
    std::unordered_set<u32> dynamicBodies;

    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        const u32 bodyIndex = shapes.bodyIndices[shapeIndex];
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const CollisionShapeType type = static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
        if (type == CollisionShapeType::Plane) {
            planeBodies.push_back(bodyIndex);
        } else if ((bodies.flags[bodyIndex] & RB_STATIC) == 0) {
            hasDynamicBodies = true;
        }
        if (hasPlaneBodies && hasDynamicBodies) {
            return BroadphaseMergeRejectReason::None;

    if (!hasPlaneBodies) {
        return BroadphaseMergeRejectReason::EmptyPlaneBodies;
    return BroadphaseMergeRejectReason::EmptyDynamicBodies;

bool broadphaseMergeRejectsForReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    BroadphaseMergeRejectReason expected) {
    return broadphaseMergeRejectReason(bodies, shapes) == expected;

BroadphaseMergePreflight preflightBroadphaseMerge(
    const CollisionShapeSoA& shapes) {
    BroadphaseMergePreflight preflight{};
namespace {

struct BroadphaseMergeScan {
    bool hasPlaneBodies = false;
    bool hasDynamicBodies = false;
};

BroadphaseMergeScan scanBroadphaseMergeBodies(
    BroadphaseMergeScan scan{};
    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        const u32 bodyIndex = shapes.bodyIndices[shapeIndex];
        if (bodyIndex >= bodies.count()) {
            continue;
        const CollisionShapeType type = static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
        if (type == CollisionShapeType::Plane) {
            scan.hasPlaneBodies = true;
            break;

    preflight.reason = broadphaseMergeRejectReason(bodies, shapes);
    preflight.hasPlaneBodies = hasPlaneBodies;
    preflight.hasDynamicBodies = hasDynamicBodies;
    preflight.emptyPlaneBodies = !hasPlaneBodies;
    preflight.emptyDynamicBodies = !hasDynamicBodies;
            ++planeBodyCount;
            ++dynamicBodyCount;

    preflight.planeBodyCount = planeBodyCount;
    preflight.dynamicBodyCount = dynamicBodyCount;
    preflight.estimatedMergePairs = planeBodyCount * dynamicBodyCount;

    preflight.emptyPlaneBodies = planeBodyCount == 0u;
    preflight.emptyDynamicBodies = dynamicBodyCount == 0u;
            dynamicBodies.push_back(bodyIndex);

    std::sort(planeBodies.begin(), planeBodies.end());
    planeBodies.erase(std::unique(planeBodies.begin(), planeBodies.end()), planeBodies.end());
    std::sort(dynamicBodies.begin(), dynamicBodies.end());
    dynamicBodies.erase(std::unique(dynamicBodies.begin(), dynamicBodies.end()), dynamicBodies.end());

    preflight.planeBodyCount = static_cast<u32>(planeBodies.size());
    preflight.dynamicBodyCount = static_cast<u32>(dynamicBodies.size());
    preflight.emptyPlaneBodies = preflight.planeBodyCount == 0u;
    preflight.emptyDynamicBodies = preflight.dynamicBodyCount == 0u;
            planeBodies.insert(bodyIndex);
            dynamicBodies.insert(bodyIndex);

    preflight.stats.planeBodyCount = static_cast<u32>(planeBodies.size());
    preflight.stats.dynamicBodyCount = static_cast<u32>(dynamicBodies.size());
    preflight.emptyPlaneBodies = preflight.stats.planeBodyCount == 0u;
    preflight.emptyDynamicBodies = preflight.stats.dynamicBodyCount == 0u;

    const std::vector<u32> uniquePlaneBodies = uniqueOccupants(planeBodies);
    const std::vector<u32> uniqueDynamicBodies = uniqueOccupants(dynamicBodies);
    preflight.planeBodyCount = static_cast<u32>(uniquePlaneBodies.size());
    preflight.dynamicBodyCount = static_cast<u32>(uniqueDynamicBodies.size());
    preflight.estimatedMergePairs = preflight.planeBodyCount * preflight.dynamicBodyCount;





    if (preflight.emptyPlaneBodies) {
        preflight.reason = BroadphaseMergeRejectReason::EmptyPlaneBodies;
    } else if (preflight.emptyDynamicBodies) {
        preflight.reason = BroadphaseMergeRejectReason::EmptyDynamicBodies;
    } else {
        preflight.reason = BroadphaseMergeRejectReason::None;

bool mergeBroadphasePreflightRejectsForReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    BroadphaseMergeRejectReason expected) {
    return preflightBroadphaseMerge(bodies, shapes).reason == expected;
}

BroadphaseMergeRejectReason mergeBroadphaseRejectReason(
    return preflightBroadphaseMerge(bodies, shapes).reason;

bool mergeBroadphaseRejectsForReason(
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
    preflight.reason = broadphaseMergeRejectReason(bodies, shapes);
        preflight.reason = MergeBroadphaseRejectReason::EmptyPlaneBodies;
        preflight.reason = MergeBroadphaseRejectReason::EmptyDynamicBodies;
        preflight.reason = MergeBroadphaseRejectReason::None;
    }
    return preflight;

    const PairBufferSoA& buffer) {

    preflight.emptyBuffer = buffer.canSkipSoAIteration();


const char* broadphaseMergeRejectReasonName(BroadphaseMergeRejectReason reason) {
    case BroadphaseMergeRejectReason::None:
    } else if (!hasDynamicBodies) {

    case BroadphaseMergeRejectReason::EmptyPlaneBodies:
        return "EmptyPlaneBodies";
    case BroadphaseMergeRejectReason::EmptyDynamicBodies:
        return "EmptyDynamicBodies";

BroadphaseMergeRejectReason broadphaseMergeRejectReason(

struct MergeBodyScan {

MergeBodyScan scanMergeBodies(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    MergeBodyScan scan{};
            hasPlaneBodies = true;



bool canSkipBroadphaseMerge(

bool shouldRunBroadphaseMerge(

            scan.hasDynamicBodies = true;
        if (scan.hasPlaneBodies && scan.hasDynamicBodies) {
    return scan;


    const BroadphaseMergeScan scan = scanBroadphaseMergeBodies(bodies, shapes);
    if (!scan.hasPlaneBodies) {
    if (!scan.hasDynamicBodies) {


    preflight.emptyPlaneBodies = !scan.hasPlaneBodies;
    preflight.emptyDynamicBodies = !scan.hasDynamicBodies;


    return !shouldRunBroadphaseMerge(bodies, shapes);
    if (!hasDynamicBodies) {






MergeBroadphaseRejectReason mergeBroadphaseRejectReason(
    const MergeBodyScan scan = scanMergeBodies(bodies, shapes);
        return MergeBroadphaseRejectReason::EmptyPlaneBodies;
        return MergeBroadphaseRejectReason::EmptyDynamicBodies;
    return MergeBroadphaseRejectReason::None;

    MergeBroadphaseRejectReason expected) {

    preflight.reason = mergeBroadphaseRejectReason(bodies, shapes);




    std::vector<u32> planeBodies;
    std::vector<u32> dynamicBodies;

            planeBodies.push_back(bodyIndex);
            dynamicBodies.push_back(bodyIndex);










    preflight.planeBodyCount = static_cast<u32>(planeBodies.size());
    preflight.dynamicBodyCount = static_cast<u32>(dynamicBodies.size());
    preflight.emptyPlaneBodies = planeBodies.empty();
    preflight.emptyDynamicBodies = dynamicBodies.empty();
    preflight.estimatedMergePairs = preflight.planeBodyCount * preflight.dynamicBodyCount;

        preflight.reason = BroadphaseMergeRejectReason::NoPlaneBodies;
        preflight.reason = BroadphaseMergeRejectReason::NoDynamicBodies;



    return !canSkipBroadphaseMerge(bodies, shapes);









bool canSkipBroadphaseMerge(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !preflightBroadphaseMerge(bodies, shapes).canMerge();
}

bool canSkipBroadphaseMerge(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    return !preflightBroadphaseMerge(bodies, shapes).canMerge();
}

bool canSkipBroadphaseMerge(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !preflightBroadphaseMerge(bodies, shapes).canMerge();
}

bool canSkipBroadphaseMerge(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !preflightBroadphaseMerge(bodies, shapes).canMerge();
}

BroadphaseMergeRejectReason broadphaseMergeRejectReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflightBroadphaseMerge(bodies, shapes).reason;
}

bool broadphaseMergeRejectsForReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    BroadphaseMergeRejectReason expected) {
    return broadphaseMergeRejectReason(bodies, shapes) == expected;
}

BroadphaseMergeBufferPreflight preflightBroadphaseMergeIntoBuffer(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    BroadphaseMergeBufferPreflight preflight{};
    const BroadphaseMergePreflight scenePreflight = preflightBroadphaseMerge(bodies, shapes);
    preflight.sceneReason = scenePreflight.reason;
    preflight.emptyPlaneBodies = scenePreflight.emptyPlaneBodies;
    preflight.emptyDynamicBodies = scenePreflight.emptyDynamicBodies;
    preflight.bufferFull = buffer.isFull();
    return preflight;
}

bool canSkipBroadphaseMergeIntoBuffer(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return !preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer).canMergeIntoBuffer();
}

bool shouldRunBroadphaseMergeIntoBuffer(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer).canMergeIntoBuffer();
}

u32 countRefinableBroadphasePairs(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    if (!shouldRunRefineBroadphase(bodies, shapes, buffer)) {
        return 0u;
    }

    u32 refinableCount = 0u;
    for (u32 pairIndex = 0; pairIndex < buffer.activeCount; ++pairIndex) {
        if (!buffer.slotIsValid(pairIndex)) {
            continue;
        }

        const u32 bodyA = buffer.bodyA[pairIndex];
        const u32 bodyB = buffer.bodyB[pairIndex];
        if (!isValidCandidatePair(bodyA, bodyB, bodies.count())) {
            continue;
        }
        if (pairPassesAabbRefine(bodyA, bodyB, bodies, shapes)) {
            ++refinableCount;
        }
    }
    return refinableCount;
}

bool hasRefinableBroadphasePair(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return countRefinableBroadphasePairs(bodies, shapes, buffer) > 0u;
}

const char* mergePairsIntoBufferRejectReasonName(MergePairsIntoBufferRejectReason reason) {
    switch (reason) {
    case MergePairsIntoBufferRejectReason::None:
        return "None";
    case MergePairsIntoBufferRejectReason::EmptyPairs:
        return "EmptyPairs";
    case MergePairsIntoBufferRejectReason::BufferFull:
        return "BufferFull";
    case MergePairsIntoBufferRejectReason::InsufficientCapacity:
        return "InsufficientCapacity";
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
    if (buffer.maxCapacity > 0u && buffer.remainingCapacity() == 0u) {
        return MergePairsIntoBufferRejectReason::InsufficientCapacity;
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
    preflight.requestedPairCount = static_cast<u32>(pairs.size());
    preflight.reason = mergePairsIntoBufferRejectReason(pairs, buffer);
    preflight.emptyPairs = preflight.reason == MergePairsIntoBufferRejectReason::EmptyPairs;
    preflight.bufferFull = preflight.reason == MergePairsIntoBufferRejectReason::BufferFull;

    if (preflight.reason == MergePairsIntoBufferRejectReason::None) {
        u32 mergeableCount = 0u;
        u32 remainingSlots = buffer.remainingCapacity();
        for (const CandidatePair& pair : pairs) {
            if (!isValidCandidatePair(pair.bodyA, pair.bodyB)) {
                continue;
            }
            if (remainingSlots == 0u) {
                break;
            ++mergeableCount;
            if (remainingSlots != UINT32_MAX) {
                --remainingSlots;
        preflight.mergeablePairCount = mergeableCount;
        preflight.partialCapacity =
            mergeableCount > 0u && mergeableCount < preflight.requestedPairCount;

    preflight.insufficientCapacity = preflight.reason == MergePairsIntoBufferRejectReason::InsufficientCapacity ||
        (buffer.maxCapacity > 0u && pairs.size() > buffer.remainingCapacity());
    preflight.pairsToMerge = static_cast<u32>(pairs.size());
    preflight.remainingCapacity = buffer.remainingCapacity();
    return preflight;
}

bool canSkipMergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, const PairBufferSoA& buffer) {
    return !preflightMergePairsIntoBuffer(pairs, buffer).canMerge();
}

bool shouldRunMergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, const PairBufferSoA& buffer) {
    return preflightMergePairsIntoBuffer(pairs, buffer).canMerge();
}

void mergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, PairBufferSoA& buffer) {
    if (!shouldRunMergePairsIntoBuffer(pairs, buffer)) {
        return;
    }

    for (const CandidatePair& pair : pairs) {
        if (!preflightPairBufferPush(buffer, pair.bodyA, pair.bodyB).canPush()) {
            break;
        buffer.push(pair.bodyA, pair.bodyB);

BroadphaseMergeBufferRejectReason mergeBroadphaseBufferRejectReason(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    if (!preflightBroadphaseMerge(bodies, shapes).canMerge()) {
        return BroadphaseMergeBufferRejectReason::SceneNotMergeable;
    if (buffer.isFull()) {
        return BroadphaseMergeBufferRejectReason::BufferAtCapacity;
    return BroadphaseMergeBufferRejectReason::None;

bool mergeBroadphaseBufferRejectsForReason(
    const PairBufferSoA& buffer,
    BroadphaseMergeBufferRejectReason expected) {
    return mergeBroadphaseBufferRejectReason(bodies, shapes, buffer) == expected;

BroadphaseMergeBufferPreflight preflightBroadphaseMergeIntoBuffer(
    BroadphaseMergeBufferPreflight preflight{};
    preflight.reason = mergeBroadphaseBufferRejectReason(bodies, shapes, buffer);
    preflight.sceneNotMergeable = preflight.reason == BroadphaseMergeBufferRejectReason::SceneNotMergeable;
    preflight.bufferAtCapacity = preflight.reason == BroadphaseMergeBufferRejectReason::BufferAtCapacity;
    return preflight;

bool canSkipBroadphaseMergeIntoBuffer(
    return !preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer).canMergeIntoBuffer();

bool shouldRunBroadphaseMergeIntoBuffer(
    return preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer).canMergeIntoBuffer();

const char* mergePairsIntoBufferRejectReasonName(MergePairsIntoBufferRejectReason reason) {
    switch (reason) {
    case MergePairsIntoBufferRejectReason::None:
        return "None";
    case MergePairsIntoBufferRejectReason::EmptyPairs:
        return "EmptyPairs";
    case MergePairsIntoBufferRejectReason::BufferAtCapacity:
        return "BufferAtCapacity";
const char* broadphaseCellPairRejectReasonName(BroadphaseCellPairRejectReason reason) {
    case BroadphaseCellPairRejectReason::None:
    case BroadphaseCellPairRejectReason::ZeroCellSlots:
        return "ZeroCellSlots";
    return "Unknown";

MergePairsIntoBufferRejectReason mergePairsIntoBufferRejectReason(
    const std::vector<CandidatePair>& pairs,
    if (pairs.empty()) {
        return MergePairsIntoBufferRejectReason::EmptyPairs;
        return MergePairsIntoBufferRejectReason::BufferAtCapacity;
    return MergePairsIntoBufferRejectReason::None;

bool mergePairsIntoBufferRejectsForReason(
    MergePairsIntoBufferRejectReason expected) {
    return mergePairsIntoBufferRejectReason(pairs, buffer) == expected;

MergePairsIntoBufferPreflight preflightMergePairsIntoBuffer(
    MergePairsIntoBufferPreflight preflight{};
    preflight.reason = mergePairsIntoBufferRejectReason(pairs, buffer);
    preflight.emptyPairs = preflight.reason == MergePairsIntoBufferRejectReason::EmptyPairs;
    preflight.bufferAtCapacity = preflight.reason == MergePairsIntoBufferRejectReason::BufferAtCapacity;

bool canSkipMergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, const PairBufferSoA& buffer) {
    return !preflightMergePairsIntoBuffer(pairs, buffer).canMerge();

bool shouldRunMergePairsIntoBuffer(const std::vector<CandidatePair>& pairs, const PairBufferSoA& buffer) {
    return preflightMergePairsIntoBuffer(pairs, buffer).canMerge();

u32 countRefinableBroadphasePairs(
    if (!shouldRunRefineBroadphase(bodies, shapes, buffer)) {
        return 0u;

    u32 refinableCount = 0u;
    for (u32 pairIndex = 0; pairIndex < buffer.activeCount; ++pairIndex) {
        if (!buffer.slotIsValid(pairIndex)) {
            continue;

        const u32 bodyA = buffer.bodyA[pairIndex];
        const u32 bodyB = buffer.bodyB[pairIndex];
        if (!isValidCandidatePair(bodyA, bodyB, bodies.count())) {
        if (pairPassesAabbRefine(bodyA, bodyB, bodies, shapes)) {
            ++refinableCount;
    return refinableCount;

bool hasRefinableBroadphasePair(
    return countRefinableBroadphasePairs(bodies, shapes, buffer) > 0u;

    case MergePairsIntoBufferRejectReason::BufferFull:
        return "BufferFull";
    case MergePairsIntoBufferRejectReason::AllInvalidPairs:
        return "AllInvalidPairs";

        return MergePairsIntoBufferRejectReason::BufferFull;

    bool hasValidPair = false;
        if (isValidCandidatePair(pair.bodyA, pair.bodyB)) {
            hasValidPair = true;
    if (!hasValidPair) {
        return MergePairsIntoBufferRejectReason::AllInvalidPairs;


    preflight.bufferFull = preflight.reason == MergePairsIntoBufferRejectReason::BufferFull;
    preflight.allInvalidPairs = preflight.reason == MergePairsIntoBufferRejectReason::AllInvalidPairs;



u32 countValidBroadphasePairs(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
    return buffer.countValidSlots() > 0u ? buffer.countValidSlots() : buffer.activeCount;

bool hasMultipleBroadphasePairs(const PairBufferSoA& buffer) {
    return countValidBroadphasePairs(buffer) > 1u;

BroadphaseMergeLaunchPreflight preflightBroadphaseMergeLaunch(
    const CollisionShapeSoA& shapes) {
    BroadphaseMergeLaunchPreflight preflight{};
    preflight.broadphase = preflightBroadphase(bodies, shapes);
    preflight.merge = preflightBroadphaseMerge(bodies, shapes);

bool canSkipBroadphaseMergeLaunch(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    return !preflightBroadphaseMergeLaunch(bodies, shapes).canLaunchMerge();

bool shouldRunBroadphaseMergeLaunch(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    return preflightBroadphaseMergeLaunch(bodies, shapes).canLaunchMerge();

BroadphaseMergeIntoBufferRejectReason mergeIntoBufferBroadphaseRejectReason(
    if (!shouldRunBroadphaseMerge(bodies, shapes)) {
        return BroadphaseMergeIntoBufferRejectReason::SceneNotMergeable;
        return BroadphaseMergeIntoBufferRejectReason::BufferFull;
    return BroadphaseMergeIntoBufferRejectReason::None;

bool mergeIntoBufferBroadphaseRejectsForReason(
    BroadphaseMergeIntoBufferRejectReason expected) {
    return mergeIntoBufferBroadphaseRejectReason(bodies, shapes, buffer) == expected;

BroadphaseMergeIntoBufferPreflight preflightBroadphaseMergeIntoBuffer(
    BroadphaseMergeIntoBufferPreflight preflight{};
    preflight.reason = mergeIntoBufferBroadphaseRejectReason(bodies, shapes, buffer);
    preflight.sceneNotMergeable =
        preflight.reason == BroadphaseMergeIntoBufferRejectReason::SceneNotMergeable;
    preflight.bufferFull = preflight.reason == BroadphaseMergeIntoBufferRejectReason::BufferFull;

    return !preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer).canMerge();

    return preflightBroadphaseMergeIntoBuffer(bodies, shapes, buffer).canMerge();

        return BroadphaseMergeBufferRejectReason::SceneRejected;


    preflight.sceneRejected = preflight.reason == BroadphaseMergeBufferRejectReason::SceneRejected;



RefineDedupeBroadphasePreflight preflightRefineDedupeBroadphase(
    RefineDedupeBroadphasePreflight preflight{};
    preflight.refineReason = refineBroadphaseRejectReason(bodies, shapes, buffer);
    preflight.dedupeReason = dedupeBroadphaseRejectReason(buffer);
BroadphaseCellPairRejectReason broadphaseCellPairRejectReason(u32 totalCellSlots) {
    if (totalCellSlots == 0u) {
        return BroadphaseCellPairRejectReason::ZeroCellSlots;
    return BroadphaseCellPairRejectReason::None;

bool broadphaseCellPairRejectsForReason(u32 totalCellSlots, BroadphaseCellPairRejectReason expected) {
    return broadphaseCellPairRejectReason(totalCellSlots) == expected;

BroadphaseCellPairPreflight preflightBroadphaseCellPairGeneration(u32 totalCellSlots) {
    BroadphaseCellPairPreflight preflight{};
    preflight.reason = broadphaseCellPairRejectReason(totalCellSlots);
    preflight.zeroCellSlots = preflight.reason == BroadphaseCellPairRejectReason::ZeroCellSlots;

bool canSkipBroadphaseCellPairGeneration(u32 totalCellSlots) {
    return !preflightBroadphaseCellPairGeneration(totalCellSlots).canDispatch();

bool shouldRunBroadphaseCellPairGeneration(u32 totalCellSlots) {
    return preflightBroadphaseCellPairGeneration(totalCellSlots).canDispatch();
BroadphaseCellPairBuildRejectReason broadphaseCellPairBuildRejectReason(u32 totalCellSlots) {
        return BroadphaseCellPairBuildRejectReason::NoCellSlots;
    return BroadphaseCellPairBuildRejectReason::None;

bool broadphaseCellPairBuildRejectsForReason(u32 totalCellSlots, BroadphaseCellPairBuildRejectReason expected) {
    return broadphaseCellPairBuildRejectReason(totalCellSlots) == expected;

BroadphaseCellPairBuildPreflight preflightBroadphaseCellPairBuild(u32 totalCellSlots) {
    BroadphaseCellPairBuildPreflight preflight{};
    preflight.reason = broadphaseCellPairBuildRejectReason(totalCellSlots);
    preflight.noCellSlots = preflight.reason == BroadphaseCellPairBuildRejectReason::NoCellSlots;

bool canSkipBroadphaseCellPairBuild(u32 totalCellSlots) {
    return !preflightBroadphaseCellPairBuild(totalCellSlots).canBuild();

bool shouldRunBroadphaseCellPairBuild(u32 totalCellSlots) {
    return preflightBroadphaseCellPairBuild(totalCellSlots).canBuild();
const char* cellPairGenRejectReasonName(CellPairGenRejectReason reason) {
    case CellPairGenRejectReason::None:
    case CellPairGenRejectReason::EmptyCell:
        return "EmptyCell";
    case CellPairGenRejectReason::SingleOccupant:
        return "SingleOccupant";

u32 countUniqueBodiesInCell(const std::vector<u32>& occupants) {
    if (occupants.empty()) {
    case CellPairGenRejectReason::InsufficientOccupants:
        return "InsufficientOccupants";

namespace {

u32 uniqueOccupantCount(const std::vector<u32>& occupants) {
    if (occupants.size() < 2u) {
        return occupants.size();
    std::vector<u32> uniqueBodies = occupants;
    std::sort(uniqueBodies.begin(), uniqueBodies.end());
    uniqueBodies.erase(std::unique(uniqueBodies.begin(), uniqueBodies.end()), uniqueBodies.end());
    return static_cast<u32>(uniqueBodies.size());

CellPairGenRejectReason cellPairGenRejectReason(const std::vector<u32>& occupants) {
        return CellPairGenRejectReason::EmptyCell;
    if (countUniqueBodiesInCell(occupants) <= 1u) {
        return CellPairGenRejectReason::SingleOccupant;
    return CellPairGenRejectReason::None;

bool cellPairGenRejectsForReason(const std::vector<u32>& occupants, CellPairGenRejectReason expected) {
    return cellPairGenRejectReason(occupants) == expected;

u32 estimateCellPairCount(const std::vector<u32>& occupants) {
    const CellPairGenPreflight preflight = preflightCellPairGeneration(occupants);
    return preflight.pairCount;

CellPairGenPreflight preflightCellPairGeneration(const std::vector<u32>& occupants) {
    CellPairGenPreflight preflight{};
    preflight.reason = cellPairGenRejectReason(occupants);
    preflight.emptyCell = preflight.reason == CellPairGenRejectReason::EmptyCell;
    preflight.singleOccupant = preflight.reason == CellPairGenRejectReason::SingleOccupant;
    preflight.uniqueBodyCount = countUniqueBodiesInCell(occupants);
    if (preflight.canGenerate()) {
        preflight.pairCount = preflight.uniqueBodyCount * (preflight.uniqueBodyCount - 1u) / 2u;

bool canSkipCellPairGeneration(const std::vector<u32>& occupants) {
    return !preflightCellPairGeneration(occupants).canGenerate();

bool shouldRunCellPairGeneration(const std::vector<u32>& occupants) {
    return preflightCellPairGeneration(occupants).canGenerate();

const char* cellShapeInsertRejectReasonName(CellShapeInsertRejectReason reason) {
    case CellShapeInsertRejectReason::None:
    case CellShapeInsertRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    case CellShapeInsertRejectReason::EmptyOccupancyRange:
        return "EmptyOccupancyRange";
    case CellShapeInsertRejectReason::ExceedsBudget:
        return "ExceedsBudget";


CellShapeInsertRejectReason cellShapeInsertRejectReasonImpl(
    u32 bodyIndex,
    u32 bodyCount,
    const CellRange3& range,
    u32 maxCells) {
    if (bodyCount > 0u && bodyIndex >= bodyCount) {
        return CellShapeInsertRejectReason::OutOfRangeBody;
    if (isEmptyCellRange(range)) {
        return CellShapeInsertRejectReason::EmptyOccupancyRange;
    if (exceedsCellOccupancyBudget(range, maxCells)) {
        return CellShapeInsertRejectReason::ExceedsBudget;
    return CellShapeInsertRejectReason::None;

    const CellRange2& range,

} // namespace

CellShapeInsertRejectReason cellShapeInsertRejectReason(
    return cellShapeInsertRejectReasonImpl(bodyIndex, bodyCount, range, maxCells);


bool cellShapeInsertRejectsForReason(
    u32 maxCells,
    CellShapeInsertRejectReason expected) {
    return cellShapeInsertRejectReason(bodyIndex, bodyCount, range, maxCells) == expected;


CellShapeInsertPreflight preflightCellShapeInsert(
    CellShapeInsertPreflight preflight{};
    preflight.reason = cellShapeInsertRejectReason(bodyIndex, bodyCount, range, maxCells);
    preflight.outOfRangeBody = preflight.reason == CellShapeInsertRejectReason::OutOfRangeBody;
    preflight.emptyOccupancyRange = preflight.reason == CellShapeInsertRejectReason::EmptyOccupancyRange;
    preflight.exceedsBudget = preflight.reason == CellShapeInsertRejectReason::ExceedsBudget;


bool canSkipCellShapeInsert(u32 bodyIndex, u32 bodyCount, const CellRange3& range, u32 maxCells) {
    return !preflightCellShapeInsert(bodyIndex, bodyCount, range, maxCells).canInsert();

bool canSkipCellShapeInsert(u32 bodyIndex, u32 bodyCount, const CellRange2& range, u32 maxCells) {

bool shouldRunCellShapeInsert(u32 bodyIndex, u32 bodyCount, const CellRange3& range, u32 maxCells) {
    return preflightCellShapeInsert(bodyIndex, bodyCount, range, maxCells).canInsert();

bool shouldRunCellShapeInsert(u32 bodyIndex, u32 bodyCount, const CellRange2& range, u32 maxCells) {

CellPairGenPreflight buildCellPairGenPreflight(u32 uniqueBodyCount) {

    preflight.uniqueBodyCount = uniqueBodyCount;
    preflight.pairCount = estimateCellPairCount(uniqueBodyCount);
    if (uniqueBodyCount < 2u) {
        preflight.reason = CellPairGenRejectReason::InsufficientOccupants;
        preflight.insufficientOccupants = true;
    } else {
        preflight.reason = CellPairGenRejectReason::None;

CellCapacityInsertRejectReason cellCapacityInsertRejectReasonImpl(
    const CellOccupancyPreflight& occupancyPreflight,
    u32 bodyCount) {
        return CellCapacityInsertRejectReason::OutOfRangeBody;
    if (occupancyPreflight.emptyRange) {
        return CellCapacityInsertRejectReason::EmptyRange;
    if (occupancyPreflight.exceedsBudget) {
        return CellCapacityInsertRejectReason::ExceedsBudget;
    return CellCapacityInsertRejectReason::None;

CellCapacityInsertPreflight buildCellCapacityInsertPreflight(


    CellCapacityInsertPreflight preflight{};
    preflight.occupancyCount = occupancyPreflight.occupancyCount;
    preflight.reason = cellCapacityInsertRejectReasonImpl(occupancyPreflight, bodyIndex, bodyCount);
    preflight.outOfRangeBody = preflight.reason == CellCapacityInsertRejectReason::OutOfRangeBody;
    preflight.emptyRange = preflight.reason == CellCapacityInsertRejectReason::EmptyRange;
    preflight.exceedsBudget = preflight.reason == CellCapacityInsertRejectReason::ExceedsBudget;


CellPairGenRejectReason cellPairGenRejectReason(u32 occupantCount) {
    return buildCellPairGenPreflight(occupantCount).reason;

    return preflightCellPairGen(occupants).reason;

bool cellPairGenRejectsForReason(u32 occupantCount, CellPairGenRejectReason expected) {
    return cellPairGenRejectReason(occupantCount) == expected;


CellPairGenPreflight preflightCellPairGen(u32 occupantCount) {
    return buildCellPairGenPreflight(occupantCount);

CellPairGenPreflight preflightCellPairGen(const std::vector<u32>& occupants) {
    return buildCellPairGenPreflight(uniqueOccupantCount(occupants));

bool canSkipCellPairGen(u32 occupantCount) {
    return !preflightCellPairGen(occupantCount).canGenerate();

bool canSkipCellPairGen(const std::vector<u32>& occupants) {
    return !preflightCellPairGen(occupants).canGenerate();

bool shouldRunCellPairGen(u32 occupantCount) {
    return preflightCellPairGen(occupantCount).canGenerate();

bool shouldRunCellPairGen(const std::vector<u32>& occupants) {
    return preflightCellPairGen(occupants).canGenerate();

u32 estimateCellPairCount(u32 uniqueBodyCount) {
    return estimatePairCountForUniqueBodies(uniqueBodyCount);

    return estimateCellPairCount(uniqueOccupantCount(occupants));














const char* cellCapacityInsertRejectReasonName(CellCapacityInsertRejectReason reason) {
    case CellCapacityInsertRejectReason::None:
    case CellCapacityInsertRejectReason::OutOfRangeBody:
    case CellCapacityInsertRejectReason::EmptyRange:
        return "EmptyRange";
    case CellCapacityInsertRejectReason::ExceedsBudget:

CellCapacityInsertRejectReason cellCapacityInsertRejectReason(
    return preflightCellCapacityInsert(range, maxCells, bodyIndex, bodyCount).reason;


bool cellCapacityInsertRejectsForReason(
    CellCapacityInsertRejectReason expected) {
    return cellCapacityInsertRejectReason(range, maxCells, bodyIndex, bodyCount) == expected;


CellCapacityInsertPreflight preflightCellCapacityInsert(
    return buildCellCapacityInsertPreflight(preflightCellOccupancy(range, maxCells), bodyIndex, bodyCount);


bool canSkipCellCapacityInsert(const CellRange3& range, u32 maxCells, u32 bodyIndex, u32 bodyCount) {
    return !preflightCellCapacityInsert(range, maxCells, bodyIndex, bodyCount).canInsert();

bool canSkipCellCapacityInsert(const CellRange2& range, u32 maxCells, u32 bodyIndex, u32 bodyCount) {

bool shouldRunCellCapacityInsert(const CellRange3& range, u32 maxCells, u32 bodyIndex, u32 bodyCount) {
    return preflightCellCapacityInsert(range, maxCells, bodyIndex, bodyCount).canInsert();

bool shouldRunCellCapacityInsert(const CellRange2& range, u32 maxCells, u32 bodyIndex, u32 bodyCount) {










u32 countUniqueCellOccupants(const std::vector<u32>& occupants) {
    return static_cast<u32>(uniqueOccupants(occupants).size());

    return cellPairCountForOccupants(occupants);

    case CellPairGenRejectReason::EmptyOccupants:
        return "EmptyOccupants";

        return CellPairGenRejectReason::EmptyOccupants;
    if (countUniqueCellOccupants(occupants) < 2u) {
        return CellPairGenRejectReason::InsufficientOccupants;


    preflight.emptyOccupants = preflight.reason == CellPairGenRejectReason::EmptyOccupants;
    preflight.insufficientOccupants = preflight.reason == CellPairGenRejectReason::InsufficientOccupants;
    preflight.uniqueOccupantCount = countUniqueCellOccupants(occupants);
    preflight.estimatedPairCount = estimateCellPairCount(occupants);



u32 countPairsForCellOccupants(const std::vector<u32>& occupants) {
    return countPairsForOccupantsInternal(occupants);



    preflight.uniqueBodyCount = countUniqueCellOccupants(occupants);
    preflight.pairCount = countPairsForCellOccupants(occupants);



u32 uniqueBodyPairCount(const std::vector<u32>& occupants) {
    const u32 bodyCount = static_cast<u32>(uniqueBodies.size());
    return bodyCount > 1u ? bodyCount * (bodyCount - 1u) / 2u : 0u;


    if (occupants.size() < 2u || uniqueBodyPairCount(occupants) == 0u) {


    preflight.pairCount = preflight.canGenerate() ? uniqueBodyPairCount(occupants) : 0u;

u32 countPairsForCell(const std::vector<u32>& occupants) {
    return preflightCellPairGen(occupants).pairCount;



ShapeCellInsertRejectReason shapeCellInsertRejectReason(
    u32 maxOccupancy) {
    if (bodyIndex >= bodyCount) {
        return ShapeCellInsertRejectReason::OutOfRangeBody;
    if (canSkipCellOccupancyIteration(range, maxOccupancy)) {
        return ShapeCellInsertRejectReason::ExceedsOccupancyBudget;
    return ShapeCellInsertRejectReason::None;


bool shapeCellInsertRejectsForReason(
    u32 maxOccupancy,
    ShapeCellInsertRejectReason expected) {
    return shapeCellInsertRejectReason(bodyIndex, bodyCount, range, maxOccupancy) == expected;


ShapeCellInsertPreflight preflightShapeCellInsert(
    ShapeCellInsertPreflight preflight{};
    preflight.reason = shapeCellInsertRejectReason(bodyIndex, bodyCount, range, maxOccupancy);
    preflight.outOfRangeBody = preflight.reason == ShapeCellInsertRejectReason::OutOfRangeBody;
    preflight.exceedsOccupancyBudget = preflight.reason == ShapeCellInsertRejectReason::ExceedsOccupancyBudget;


bool canSkipShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange3& range, u32 maxOccupancy) {
    return !preflightShapeCellInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert();

bool canSkipShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange2& range, u32 maxOccupancy) {

bool shouldRunShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange3& range, u32 maxOccupancy) {
    return preflightShapeCellInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert();

bool shouldRunShapeCellInsert(u32 bodyIndex, u32 bodyCount, const CellRange2& range, u32 maxOccupancy) {

    const u32 uniqueCount = countUniqueCellOccupants(occupants);
    return estimatePairCountForUniqueBodies(uniqueCount);

    if (uniqueCount == 0u) {
    if (uniqueCount == 1u) {


    preflight.pairCount = estimateCellPairCount(occupants);



        return CellCapacityInsertRejectReason::ExceedsOccupancy;


    return cellCapacityInsertRejectReason(bodyIndex, bodyCount, range, maxOccupancy) == expected;


    preflight.reason = cellCapacityInsertRejectReason(bodyIndex, bodyCount, range, maxOccupancy);
    preflight.exceedsOccupancy = preflight.reason == CellCapacityInsertRejectReason::ExceedsOccupancy;


bool canSkipCellCapacityInsert(
    return !preflightCellCapacityInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert();


bool shouldRunCellCapacityInsert(
    return preflightCellCapacityInsert(bodyIndex, bodyCount, range, maxOccupancy).canInsert();


        return static_cast<u32>(occupants.size());

u32 countPairsForOccupants(const std::vector<u32>& occupants) {
    const CellPairGenPreflight preflight = preflightCellPairGen(occupants);

    const u32 uniqueCount = uniqueOccupantCount(occupants);
    if (uniqueCount < 2u) {


    preflight.uniqueBodyCount = uniqueOccupantCount(occupants);
    preflight.pairCount = preflight.uniqueBodyCount > 1u
        ? preflight.uniqueBodyCount * (preflight.uniqueBodyCount - 1u) / 2u
        : 0u;




CellRange3 shapeCellRange3(
    const vec3& position,
    u32 shapeIndex,
    f32 cellSize,
    u32 maxSpan) {

    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    preflight.uniqueOccupantCount = static_cast<u32>(uniqueBodies.size());
    preflight.reason = cellPairGenRejectReason(preflight.uniqueOccupantCount);
    preflight.pairCount = estimatePairsForUniqueOccupants(preflight.uniqueOccupantCount);




    const u32 bodyIndex = shapeBodyIndex(shapes, shapeIndex);
    const vec3 position = bodies.positions[bodyIndex];
    const CollisionShapeType type = shapeType(shapes, shapeIndex);
    if (type == CollisionShapeType::Box) {
        const vec3 halfExtents = shapes.params[shapeIndex];
        return cellRangeFromBox(position, halfExtents, cellSize, maxSpan);
    const f32 radius = shapeRadius(shapes, shapeIndex);
    return cellRangeFromSphere(position, radius, cellSize, maxSpan);

CellRange2 shapeCellRange2(
        const aabb bounds = aabbFromBox(position, halfExtents);
        return cellRangeFromAabb2D(bounds, cellSize, maxSpan);
    return cellRangeFromSphere2D({position.x, position.y}, radius, cellSize, maxSpan);


    case CellCapacityInsertRejectReason::OccupancyRejected:
        return "OccupancyRejected";

    const SpatialHashParams& params,
    bool use2D) {
    if (bodyIndex >= bodies.count()) {

    const SpatialHashParams normalizedParams = normalizeSpatialHashParams(params);
    const u32 maxOccupancy = normalizedParams.maxCellOccupancy;

    if (use2D) {
        const CellRange2 range =
            shapeCellRange2(position, shapes, shapeIndex, normalizedParams.cellSize, normalizedParams.maxCellSpanPerAxis);
            return CellCapacityInsertRejectReason::OccupancyRejected;

    const CellRange3 range =
        shapeCellRange3(position, shapes, shapeIndex, normalizedParams.cellSize, normalizedParams.maxCellSpanPerAxis);

    bool use2D,
    return cellCapacityInsertRejectReason(shapeIndex, bodies, shapes, params, use2D) == expected;

    preflight.reason = cellCapacityInsertRejectReason(shapeIndex, bodies, shapes, params, use2D);
    preflight.occupancyRejected = preflight.reason == CellCapacityInsertRejectReason::OccupancyRejected;

    return !preflightCellCapacityInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();

    return preflightCellCapacityInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();
    case CellPairGenRejectReason::SingletonOccupants:
        return "SingletonOccupants";

CellPairGenRejectReason cellPairGenRejectReason(u32 occupantCount, u32 uniqueOccupantCount) {
    if (occupantCount < 2u) {
    if (uniqueOccupantCount < 2u) {
        return CellPairGenRejectReason::SingletonOccupants;

bool cellPairGenRejectsForReason(u32 occupantCount, u32 uniqueOccupantCount, CellPairGenRejectReason expected) {
    return cellPairGenRejectReason(occupantCount, uniqueOccupantCount) == expected;

CellPairGenPreflight preflightCellPairGeneration(u32 occupantCount, u32 uniqueOccupantCount) {
    preflight.reason = cellPairGenRejectReason(occupantCount, uniqueOccupantCount);
    preflight.singletonOccupants = preflight.reason == CellPairGenRejectReason::SingletonOccupants;
    preflight.pairCount = estimateCellPairCount(uniqueOccupantCount);

bool canSkipCellPairGeneration(u32 occupantCount, u32 uniqueOccupantCount) {
    return !preflightCellPairGeneration(occupantCount, uniqueOccupantCount).canGenerate();

bool shouldRunCellPairGeneration(u32 occupantCount, u32 uniqueOccupantCount) {
    return preflightCellPairGeneration(occupantCount, uniqueOccupantCount).canGenerate();

    case CellPairGenRejectReason::SingletonOccupant:
        return "SingletonOccupant";

const char* shapeCellInsertRejectReasonName(ShapeCellInsertRejectReason reason) {
    case ShapeCellInsertRejectReason::None:
    case ShapeCellInsertRejectReason::OutOfRangeBody:
    case ShapeCellInsertRejectReason::EmptyRange:
    case ShapeCellInsertRejectReason::ExceedsOccupancy:
        return "ExceedsOccupancy";

        return ShapeCellInsertRejectReason::EmptyRange;
    if (exceedsCellOccupancyBudget(range, maxOccupancy)) {
        return ShapeCellInsertRejectReason::ExceedsOccupancy;




    preflight.emptyRange = preflight.reason == ShapeCellInsertRejectReason::EmptyRange;
    preflight.exceedsOccupancy = preflight.reason == ShapeCellInsertRejectReason::ExceedsOccupancy;
    preflight.occupancyCount = estimateCellOccupancyCount(range);







ShapeCellInsertRejectReason shapeCellInsertRejectReasonFromOccupancy(const CellOccupancyPreflight& occupancy) {
    if (occupancy.emptyRange) {
        return ShapeCellInsertRejectReason::EmptyCellRange;
    if (occupancy.exceedsBudget) {


    case ShapeCellInsertRejectReason::EmptyCellRange:
        return "EmptyCellRange";
    case ShapeCellInsertRejectReason::ExceedsOccupancyBudget:
        return "ExceedsOccupancyBudget";

    return preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).reason;

    return shapeCellInsertRejectReason(shapeIndex, bodies, shapes, params, use2D) == expected;

        preflight.reason = ShapeCellInsertRejectReason::OutOfRangeBody;
        preflight.outOfRangeBody = true;

    const f32 cellSize = normalizedParams.cellSize;
    const u32 maxSpan = normalizedParams.maxCellSpanPerAxis;

        const CellRange2 range = shapeCellRange2(shapeIndex, bodies, shapes, cellSize, maxSpan);
        const CellOccupancyPreflight occupancy = preflightCellOccupancy(range, maxOccupancy);
        preflight.occupancyCount = occupancy.occupancyCount;
        preflight.reason = shapeCellInsertRejectReasonFromOccupancy(occupancy);
        preflight.emptyCellRange = preflight.reason == ShapeCellInsertRejectReason::EmptyCellRange;
        preflight.exceedsOccupancyBudget =
            preflight.reason == ShapeCellInsertRejectReason::ExceedsOccupancyBudget;

    const CellRange3 range = shapeCellRange3(shapeIndex, bodies, shapes, cellSize, maxSpan);

bool canSkipShapeCellInsert(
    return !preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();

bool shouldRunShapeCellInsert(
    return preflightShapeCellInsert(shapeIndex, bodies, shapes, params, use2D).canInsert();

    case ShapeCellInsertRejectReason::ExceedsBudget:

    return preflightShapeCellInsertImpl(shapeIndex, bodies, shapes, params, use2D).reason;


    return preflightShapeCellInsertImpl(shapeIndex, bodies, shapes, params, use2D);



const char* mergePairPushRejectReasonName(MergePairPushRejectReason reason) {
    case MergePairPushRejectReason::None:
    case MergePairPushRejectReason::InvalidPair:
        return "InvalidPair";
    case MergePairPushRejectReason::AtCapacity:
        return "AtCapacity";

MergePairPushRejectReason mergePairPushRejectReason(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    if (!isValidCandidatePair(idxA, idxB)) {
        return MergePairPushRejectReason::InvalidPair;
        return MergePairPushRejectReason::AtCapacity;
    return MergePairPushRejectReason::None;

bool mergePairPushRejectsForReason(
    u32 idxA,
    u32 idxB,
    MergePairPushRejectReason expected) {
    return mergePairPushRejectReason(buffer, idxA, idxB) == expected;

MergePairPushPreflight preflightMergePairPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    MergePairPushPreflight preflight{};
    preflight.reason = mergePairPushRejectReason(buffer, idxA, idxB);
    preflight.invalidPair = preflight.reason == MergePairPushRejectReason::InvalidPair;
    preflight.atCapacity = preflight.reason == MergePairPushRejectReason::AtCapacity;

MergePairPushRejectReason mergePairPushRejectReason(
    u32 bodyA,
    u32 bodyB) {
    const PairBufferPushRejectReason pushReason = pairBufferPushRejectReason(buffer, bodyA, bodyB);
    switch (pushReason) {
    case PairBufferPushRejectReason::InvalidPair:
    case PairBufferPushRejectReason::AtCapacity:
    case PairBufferPushRejectReason::None:

    u32 bodyB,
    return mergePairPushRejectReason(buffer, bodyA, bodyB) == expected;

MergePairPushPreflight preflightMergePairPush(const PairBufferSoA& buffer, u32 bodyA, u32 bodyB) {
    preflight.reason = mergePairPushRejectReason(buffer, bodyA, bodyB);

bool canSkipMergePairPush(const PairBufferSoA& buffer, u32 bodyA, u32 bodyB) {
    return !preflightMergePairPush(buffer, bodyA, bodyB).canPush();

bool shouldRunMergePairPush(const PairBufferSoA& buffer, u32 bodyA, u32 bodyB) {
    return preflightMergePairPush(buffer, bodyA, bodyB).canPush();


ShapeCellInsertRejectReason shapeCellInsertRejectReasonImpl(
    if (shapeIndex >= shapes.count()) {

    const u32 bodyIndex = shapes.bodyIndices[shapeIndex];

    CellRange3 range3 = {};
    CellRange2 range2 = {};
    if (!shapeCellRangeForInsert(shapeIndex, bodies, shapes, params, use2D, range3, range2)) {

    const u32 maxOccupancy = params.maxCellOccupancy;
        if (canSkipCellOccupancyIteration(range2, maxOccupancy)) {
            return ShapeCellInsertRejectReason::CellOccupancyRejected;
    } else if (canSkipCellOccupancyIteration(range3, maxOccupancy)) {


    case ShapeCellInsertRejectReason::CellOccupancyRejected:
        return "CellOccupancyRejected";

    return shapeCellInsertRejectReasonImpl(shapeIndex, bodies, shapes, params, use2D);


    preflight.reason = shapeCellInsertRejectReason(shapeIndex, bodies, shapes, params, use2D);
    preflight.cellOccupancyRejected = preflight.reason == ShapeCellInsertRejectReason::CellOccupancyRejected;



const char* refinePairRejectReasonName(RefinePairRejectReason reason) {
    case RefinePairRejectReason::None:
    case RefinePairRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case RefinePairRejectReason::InvalidSlot:
        return "InvalidSlot";
    case RefinePairRejectReason::InvalidPair:

RefinePairRejectReason refinePairRejectReason(
    u32 pairIndex,
    if (pairIndex >= buffer.activeCount) {
        return RefinePairRejectReason::OutOfRangeSlot;
        return RefinePairRejectReason::InvalidSlot;
    if (!isValidCandidatePair(bodyA, bodyB, bodyCount)) {
        return RefinePairRejectReason::InvalidPair;
    return RefinePairRejectReason::None;

bool refinePairRejectsForReason(
    RefinePairRejectReason expected) {
    return refinePairRejectReason(buffer, pairIndex, bodyCount) == expected;

RefinePairPreflight preflightRefinePair(
    RefinePairPreflight preflight{};
    preflight.reason = refinePairRejectReason(buffer, pairIndex, bodyCount);
    preflight.outOfRangeSlot = preflight.reason == RefinePairRejectReason::OutOfRangeSlot;
    preflight.invalidSlot = preflight.reason == RefinePairRejectReason::InvalidSlot;
    preflight.invalidPair = preflight.reason == RefinePairRejectReason::InvalidPair;

bool canSkipRefinePair(const PairBufferSoA& buffer, u32 pairIndex, u32 bodyCount) {
    return !preflightRefinePair(buffer, pairIndex, bodyCount).canRefine();

bool shouldRunRefinePair(const PairBufferSoA& buffer, u32 pairIndex, u32 bodyCount) {
    return preflightRefinePair(buffer, pairIndex, bodyCount).canRefine();
const char* mergePairIntoBufferRejectReasonName(MergePairIntoBufferRejectReason reason) {
    case MergePairIntoBufferRejectReason::None:
    case MergePairIntoBufferRejectReason::InvalidPair:
    case MergePairIntoBufferRejectReason::BufferFull:

MergePairIntoBufferRejectReason mergePairIntoBufferRejectReason(
    if (!isValidCandidatePair(bodyA, bodyB)) {
        return MergePairIntoBufferRejectReason::InvalidPair;
        return MergePairIntoBufferRejectReason::BufferFull;
    return MergePairIntoBufferRejectReason::None;

bool mergePairIntoBufferRejectsForReason(
    MergePairIntoBufferRejectReason expected) {
    return mergePairIntoBufferRejectReason(buffer, bodyA, bodyB) == expected;

MergePairIntoBufferPreflight preflightMergePairIntoBuffer(
    MergePairIntoBufferPreflight preflight{};
    preflight.reason = mergePairIntoBufferRejectReason(buffer, bodyA, bodyB);
    preflight.invalidPair = preflight.reason == MergePairIntoBufferRejectReason::InvalidPair;
    preflight.bufferFull = preflight.reason == MergePairIntoBufferRejectReason::BufferFull;

bool canSkipMergePairIntoBuffer(const PairBufferSoA& buffer, u32 bodyA, u32 bodyB) {
    return !preflightMergePairIntoBuffer(buffer, bodyA, bodyB).canMerge();

bool shouldRunMergePairIntoBuffer(const PairBufferSoA& buffer, u32 bodyA, u32 bodyB) {
    return preflightMergePairIntoBuffer(buffer, bodyA, bodyB).canMerge();

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

u32 countPairsForOccupants(const std::vector<u32>& occupants) {
    if (occupants.size() < 2u) {
        return 0u;
    }
    const std::vector<u32> uniqueBodies = uniqueOccupants(occupants);
    const u32 bodyCount = static_cast<u32>(uniqueBodies.size());
    return bodyCount > 1u ? bodyCount * (bodyCount - 1u) / 2u : 0u;

CellPairGenRejectReason cellPairGenRejectReason(const std::vector<u32>& occupants) {
        return CellPairGenRejectReason::SingletonOccupant;
u32 countUniqueCellOccupants(const std::vector<u32>& occupants) {
    if (occupants.empty()) {
    std::vector<u32> uniqueBodies = occupants;
    std::sort(uniqueBodies.begin(), uniqueBodies.end());
    uniqueBodies.erase(std::unique(uniqueBodies.begin(), uniqueBodies.end()), uniqueBodies.end());
    return static_cast<u32>(uniqueBodies.size());

u32 estimateCellPairCount(const std::vector<u32>& occupants) {
    const u32 uniqueBodyCount = countUniqueCellOccupants(occupants);
    return estimatePairCountForUniqueBodies(uniqueBodyCount);

        return CellPairGenRejectReason::EmptyOccupants;
    if (uniqueBodyCount < 2u) {
        return CellPairGenRejectReason::SingleOccupant;
    }
    return CellPairGenRejectReason::None;
}

bool cellPairGenRejectsForReason(const std::vector<u32>& occupants, CellPairGenRejectReason expected) {
    return cellPairGenRejectReason(occupants) == expected;
}

CellPairGenPreflight preflightCellPairGen(const std::vector<u32>& occupants) {
    CellPairGenPreflight preflight{};
    preflight.occupantCount = static_cast<u32>(occupants.size());
    preflight.reason = cellPairGenRejectReason(occupants);
    preflight.singletonOccupant = preflight.reason == CellPairGenRejectReason::SingletonOccupant;
    preflight.estimatedPairCount = countPairsForOccupants(occupants);
    preflight.emptyOccupants = preflight.reason == CellPairGenRejectReason::EmptyOccupants;
    preflight.singleOccupant = preflight.reason == CellPairGenRejectReason::SingleOccupant;
    preflight.uniqueBodyCount = countUniqueCellOccupants(occupants);
    preflight.estimatedPairCount = estimateCellPairCount(occupants);
    return preflight;
}

bool canSkipCellPairGen(const std::vector<u32>& occupants) {
    return !preflightCellPairGen(occupants).canGenerate();
}

bool shouldRunCellPairGen(const std::vector<u32>& occupants) {
    return preflightCellPairGen(occupants).canGenerate();
}

} // namespace fuse::physics::broadphase
