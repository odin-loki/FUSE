#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::broadphase {

/// SoA candidate-pair storage with clear/reuse for frame-to-frame broadphase output (B4.2 deepen).
struct PairBufferSoA {
    std::vector<u32> bodyA;
    std::vector<u32> bodyB;
    std::vector<u8> validFlags;

    u32 activeCount = 0;
    u32 pairSlotCount = 0;
    u32 maxCapacity = 0;
    u32 droppedCount = 0;

    bool isEmpty() const { return activeCount == 0u; }
    bool hasValidPairs() const { return activeCount > 0u; }
    /// True when clamping dropped one or more candidate pairs.
    bool hasDroppedPairs() const { return droppedCount > 0u; }
    /// True when `maxCapacity` is set and no additional pairs may be pushed.
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Pair-list guard: true when `additionalCount` pairs fit before `maxCapacity` clamp.
    bool canAcceptPairs(u32 additionalCount = 1u) const;
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;
    /// True when post-pass truncation would drop pairs.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.2 deepen follow-up).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when at most one canonical pair is present (dedupe is a no-op).
    bool canSkipDedupe() const { return canSkipSoAIteration() || activeCount <= 1u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;

    void reserve(u32 capacity);
    /// Reserve pair slots for `uniqueBodyCount` canonical pairs (B4.2 deepen follow-up).
    void reserveForUniqueBodies(u32 uniqueBodyCount);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 slotCount);
    void writeSlot(u32 slot, u32 idxA, u32 idxB);
    void invalidateSlot(u32 slot);
    bool push(u32 idxA, u32 idxB);
    u32 compact();
    void sortCanonical();
    u32 applyMaxCapacityClamp();
    u32 compactAndClamp();
    bool isSortedCanonical() const;
    bool containsCanonicalPair(u32 idxA, u32 idxB) const;
    CandidatePair pairAt(u32 index) const;
    std::vector<CandidatePair> toVector() const;
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

/// Read-only push diagnostics — no mutation (B4.2 deepen follow-up).
struct PairBufferPushPreflight {
    PairBufferPushRejectReason reason = PairBufferPushRejectReason::None;
    bool invalidPair = false;
    bool atCapacity = false;

    bool canPush() const { return reason == PairBufferPushRejectReason::None; }
};

PairBufferPushPreflight preflightPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB);

/// Why pair-buffer compaction would early-out (B4.2 deepen pass).
enum class PairBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for pair-buffer compaction reject reasons (logging / tests).
const char* pairBufferCompactionRejectReasonName(PairBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed.
PairBufferCompactionRejectReason pairBufferCompactionRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferCompactionRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferCompactionRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferCompactionRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.2 deepen follow-up).
struct PairBufferCompactionPreflight {
    PairBufferCompactionRejectReason reason = PairBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsCompaction() const { return reason == PairBufferCompactionRejectReason::None; }
};

PairBufferCompactionPreflight preflightPairBufferCompaction(const PairBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.2 deepen pass).
bool canSkipPairBufferCompaction(const PairBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflightPairBufferCompaction` (B4.2 deepen pass).
bool shouldRunPairBufferCompaction(const PairBufferSoA& buffer);

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
bool pairBufferClampRejectsForReason(const PairBufferSoA& buffer, PairBufferClampRejectReason expected);

/// Read-only max-capacity clamp diagnostics — no mutation (B4.2 deepen follow-up).
struct PairBufferClampPreflight {
    PairBufferClampRejectReason reason = PairBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needsClamp() const { return reason == PairBufferClampRejectReason::None; }
};

PairBufferClampPreflight preflightPairBufferClamp(const PairBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.2 deepen pass).
bool canSkipPairBufferClamp(const PairBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflightPairBufferClamp` (B4.2 deepen pass).
bool shouldRunPairBufferClamp(const PairBufferSoA& buffer);

/// Why pair-buffer dedupe would early-out at the SoA layer (B4.2 deepen pass).
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
bool pairBufferDedupeRejectsForReason(const PairBufferSoA& buffer, PairBufferDedupeRejectReason expected);

/// Read-only dedupe diagnostics at the SoA layer — no mutation (B4.2 deepen follow-up pass).
struct PairBufferDedupePreflight {
    PairBufferDedupeRejectReason reason = PairBufferDedupeRejectReason::None;
    bool emptyBuffer = false;
    bool singlePair = false;

    bool canDedupe() const { return reason == PairBufferDedupeRejectReason::None; }
};

PairBufferDedupePreflight preflightPairBufferDedupe(const PairBufferSoA& buffer);

/// Non-mutating dedupe skip predicate — mirrors `PairBufferSoA::canSkipDedupe` (B4.2 deepen follow-up pass).
bool canSkipPairBufferDedupe(const PairBufferSoA& buffer);

/// Non-mutating dedupe predicate — inverse of `canSkipPairBufferDedupe` (B4.2 deepen pass).
bool shouldRunPairBufferDedupe(const PairBufferSoA& buffer);

/// Why pair-buffer canonical sort would early-out (B4.2 deepen pass).
enum class PairBufferSortRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    SinglePair,
};

/// Human-readable label for pair-buffer sort reject reasons (logging / tests).
const char* pairBufferSortRejectReasonName(PairBufferSortRejectReason reason);

/// Diagnose why sort would skip; vacuously succeeds when sort may proceed.
PairBufferSortRejectReason pairBufferSortRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferSortRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferSortRejectsForReason(const PairBufferSoA& buffer, PairBufferSortRejectReason expected);

/// Read-only canonical-sort diagnostics — no mutation (B4.2 deepen follow-up pass).
struct PairBufferSortPreflight {
    PairBufferSortRejectReason reason = PairBufferSortRejectReason::None;
    bool emptyBuffer = false;
    bool singlePair = false;

    bool needsSort() const { return reason == PairBufferSortRejectReason::None; }
};

PairBufferSortPreflight preflightPairBufferSort(const PairBufferSoA& buffer);

/// Non-mutating sort skip predicate — inverse of `needsSort` (B4.2 deepen pass).
bool canSkipPairBufferSort(const PairBufferSoA& buffer);

/// Non-mutating sort predicate — mirrors `preflightPairBufferSort` (B4.2 deepen pass).
bool shouldRunPairBufferSort(const PairBufferSoA& buffer);

/// Why pair-buffer compact-and-clamp would early-out (B4.2 deepen pass).
enum class PairBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for compact-and-clamp reject reasons (logging / tests).
const char* pairBufferCompactAndClampRejectReasonName(PairBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed.
PairBufferCompactAndClampRejectReason pairBufferCompactAndClampRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferCompactAndClampRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferCompactAndClampRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.2 deepen pass).
struct PairBufferCompactAndClampPreflight {
    PairBufferCompactAndClampRejectReason reason = PairBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompactAndClamp() const { return reason == PairBufferCompactAndClampRejectReason::None; }
};

PairBufferCompactAndClampPreflight preflightPairBufferCompactAndClamp(const PairBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.2 deepen pass).
bool canSkipPairBufferCompactAndClamp(const PairBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflightPairBufferCompactAndClamp` (B4.2 deepen pass).
bool shouldRunPairBufferCompactAndClamp(const PairBufferSoA& buffer);

/// Why pair-buffer slot write would reject (B4.2 deepen follow-up pass).
enum class PairBufferWriteSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidPair,
};

/// Human-readable label for pair-buffer write-slot reject reasons (logging / tests).
const char* pairBufferWriteSlotRejectReasonName(PairBufferWriteSlotRejectReason reason);

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed.
PairBufferWriteSlotRejectReason pairBufferWriteSlotRejectReason(
    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,
    u32 idxB);

/// Returns true when `pairBufferWriteSlotRejectReason` matches `expected` (B4.2 deepen follow-up pass).
bool pairBufferWriteSlotRejectsForReason(
    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,
    u32 idxB,
    PairBufferWriteSlotRejectReason expected);

/// Read-only write-slot diagnostics — no mutation (B4.2 deepen follow-up pass).
struct PairBufferWriteSlotPreflight {
    PairBufferWriteSlotRejectReason reason = PairBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidPair = false;

    bool canWrite() const { return reason == PairBufferWriteSlotRejectReason::None; }
};

PairBufferWriteSlotPreflight preflightPairBufferWriteSlot(
    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,
    u32 idxB);

/// Non-mutating write-slot skip predicate — inverse of `canWrite` (B4.2 deepen follow-up pass).
bool canSkipPairBufferWriteSlot(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB);

/// Non-mutating write-slot predicate — mirrors `preflightPairBufferWriteSlot` (B4.2 deepen follow-up pass).
bool shouldRunPairBufferWriteSlot(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB);

/// Early-out when write-slot preflight would reject — same ordering as `canSkipPairBufferWriteSlot` (B4.2 deepen pass).
bool wouldSkipPairBufferWriteSlot(
    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,
    u32 idxB,
    PairBufferWriteSlotRejectReason* reason = nullptr);

/// Why pair-buffer slot invalidate would early-out (B4.2 deepen pass).
enum class PairBufferInvalidateSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    AlreadyInvalid,
};

/// Human-readable label for pair-buffer invalidate-slot reject reasons (logging / tests).
const char* pairBufferInvalidateSlotRejectReasonName(PairBufferInvalidateSlotRejectReason reason);

/// Diagnose why invalidateSlot would skip; vacuously succeeds when invalidate may proceed.
PairBufferInvalidateSlotRejectReason pairBufferInvalidateSlotRejectReason(const PairBufferSoA& buffer, u32 slot);

/// Returns true when `pairBufferInvalidateSlotRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferInvalidateSlotRejectsForReason(
    const PairBufferSoA& buffer,
    u32 slot,
    PairBufferInvalidateSlotRejectReason expected);

/// Read-only invalidate-slot diagnostics — no mutation (B4.2 deepen pass).
struct PairBufferInvalidateSlotPreflight {
    PairBufferInvalidateSlotRejectReason reason = PairBufferInvalidateSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool alreadyInvalid = false;

    bool canInvalidate() const { return reason == PairBufferInvalidateSlotRejectReason::None; }
};

PairBufferInvalidateSlotPreflight preflightPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot);

/// Non-mutating invalidate-slot skip predicate — inverse of `canInvalidate` (B4.2 deepen pass).
bool canSkipPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot);

/// Non-mutating invalidate-slot predicate — mirrors `preflightPairBufferInvalidateSlot` (B4.2 deepen pass).
bool shouldRunPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot);

/// Early-out when invalidate-slot preflight would reject — same ordering as `canSkipPairBufferInvalidateSlot` (B4.2 deepen pass).
bool wouldSkipPairBufferInvalidateSlot(
    const PairBufferSoA& buffer,
    u32 slot,
    PairBufferInvalidateSlotRejectReason* reason = nullptr);

/// Why pair-buffer toVector would early-out (B4.2 deepen follow-up pass).
enum class PairBufferToVectorRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

/// Human-readable label for pair-buffer toVector reject reasons (logging / tests).
const char* pairBufferToVectorRejectReasonName(PairBufferToVectorRejectReason reason);

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed.
PairBufferToVectorRejectReason pairBufferToVectorRejectReason(const PairBufferSoA& buffer);

/// Returns true when `pairBufferToVectorRejectReason` matches `expected` (B4.2 deepen follow-up pass).
bool pairBufferToVectorRejectsForReason(const PairBufferSoA& buffer, PairBufferToVectorRejectReason expected);

/// Read-only toVector diagnostics — no mutation (B4.2 deepen follow-up pass).
struct PairBufferToVectorPreflight {
    PairBufferToVectorRejectReason reason = PairBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool canExport() const { return reason == PairBufferToVectorRejectReason::None; }
};

PairBufferToVectorPreflight preflightPairBufferToVector(const PairBufferSoA& buffer);

/// Non-mutating toVector skip predicate — inverse of `canExport` (B4.2 deepen follow-up pass).
bool canSkipPairBufferToVector(const PairBufferSoA& buffer);

/// Non-mutating toVector predicate — mirrors `preflightPairBufferToVector` (B4.2 deepen follow-up pass).
bool shouldRunPairBufferToVector(const PairBufferSoA& buffer);

/// Why pair-buffer slot invalidation would reject (B4.2 deepen pass).
enum class PairBufferInvalidateSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
};

/// Human-readable label for pair-buffer invalidate-slot reject reasons (logging / tests).
const char* pairBufferInvalidateSlotRejectReasonName(PairBufferInvalidateSlotRejectReason reason);

/// Diagnose why invalidateSlot would reject; vacuously succeeds when invalidation may proceed.
PairBufferInvalidateSlotRejectReason pairBufferInvalidateSlotRejectReason(const PairBufferSoA& buffer, u32 slot);

/// Returns true when `pairBufferInvalidateSlotRejectReason` matches `expected` (B4.2 deepen pass).
bool pairBufferInvalidateSlotRejectsForReason(
    const PairBufferSoA& buffer,
    u32 slot,
    PairBufferInvalidateSlotRejectReason expected);

/// Read-only invalidate-slot diagnostics — no mutation (B4.2 deepen pass).
struct PairBufferInvalidateSlotPreflight {
    PairBufferInvalidateSlotRejectReason reason = PairBufferInvalidateSlotRejectReason::None;
    bool outOfRangeSlot = false;

    bool canInvalidate() const { return reason == PairBufferInvalidateSlotRejectReason::None; }
};

PairBufferInvalidateSlotPreflight preflightPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot);

/// Non-mutating invalidate-slot skip predicate — inverse of `canInvalidate` (B4.2 deepen pass).
bool canSkipPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot);

/// Non-mutating invalidate-slot predicate — mirrors `preflightPairBufferInvalidateSlot` (B4.2 deepen pass).
bool shouldRunPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot);

} // namespace fuse::physics::broadphase

// --- deepen additive from deepen-b4-broadphase-guards-fcb2 ---
    CandidateRejectReason lastRejectReason = CandidateRejectReason::None;

// --- deepen additive from deepen-b4-broadphase-guards-cd2f ---
    bool wouldRejectPush(u32 idxA, u32 idxB, u32 bodyCount = 0u) const;

// --- deepen additive from deepen-b4-broadphase-guards-b3ad ---
    CandidatePairRejectReason lastRejectReason = CandidatePairRejectReason::None;

// --- deepen additive from deepen-b4-broadphase-guards-bcce ---
    bool wouldRejectAdditionalPairs(u32 additionalCount = 1u) const;

// --- deepen additive from deepen-b4-broadphase-preflights-82c3 ---
    struct DedupePreflight {
    DedupePreflight preflight_dedupe() const;

// --- deepen additive from deepen-b4-broadphase-preflights-ec03 ---
PairBufferDedupePreflight preflight_dedupe_pair_buffer(const PairBufferSoA& buffer);
bool should_skip_dedupe_pair_buffer(const PairBufferSoA& buffer);
PairBufferClampPreflight preflight_pair_buffer_clamp(const PairBufferSoA& buffer);

// --- deepen additive from b4-broadphase-deepen-guards-1b87 ---
struct PairSlotPreflight {
PairSlotPreflight preflightPairSlots(u32 slotCount, const PairBufferSoA& buffer);

// --- deepen additive from deepen-b4-broadphase-preflights-76e1 ---
PairBufferDedupePreflight preflight_pair_buffer_dedupe(const PairBufferSoA& buffer);
bool should_skip_pair_buffer_dedupe(const PairBufferSoA& buffer);

// --- deepen additive from deepen-b4-broadphase-preflights-4247 ---
struct PairBufferPreflight {
PairBufferPreflight preflight_pair_buffer(const PairBufferSoA& buffer);
bool should_skip_pair_buffer_compaction(const PairBufferSoA& buffer);

// --- deepen additive from deepen-b4-broadphase-guards-bd20 ---
bool pairBufferCompactionRejectsForReason(const PairBufferSoA& buffer, PairBufferCompactionRejectReason expected);

// --- deepen additive from deepen-b4-broadphase-guards-7162 ---
struct PairBufferPrepareSlotsPreflight {
PairBufferPrepareSlotsPreflight preflightPairBufferPrepareSlots(u32 slotCount);
struct PairBufferMergePreflight {
PairBufferMergePreflight preflightPairBufferMerge(const PairBufferSoA& buffer, u32 incomingCount);
struct BroadphaseMergeIntoBufferPreflight {
    BroadphaseMergePreflight merge{};
    PairBufferMergePreflight buffer{};
BroadphaseMergeIntoBufferPreflight preflightBroadphaseMergeIntoBuffer(

// --- deepen additive from deepen-b4-broadphase-guards-9072 ---
    bool wouldTruncate = false;
PairBufferMergePreflight preflightPairBufferMerge(const PairBufferSoA& buffer, u32 incomingPairCount);

// --- deepen additive from deepen-b4-broadphase-guards-cbb3 ---
    PairBufferCompactionPreflight compaction{};
    PairBufferClampPreflight clamp{};

// --- deepen additive from deepen-b4-broadphase-guards-b64e ---
enum class PairBufferSlotWriteRejectReason : u8 {
const char* pairBufferSlotWriteRejectReasonName(PairBufferSlotWriteRejectReason reason);
PairBufferSlotWriteRejectReason pairBufferSlotWriteRejectReason(
    PairBufferSlotWriteRejectReason expected);
struct PairBufferSlotWritePreflight {
    PairBufferSlotWriteRejectReason reason = PairBufferSlotWriteRejectReason::None;
    bool canWrite() const { return reason == PairBufferSlotWriteRejectReason::None; }
PairBufferSlotWritePreflight preflightPairBufferSlotWrite(
enum class PairBufferSlotReservationRejectReason : u8 {
const char* pairBufferSlotReservationRejectReasonName(PairBufferSlotReservationRejectReason reason);
PairBufferSlotReservationRejectReason pairBufferSlotReservationRejectReason(
    PairBufferSlotReservationRejectReason expected);
struct PairBufferSlotReservationPreflight {
    PairBufferSlotReservationRejectReason reason = PairBufferSlotReservationRejectReason::None;
    bool canReserve() const { return reason == PairBufferSlotReservationRejectReason::None; }
PairBufferSlotReservationPreflight preflightPairBufferSlotReservation(

// --- deepen additive from deepen-b4-broadphase-guards-c372 ---
    bool needsWork() const { return reason == PairBufferCompactAndClampRejectReason::None; }
