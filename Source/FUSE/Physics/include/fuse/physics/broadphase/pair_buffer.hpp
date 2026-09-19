#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::broadphase {

struct PairBufferSoA;

/// Pair-buffer slot preflight before `preparePairSlots` (B4.2 deepen pass).
struct PairSlotPreflight {
    bool skipped = false;
    u32 slotCount = 0u;
    bool exceedsBufferCapacity = false;
};

PairSlotPreflight preflightPairSlots(u32 slotCount, const PairBufferSoA& buffer);

/// SoA candidate-pair storage with clear/reuse for frame-to-frame broadphase output (B4.2 deepen).
struct PairBufferSoA {
    std::vector<u32> bodyA;
    std::vector<u32> bodyB;
    std::vector<u8> validFlags;

    u32 activeCount = 0;
    u32 pairSlotCount = 0;
    u32 maxCapacity = 0;
    u32 droppedCount = 0;
    CandidateRejectReason lastRejectReason = CandidateRejectReason::None;
    CandidatePairRejectReason lastRejectReason = CandidatePairRejectReason::None;

    bool isEmpty() const { return activeCount == 0u; }
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    bool hasValidPairs() const { return activeCount > 0u; }
    /// True when clamping dropped one or more candidate pairs.
    /// True when clamped or rejected pushes dropped pairs.
    bool hasDroppedPairs() const { return droppedCount > 0u; }
    /// True when `maxCapacity` is set and no additional pairs may be pushed.
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Pair-list guard: true when `additionalCount` pairs fit before `maxCapacity` clamp.
    bool canAcceptPairs(u32 additionalCount = 1u) const;
    /// Inverse of `canAcceptPairs` for overflow preflight (B4.2 deepen pass).
    bool wouldRejectAdditionalPairs(u32 additionalCount = 1u) const;
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;
    /// True when post-pass truncation would drop pairs.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.2 deepen follow-up).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    u32 remainingCapacity() const {
        return maxCapacity > 0u && activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
    }
    /// True when post-pass maxCapacity clamp is unnecessary.
    /// True when push would fail due to invalid pair or capacity.
    bool wouldRejectPush(u32 idxA, u32 idxB) const;
    /// True when compact+clamp would leave the buffer unchanged (B4.2 deepen pass).
    bool canSkipCompactAndClamp() const;
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when AABB refine can be skipped (no pairs to test).
    bool canSkipRefine() const { return canSkipSoAIteration(); }
    /// True when at most one canonical pair is present (dedupe is a no-op).
    bool canSkipDedupe() const { return canSkipSoAIteration() || activeCount <= 1u; }
    /// True when two or more valid slots share the same canonical pair (B4.2 deepen pass).
    bool hasDuplicateCanonicalPairs() const;
    /// True when dedupe would remove at least one duplicate pair (B4.2 deepen pass).
    bool needsDedupe() const;
    /// True when refine dispatch may early-out for this buffer alone (B4.2 deepen pass).
    bool canSkipRefine() const { return canSkipSoAIteration() || !hasValidPairs(); }
    bool canSkipDedupe() const;
    /// True when compact+clamp would leave the buffer unchanged.
    bool canSkipCompactAndClamp() const;
    /// True when refine would be a no-op (empty buffer or no valid pairs).
    /// Buffer-only refine guard: empty buffer or no valid pairs (B4.2 deepen follow-up).
    /// True when canonical sort would leave pair order unchanged (B4.2 deepen follow-up).
    bool canSkipSortCanonical() const;
    /// True when no duplicate canonical pairs are present (B4.2 deepen follow-up).
    bool isDuplicateFree() const;
    /// True when sort+unique dedupe pass would be a no-op (B4.2 deepen follow-up).
    /// Buffer-only refine guard: empty buffer or no valid pairs (B4.2 deepen follow-up pass).
    /// True when canonical sort would leave pair order unchanged (B4.2 deepen follow-up pass).
    /// True when no duplicate canonical pairs are present (B4.2 deepen follow-up pass).
    /// True when sort+unique dedupe pass would be a no-op (B4.2 deepen follow-up pass).
    bool canSkipDedupePass() const;
    /// True when canonical sort would be a no-op (B4.2 deepen follow-up pass).
    bool canSkipPairBufferSort() const;
    /// True when at least two valid slots share the same canonical pair (B4.2 deepen follow-up pass).
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// True when compact has no invalidated slots to gather.
    bool hasInvalidSlots() const;
    /// True when at most one valid pair is present (dedupe is a no-op).
    /// True when compact+clamp would leave the buffer unchanged (B4.2 deepen pass).
    /// True when refine dispatch has no valid pairs to scan (B4.2 deepen pass).
    bool canSkipRefineIteration() const;
    /// True when dedupe followed by clamp would be a no-op (B4.2 deepen pass).
    bool canSkipDedupeAndClamp() const;
    /// True when compact has invalidated slots to gather.
    /// True when at most one valid canonical pair is present (dedupe is a no-op).
    /// True when post-pass max-capacity clamp would drop pairs.
    bool needsMaxCapacityClamp() const { return canApplyMaxCapacityClamp(); }
    /// True when compact+clamp would leave the buffer unchanged (B4.2 deepen follow-up pass).
    bool canSkipCompactAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    /// True when canonical sort is a no-op (empty or single pair).
    bool canSkipSort() const { return canSkipSoAIteration() || activeCount <= 1u; }
    /// True when AABB refine can be skipped (no pairs to test).
    bool canSkipRefine() const { return canSkipSoAIteration(); }
    /// True when compact has no invalidated slots to gather.
    bool canSkipCompact() const;
    /// True when any prepared slot has been invalidated.
    bool hasInvalidSlots() const;
    bool slotIsValid(u32 slot) const;

    /// Const preflight for canonical pair dedupe dispatch (B4.2 deepen pass).
    struct DedupePreflight {
        u32 duplicateCount = 0;
        bool skipped = false;

        bool needs_dedupe() const { return !skipped && duplicateCount > 0u; }
    };

    DedupePreflight preflight_dedupe() const;

    void reserve(u32 capacity);
    /// Reserve pair slots for `uniqueBodyCount` canonical pairs (B4.2 deepen follow-up).
    void reserveForUniqueBodies(u32 uniqueBodyCount);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 slotCount);
    void writeSlot(u32 slot, u32 idxA, u32 idxB, u32 bodyCount = 0u);
    void invalidateSlot(u32 slot);
    bool wouldRejectPush(u32 idxA, u32 idxB, u32 bodyCount = 0u) const;
    bool push(u32 idxA, u32 idxB, u32 bodyCount = 0u);
    u32 invalidateInvalidPairs(u32 bodyCount);
    u32 compact();
    void sortCanonical();
    /// Sort only when `canSkipSortCanonical` is false (B4.2 deepen follow-up).
    /// Sort only when `canSkipSortCanonical` is false (B4.2 deepen follow-up pass).
    void sortCanonicalIfNeeded();
    u32 applyMaxCapacityClamp();
    u32 compactAndClamp();
    bool isSortedCanonical() const;
    bool containsCanonicalPair(u32 idxA, u32 idxB) const;
    /// Count active pairs failing the validity guard.
    u32 countInvalidPairs(u32 bodyCount = 0u) const;
    /// Invalidate and compact pairs failing the validity guard; returns remaining count.
    u32 pruneInvalidPairs(u32 bodyCount = 0u);
    CandidatePair pairAt(u32 index) const;
    std::vector<CandidatePair> toVector() const;
};

/// Why pair-buffer push would reject (B4.2 deepen pass).
/// Why a pair-buffer push would be rejected (B4.2 deepen pass).
/// Why pair-buffer push would reject a candidate pair (B4.2 deepen follow-up pass).
/// Why a pair-buffer push would be rejected (B4.2 deepen follow-up pass).
enum class PairBufferPushRejectReason : u8 {
    None = 0,
    InvalidPair,
    AtCapacity,
};

/// Human-readable label for pair-buffer push reject reasons (logging / tests).
const char* pairBufferPushRejectReasonName(PairBufferPushRejectReason reason);

/// Diagnose why push would reject; vacuously succeeds when push may proceed.
/// Diagnose why push would fail; vacuously succeeds when push may proceed.
PairBufferPushRejectReason pairBufferPushRejectReason(const PairBufferSoA& buffer, u32 idxA, u32 idxB);

/// Returns true when `pairBufferPushRejectReason` matches `expected` (B4.2 deepen pass).

/// Diagnose why push would reject; vacuously succeeds on valid pairs under capacity.

/// Returns true when `pairBufferPushRejectReason` matches `expected` (B4.2 deepen follow-up pass).


/// Diagnose why a push would fail; vacuously succeeds when push may proceed.


bool pairBufferPushRejectsForReason(
    const PairBufferSoA& buffer,
    u32 idxA,
    u32 idxB,
    PairBufferPushRejectReason expected);

/// Read-only push diagnostics — no mutation (B4.2 deepen follow-up).
/// Why pair-buffer push would early-out (B4.2 deepen follow-up pass).
enum class PairBufferPushRejectReason : u8 {
    None = 0,
    InvalidPair,
    AtCapacity,
};

/// Human-readable label for push reject reasons (logging / tests).
const char* pairBufferPushRejectReasonName(PairBufferPushRejectReason reason);

/// Diagnose why push would skip; vacuously succeeds when push may proceed.
PairBufferPushRejectReason pairBufferPushRejectReason(const PairBufferSoA& buffer, u32 idxA, u32 idxB);

/// Returns true when `pairBufferPushRejectReason` matches `expected` (B4.2 deepen follow-up pass).
bool pairBufferPushRejectsForReason(
    const PairBufferSoA& buffer,
    u32 idxA,
    u32 idxB,
    PairBufferPushRejectReason expected);

struct PairBufferPushPreflight {
    PairBufferPushRejectReason reason = PairBufferPushRejectReason::None;
    bool invalidPair = false;
    bool atCapacity = false;

    bool canPush() const { return reason == PairBufferPushRejectReason::None; }
};

/// Why pair-buffer push would reject (B4.2 deepen pass).
enum class PairBufferPushRejectReason : u8 {
    None = 0,
    InvalidPair,
    AtCapacity,
};

/// Human-readable label for pair-buffer push reject reasons (logging / tests).
const char* pairBufferPushRejectReasonName(PairBufferPushRejectReason reason);

/// Diagnose why push would reject; vacuously succeeds when push may proceed.
PairBufferPushRejectReason pairBufferPushRejectReason(const PairBufferSoA& buffer, u32 idxA, u32 idxB);

/// Returns true when `pairBufferPushRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferPushRejectsForReason(
    const PairBufferSoA& buffer,
    u32 idxA,
    u32 idxB,
    PairBufferPushRejectReason expected);

PairBufferPushPreflight preflightPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB);

/// Why pair-buffer compaction would early-out (B4.2 deepen pass).
enum class PairBufferCompactionRejectReason : u8 {
    EmptyBuffer,
    AllValid,
    None = 0,
/// Why pair-buffer compaction would early-out (B4.2 deepen follow-up pass).
/// Non-mutating push skip predicate — inverse of `preflightPairBufferPush` (B4.2 deepen pass).
bool canSkipPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB);

};

/// Human-readable label for pair-buffer compaction reject reasons (logging / tests).
const char* pairBufferCompactionRejectReasonName(PairBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed.

/// Human-readable label for compaction reject reasons (logging / tests).




/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed.
PairBufferCompactionRejectReason pairBufferCompactionRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferCompactionRejectReason` matches `expected` (B4.2 deepen pass).

/// Returns true when `pairBufferCompactionRejectReason` matches `expected` (B4.2 deepen follow-up pass).
bool pairBufferCompactionRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferCompactionRejectReason expected);

bool pairBufferCompactionRejectsForReason(const PairBufferSoA& buffer, PairBufferCompactionRejectReason expected);
/// Read-only push diagnostics with optional body-count bounds check (B4.2 deepen follow-up pass).
PairBufferPushPreflight preflightPairBufferPush(
    u32 idxA,
    u32 idxB,
    u32 bodyCount);

/// Read-only slot-write diagnostics — no mutation (B4.2 deepen follow-up pass).
struct PairBufferWriteSlotPreflight {
    bool invalidSlot = false;
    bool invalidPair = false;

    bool canWrite() const { return !invalidSlot && !invalidPair; }

PairBufferWriteSlotPreflight preflightPairBufferWriteSlot(
    u32 slot,
    u32 idxB);

/// Read-only slot-write diagnostics with optional body-count bounds check (B4.2 deepen follow-up pass).

/// Read-only merge-into-buffer diagnostics — no mutation (B4.2 deepen follow-up pass).
struct PairBufferMergePreflight {
    bool emptyIncoming = false;
    bool bufferAtCapacity = false;
    bool wouldTruncate = false;
    u32 incomingCount = 0;
    u32 remainingCapacity = 0;

    bool canMergeAll() const { return !emptyIncoming && !bufferAtCapacity && !wouldTruncate; }
    bool canMergeAny() const { return !emptyIncoming && remainingCapacity > 0u; }

PairBufferMergePreflight preflightPairBufferMerge(const PairBufferSoA& buffer, u32 incomingPairCount);


/// Read-only compaction diagnostics — no mutation (B4.2 deepen follow-up).
/// Why pair-buffer compaction would early-out (B4.2 deepen follow-up pass).
enum class PairBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for compaction reject reasons (logging / tests).
const char* pairBufferCompactionRejectReasonName(PairBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed.
PairBufferCompactionRejectReason pairBufferCompactionRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferCompactionRejectReason` matches `expected` (B4.2 deepen follow-up pass).
bool pairBufferCompactionRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferCompactionRejectReason expected);

struct PairBufferCompactionPreflight {
    PairBufferCompactionRejectReason reason = PairBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsCompaction() const { return reason == PairBufferCompactionRejectReason::None; }

/// Why pair-buffer compaction would early-out (B4.2 deepen pass).
enum class PairBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for pair-buffer compaction reject reasons (logging / tests).
const char* pairBufferCompactionRejectReasonName(PairBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed.
PairBufferCompactionRejectReason pairBufferCompactionRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferCompactionRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferCompactionRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferCompactionRejectReason expected);

PairBufferCompactionPreflight preflightPairBufferCompaction(const PairBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.2 deepen pass).
bool canSkipPairBufferCompaction(const PairBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflightPairBufferCompaction` (B4.2 deepen pass).
bool shouldRunPairBufferCompaction(const PairBufferSoA& buffer);

/// Why pair-buffer max-capacity clamp would early-out (B4.2 deepen pass).
enum class PairBufferClampRejectReason : u8 {
    WithinCapacity,
};


    None = 0,
    EmptyBuffer,


/// Non-mutating compaction predicate — inverse of `canSkipPairBufferCompaction` (B4.2 deepen pass).

/// Non-mutating compaction skip predicate — mirrors `PairBufferSoA::canSkipCompaction` inversion (B4.2 deepen pass).

/// Why pair-buffer max-capacity clamp would early-out (B4.2 deepen follow-up pass).


/// Non-mutating compaction skip predicate — inverse of `preflightPairBufferCompaction` (B4.2 deepen pass).




/// Human-readable label for pair-buffer clamp reject reasons (logging / tests).


/// Non-mutating compaction skip predicate — inverse of `PairBufferCompactionPreflight::needsCompaction` (B4.2 deepen pass).



/// Why max-capacity clamp would early-out (B4.2 deepen follow-up pass).

/// Human-readable label for clamp reject reasons (logging / tests).


/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.2 deepen follow-up pass).









/// Why max-capacity clamp would early-out (B4.2 deepen pass).





const char* pairBufferClampRejectReasonName(PairBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed.
PairBufferClampRejectReason pairBufferClampRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferClampRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferClampRejectsForReason(const PairBufferSoA& buffer, PairBufferClampRejectReason expected);



/// Non-mutating compaction skip predicate — inverse of `preflightPairBufferCompaction().needsCompaction()`.

/// Returns true when `pairBufferClampRejectReason` matches `expected` (B4.2 deepen follow-up pass).




/// Read-only max-capacity clamp diagnostics — no mutation (B4.2 deepen follow-up).
/// Why pair-buffer clamp would early-out (B4.2 deepen follow-up pass).
enum class PairBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

/// Human-readable label for clamp reject reasons (logging / tests).
const char* pairBufferClampRejectReasonName(PairBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed.
PairBufferClampRejectReason pairBufferClampRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferClampRejectReason` matches `expected` (B4.2 deepen follow-up pass).
bool pairBufferClampRejectsForReason(const PairBufferSoA& buffer, PairBufferClampRejectReason expected);

struct PairBufferClampPreflight {
    PairBufferClampRejectReason reason = PairBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needsClamp() const { return reason == PairBufferClampRejectReason::None; }
};

/// Why pair-buffer max-capacity clamp would early-out (B4.2 deepen pass).
enum class PairBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

/// Human-readable label for pair-buffer clamp reject reasons (logging / tests).
const char* pairBufferClampRejectReasonName(PairBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed.
PairBufferClampRejectReason pairBufferClampRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferClampRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferClampRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferClampRejectReason expected);

PairBufferClampPreflight preflightPairBufferClamp(const PairBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.2 deepen pass).
bool canSkipPairBufferClamp(const PairBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflightPairBufferClamp` (B4.2 deepen pass).
bool shouldRunPairBufferClamp(const PairBufferSoA& buffer);

/// Why pair-buffer dedupe would early-out at the SoA layer (B4.2 deepen pass).
enum class PairBufferDedupeRejectReason : u8 {
    SinglePair,
    bool emptyBuffer = false;

};


    None = 0,
    EmptyBuffer,



/// Non-mutating clamp predicate — inverse of `canSkipPairBufferClamp` (B4.2 deepen pass).

/// Non-mutating clamp skip predicate — mirrors `PairBufferSoA::canSkipMaxCapacityClamp` inversion (B4.2 deepen pass).

/// Why pair-buffer dedupe would early-out (B4.2 deepen pass).


/// Non-mutating clamp skip predicate — mirrors `PairBufferSoA::canSkipMaxCapacityClamp` (B4.2 deepen pass).



/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.2 deepen follow-up pass).

/// Why pair-buffer dedupe would early-out at the SoA layer (B4.2 deepen follow-up pass).
/// Non-mutating clamp skip predicate — inverse of `preflightPairBufferClamp` (B4.2 deepen pass).



/// Human-readable label for pair-buffer dedupe reject reasons (logging / tests).
const char* pairBufferDedupeRejectReasonName(PairBufferDedupeRejectReason reason);

/// Diagnose why SoA dedupe would skip; vacuously succeeds when dedupe may proceed.
/// Diagnose why dedupe would skip; vacuously succeeds when dedupe may proceed.


/// Human-readable label for SoA dedupe reject reasons (logging / tests).

PairBufferDedupeRejectReason pairBufferDedupeRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferDedupeRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferDedupeRejectsForReason(const PairBufferSoA& buffer, PairBufferDedupeRejectReason expected);




/// Non-mutating clamp skip predicate — inverse of `preflightPairBufferClamp().needsClamp()`.



/// Non-mutating clamp skip predicate — inverse of `PairBufferClampPreflight::needsClamp` (B4.2 deepen pass).


/// Read-only dedupe diagnostics at the SoA layer — no mutation (B4.2 deepen follow-up pass).
struct PairBufferDedupePreflight {
    PairBufferDedupeRejectReason reason = PairBufferDedupeRejectReason::None;
    bool singlePair = false;

    bool canDedupe() const { return reason == PairBufferDedupeRejectReason::None; }
    bool emptyBuffer = false;





/// Returns true when `pairBufferDedupeRejectReason` matches `expected` (B4.2 deepen follow-up pass).





};

/// Why pair-buffer dedupe would early-out (B4.2 deepen pass).
enum class PairBufferDedupeRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    SinglePair,
};

/// Human-readable label for pair-buffer dedupe reject reasons (logging / tests).
const char* pairBufferDedupeRejectReasonName(PairBufferDedupeRejectReason reason);

/// Diagnose why SoA dedupe would skip; vacuously succeeds when dedupe may proceed.
PairBufferDedupeRejectReason pairBufferDedupeRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferDedupeRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferDedupeRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferDedupeRejectReason expected);

PairBufferDedupePreflight preflightPairBufferDedupe(const PairBufferSoA& buffer);

/// Non-mutating dedupe predicate — inverse of `canSkipPairBufferDedupe` (B4.2 deepen pass).
bool shouldRunPairBufferDedupe(const PairBufferSoA& buffer);

/// Non-mutating dedupe skip predicate — mirrors `PairBufferSoA::canSkipDedupe` (B4.2 deepen follow-up pass).
bool canSkipPairBufferDedupe(const PairBufferSoA& buffer);

/// Non-mutating dedupe predicate — inverse of `canSkipPairBufferDedupe` (B4.2 deepen pass).
/// Non-mutating dedupe launch predicate — inverse of `canSkipPairBufferDedupe` (B4.2 deepen pass).
bool shouldRunPairBufferDedupe(const PairBufferSoA& buffer);

/// Why pair-buffer canonical sort would early-out (B4.2 deepen pass).
/// Why canonical sort would early-out at the SoA layer (B4.2 deepen follow-up pass).
/// Why canonical sort would early-out (B4.2 deepen pass).
/// Non-mutating dedupe predicate — mirrors `preflightPairBufferDedupe` (B4.2 deepen follow-up pass).

/// Why pair-buffer canonical sort would early-out (B4.2 deepen follow-up pass).
enum class PairBufferSortRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    SinglePair,
};

/// Human-readable label for pair-buffer sort reject reasons (logging / tests).
/// Human-readable label for sort reject reasons (logging / tests).
const char* pairBufferSortRejectReasonName(PairBufferSortRejectReason reason);

/// Diagnose why sort would skip; vacuously succeeds when sort may proceed.
PairBufferSortRejectReason pairBufferSortRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferSortRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferSortRejectsForReason(const PairBufferSoA& buffer, PairBufferSortRejectReason expected);

/// Non-mutating dedupe predicate — inverse of `canSkipPairBufferDedupe` (B4.2 deepen follow-up pass).

/// Read-only canonical-sort diagnostics — no mutation (B4.2 deepen follow-up pass).
struct PairBufferSortPreflight {
    PairBufferSortRejectReason reason = PairBufferSortRejectReason::None;

    bool needsSort() const { return reason == PairBufferSortRejectReason::None; }

/// Why pair-buffer canonical sort would early-out (B4.2 deepen pass).
enum class PairBufferSortRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    SinglePair,
    bool emptyBuffer = false;
    bool singlePair = false;

/// Returns true when `pairBufferSortRejectReason` matches `expected` (B4.2 deepen follow-up pass).


};

/// Human-readable label for pair-buffer sort reject reasons (logging / tests).
const char* pairBufferSortRejectReasonName(PairBufferSortRejectReason reason);

/// Diagnose why sort would skip; vacuously succeeds when sort may proceed.
PairBufferSortRejectReason pairBufferSortRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferSortRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferSortRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferSortRejectReason expected);

PairBufferSortPreflight preflightPairBufferSort(const PairBufferSoA& buffer);

/// Non-mutating sort skip predicate — inverse of `needsSort` (B4.2 deepen pass).
bool canSkipPairBufferSort(const PairBufferSoA& buffer);

/// Non-mutating sort predicate — mirrors `preflightPairBufferSort` (B4.2 deepen pass).
bool shouldRunPairBufferSort(const PairBufferSoA& buffer);

/// Why pair-buffer compact-and-clamp would early-out (B4.2 deepen pass).
enum class PairBufferCompactAndClampRejectReason : u8 {
    NoWork,

/// Human-readable label for compact-and-clamp reject reasons (logging / tests).
const char* pairBufferCompactAndClampRejectReasonName(PairBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed.
/// Non-mutating sort launch predicate — mirrors `preflightPairBufferSort` (B4.2 deepen pass).

/// Pair-buffer slot preflight before `preparePairSlots` (B4.2 deepen pass).
struct PairSlotPreflight {
    bool skipped = false;
    u32 slotCount = 0u;
    bool exceedsBufferCapacity = false;

    bool canPrepare() const { return !skipped; }
};

PairSlotPreflight preflightPairSlots(u32 slotCount, const PairBufferSoA& buffer);

/// Why pair-buffer compact+clamp would early-out (B4.2 deepen pass).
    None = 0,
    EmptyBuffer,
    NoWorkNeeded,

/// Human-readable label for compact+clamp reject reasons (logging / tests).
/// Non-mutating sort skip predicate — inverse of `needsSort` (B4.2 deepen follow-up pass).

/// Non-mutating sort predicate — mirrors `preflightPairBufferSort` (B4.2 deepen follow-up pass).

/// Why pair-buffer compact+clamp would early-out (B4.2 deepen follow-up pass).
    AlreadyCompactAndWithinCapacity,

/// Human-readable label for pair-buffer compact+clamp reject reasons (logging / tests).

/// Diagnose why compact+clamp would skip; vacuously succeeds when work may proceed.
PairBufferCompactAndClampRejectReason pairBufferCompactAndClampRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferCompactAndClampRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferCompactAndClampRejectsForReason(
    PairBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.2 deepen pass).
struct PairBufferCompactAndClampPreflight {
    PairBufferCompactAndClampRejectReason reason = PairBufferCompactAndClampRejectReason::None;
    bool noWork = false;

    bool needsCompactAndClamp() const { return reason == PairBufferCompactAndClampRejectReason::None; }

PairBufferCompactAndClampPreflight preflightPairBufferCompactAndClamp(const PairBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.2 deepen pass).
bool canSkipPairBufferCompactAndClamp(const PairBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflightPairBufferCompactAndClamp` (B4.2 deepen pass).
bool shouldRunPairBufferCompactAndClamp(const PairBufferSoA& buffer);

/// Why pair-buffer slot write would reject (B4.2 deepen follow-up pass).
enum class PairBufferWriteSlotRejectReason : u8 {
    OutOfRangeSlot,

/// Human-readable label for pair-buffer write-slot reject reasons (logging / tests).
const char* pairBufferWriteSlotRejectReasonName(PairBufferWriteSlotRejectReason reason);

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed.
PairBufferWriteSlotRejectReason pairBufferWriteSlotRejectReason(
    u32 slot,
    u32 idxB);

/// Returns true when `pairBufferWriteSlotRejectReason` matches `expected` (B4.2 deepen follow-up pass).
bool pairBufferWriteSlotRejectsForReason(
    PairBufferWriteSlotRejectReason expected);

/// Read-only write-slot diagnostics — no mutation (B4.2 deepen follow-up pass).
struct PairBufferWriteSlotPreflight {
    PairBufferWriteSlotRejectReason reason = PairBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;

    bool canWrite() const { return reason == PairBufferWriteSlotRejectReason::None; }

PairBufferWriteSlotPreflight preflightPairBufferWriteSlot(

/// Non-mutating write-slot skip predicate — inverse of `canWrite` (B4.2 deepen follow-up pass).
bool canSkipPairBufferWriteSlot(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB);

/// Non-mutating write-slot predicate — mirrors `preflightPairBufferWriteSlot` (B4.2 deepen follow-up pass).
bool shouldRunPairBufferWriteSlot(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB);

/// Early-out when write-slot preflight would reject — same ordering as `canSkipPairBufferWriteSlot` (B4.2 deepen pass).
bool wouldSkipPairBufferWriteSlot(
    PairBufferWriteSlotRejectReason* reason = nullptr);

/// Why pair-buffer slot invalidate would early-out (B4.2 deepen pass).
enum class PairBufferInvalidateSlotRejectReason : u8 {
    AlreadyInvalid,

/// Human-readable label for pair-buffer invalidate-slot reject reasons (logging / tests).
const char* pairBufferInvalidateSlotRejectReasonName(PairBufferInvalidateSlotRejectReason reason);

/// Diagnose why invalidateSlot would skip; vacuously succeeds when invalidate may proceed.
PairBufferInvalidateSlotRejectReason pairBufferInvalidateSlotRejectReason(const PairBufferSoA& buffer, u32 slot);

/// Returns true when `pairBufferInvalidateSlotRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferInvalidateSlotRejectsForReason(
    PairBufferInvalidateSlotRejectReason expected);

/// Read-only invalidate-slot diagnostics — no mutation (B4.2 deepen pass).
struct PairBufferInvalidateSlotPreflight {
    PairBufferInvalidateSlotRejectReason reason = PairBufferInvalidateSlotRejectReason::None;
    bool alreadyInvalid = false;

    bool canInvalidate() const { return reason == PairBufferInvalidateSlotRejectReason::None; }

PairBufferInvalidateSlotPreflight preflightPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot);

/// Non-mutating invalidate-slot skip predicate — inverse of `canInvalidate` (B4.2 deepen pass).
bool canSkipPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot);

/// Non-mutating invalidate-slot predicate — mirrors `preflightPairBufferInvalidateSlot` (B4.2 deepen pass).
bool shouldRunPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot);

/// Early-out when invalidate-slot preflight would reject — same ordering as `canSkipPairBufferInvalidateSlot` (B4.2 deepen pass).
bool wouldSkipPairBufferInvalidateSlot(
    PairBufferInvalidateSlotRejectReason* reason = nullptr);

/// Why pair-buffer toVector would early-out (B4.2 deepen follow-up pass).
enum class PairBufferToVectorRejectReason : u8 {

/// Human-readable label for pair-buffer toVector reject reasons (logging / tests).
const char* pairBufferToVectorRejectReasonName(PairBufferToVectorRejectReason reason);

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed.
PairBufferToVectorRejectReason pairBufferToVectorRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferToVectorRejectReason` matches `expected` (B4.2 deepen follow-up pass).
bool pairBufferToVectorRejectsForReason(const PairBufferSoA& buffer, PairBufferToVectorRejectReason expected);

/// Read-only toVector diagnostics — no mutation (B4.2 deepen follow-up pass).
struct PairBufferToVectorPreflight {
    PairBufferToVectorRejectReason reason = PairBufferToVectorRejectReason::None;

    bool canExport() const { return reason == PairBufferToVectorRejectReason::None; }

PairBufferToVectorPreflight preflightPairBufferToVector(const PairBufferSoA& buffer);

/// Non-mutating toVector skip predicate — inverse of `canExport` (B4.2 deepen follow-up pass).
bool canSkipPairBufferToVector(const PairBufferSoA& buffer);

/// Non-mutating toVector predicate — mirrors `preflightPairBufferToVector` (B4.2 deepen follow-up pass).
bool shouldRunPairBufferToVector(const PairBufferSoA& buffer);

/// Why pair-buffer slot invalidation would reject (B4.2 deepen pass).


/// Diagnose why invalidateSlot would reject; vacuously succeeds when invalidation may proceed.






/// Empty-set refine guard: skip refine when input is empty or the pair buffer has no valid slots (B4.2 deepen pass).
inline bool canSkipBroadphaseRefine(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const PairBufferSoA& buffer) {
    return canSkipBroadphase(bodies, shapes) || buffer.canSkipRefineIteration();
}

/// Empty-output guard: true when broadphase produced no active pairs (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool isEmptyBroadphaseOutput(const PairBufferSoA& buffer) {
    return buffer.isEmpty() || buffer.canSkipSoAIteration();

/// Const preflight for pair-buffer dedupe dispatch (B4.2 deepen pass).

    u32 activePairCount = 0;
    bool skipped = false;

    bool needs_dedupe() const { return !skipped && activePairCount > 1u; }
    bool can_dedupe() const { return needs_dedupe(); }

/// Populate dedupe preflight without sorting pair slots (B4.2 deepen pass).
PairBufferDedupePreflight preflight_dedupe_pair_buffer(const PairBufferSoA& buffer);

/// Returns true when dedupe should skip before sort/unique pass (B4.2 deepen pass).
bool should_skip_dedupe_pair_buffer(const PairBufferSoA& buffer);

/// Const preflight for pair-buffer capacity clamp (B4.2 deepen pass).
    u32 maxCapacity = 0;
    u32 excessCount = 0;

    bool needs_clamp() const { return !skipped && maxCapacity > 0u && activePairCount > maxCapacity; }
    bool can_clamp() const { return needs_clamp(); }


/// Populate clamp preflight without truncating pair slots (B4.2 deepen pass).
PairBufferClampPreflight preflight_pair_buffer_clamp(const PairBufferSoA& buffer);
/// Const preflight for pair-buffer dedupe (B4.2 deepen pass).
    u32 activeCount = 0;

    bool can_dedupe() const { return !skipped && activeCount > 1u; }

/// Populate dedupe preflight without sorting or compacting pairs (B4.2 deepen pass).
PairBufferDedupePreflight preflight_pair_buffer_dedupe(const PairBufferSoA& buffer);

/// Early-out guard for pair-buffer dedupe (B4.2 deepen pass).
bool should_skip_pair_buffer_dedupe(const PairBufferSoA& buffer);

/// Const preflight for pair-buffer capacity and compaction guards (B4.2 deepen pass).
struct PairBufferPreflight {
    u32 droppedCount = 0;
    u32 remainingCapacity = 0;
    bool empty = false;
    bool full = false;
    bool skipDedupe = false;
    bool skipCompaction = false;
    bool needsClamp = false;

    bool can_push(u32 additionalCount = 1u) const;
    bool can_compact() const { return !skipped && !skipCompaction; }

PairBufferPreflight preflight_pair_buffer(const PairBufferSoA& buffer);

/// True when dedupe is unnecessary for this buffer state (B4.2 deepen pass).

/// True when compaction would be a no-op (B4.2 deepen pass).
bool should_skip_pair_buffer_compaction(const PairBufferSoA& buffer);


    bool canPush() const { return !invalidPair && !atCapacity; }



    bool needsCompaction() const { return !emptyBuffer && !allValid; }



    bool needsClamp() const { return !emptyBuffer && !withinCapacity; }


    bool emptyBuffer = false;
    bool singlePair = false;

};


/// Non-mutating sort launch predicate — inverse of sort preflight skip (B4.2 deepen pass).



/// Non-mutating sort predicate — inverse of `canSkipPairBufferSort` (B4.2 deepen pass).

/// Non-mutating sort skip predicate — mirrors sort preflight inversion (B4.2 deepen pass).

    bool compactionNeeded = false;
    bool clampNeeded = false;

    bool canSkipAll() const { return emptyBuffer || (!compactionNeeded && !clampNeeded); }






/// Non-mutating sort skip predicate — inverse of `preflightPairBufferSort::needsSort` (B4.2 deepen follow-up pass).

/// Non-mutating compaction skip predicate — inverse of `preflightPairBufferCompaction::needsCompaction` (B4.2 deepen follow-up pass).
bool canSkipPairBufferCompaction(const PairBufferSoA& buffer);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.2 deepen follow-up pass).
    bool needsCompaction = false;

    bool canSkip() const { return emptyBuffer || (!needsCompaction && !needsClamp); }


/// Non-mutating sort skip predicate — inverse of `PairBufferSortPreflight::needsSort` (B4.2 deepen pass).
/// Read-only slot-write diagnostics — no mutation (B4.2 deepen follow-up pass).
    bool invalidPair = false;

    bool canWrite() const { return !outOfRangeSlot && !invalidPair; }

    const PairBufferSoA& buffer,
    u32 idxA,



/// Read-only slot-prepare diagnostics — no mutation (B4.2 deepen follow-up pass).
struct PairBufferPrepareSlotsPreflight {
    bool zeroSlots = false;

    bool canPrepare() const { return !zeroSlots; }

PairBufferPrepareSlotsPreflight preflightPairBufferPrepareSlots(u32 slotCount);

/// Read-only merge-into-buffer diagnostics — no mutation (B4.2 deepen follow-up pass).
struct PairBufferMergePreflight {
    bool emptyIncoming = false;
    bool atCapacity = false;
    u32 acceptedCount = 0;
    u32 rejectedCount = 0;

    bool canMergeAny() const { return !emptyIncoming && !atCapacity; }

PairBufferMergePreflight preflightPairBufferMerge(const PairBufferSoA& buffer, u32 incomingCount);

/// Merge preflight including pair-buffer capacity diagnostics (B4.2 deepen follow-up pass).
struct BroadphaseMergeIntoBufferPreflight {
    BroadphaseMergePreflight merge{};
    PairBufferMergePreflight buffer{};
    u32 incomingPairCount = 0;

    bool canMergeAny() const { return merge.canMerge() && buffer.canMergeAny(); }

BroadphaseMergeIntoBufferPreflight preflightBroadphaseMergeIntoBuffer(
    u32 incomingPairCount);

/// Non-mutating sort skip predicate — inverse of `PairBufferSortPreflight::needsSort`.

/// Returns true when `pairBufferSortRejectReason` matches `expected` (B4.2 deepen follow-up pass).




/// Non-mutating sort skip predicate — inverse of `needsSort` (B4.2 deepen follow-up pass).






/// Non-mutating sort skip predicate — inverse of `preflightPairBufferSort` (B4.2 deepen pass).



    PairBufferCompactionPreflight compaction{};
    PairBufferClampPreflight clamp{};

    bool needsWork() const { return compaction.needsCompaction() || clamp.needsClamp(); }


/// Non-mutating compact-and-clamp skip predicate — true when both sub-passes are no-ops (B4.2 deepen pass).
/// Read-only compact+clamp diagnostics — no mutation (B4.2 deepen pass).

    bool needsWork() const { return !emptyBuffer && (needsCompaction || needsClamp); }


/// Non-mutating compact+clamp skip predicate — true only when the buffer is empty (B4.2 deepen pass).

/// Non-mutating compaction skip predicate — mirrors `PairBufferSoA::canSkipCompaction` (B4.2 deepen pass).

/// Non-mutating clamp skip predicate — mirrors `PairBufferSoA::canSkipMaxCapacityClamp` (B4.2 deepen pass).
bool canSkipPairBufferClamp(const PairBufferSoA& buffer);

/// Non-mutating sort skip predicate — inverse of `preflightPairBufferSort::needsSort` (B4.2 deepen pass).


    bool noWorkNeeded = false;



/// Non-mutating compact+clamp skip predicate — inverse of `needsCompactAndClamp` (B4.2 deepen pass).

/// Non-mutating compact+clamp launch predicate — mirrors `preflightPairBufferCompactAndClamp` (B4.2 deepen pass).

/// Returns true when `pairBufferCompactAndClampRejectReason` matches `expected` (B4.2 deepen follow-up pass).

/// Read-only compact+clamp diagnostics — no mutation (B4.2 deepen follow-up pass).
    bool alreadyCompactAndWithinCapacity = false;



/// Non-mutating compact+clamp skip predicate — inverse of `needsCompactAndClamp` (B4.2 deepen follow-up pass).

/// Non-mutating compact+clamp predicate — mirrors `preflightPairBufferCompactAndClamp` (B4.2 deepen follow-up pass).

} // namespace fuse::physics::broadphase
