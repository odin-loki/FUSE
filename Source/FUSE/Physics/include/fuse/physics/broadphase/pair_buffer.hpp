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

/// Why a pair-buffer push would be rejected (B4.2 deepen pass).
enum class PairBufferPushRejectReason : u8 {
    None = 0,
    InvalidPair,
    AtCapacity,
};

/// Human-readable label for pair-buffer push reject reasons (logging / tests).
const char* pairBufferPushRejectReasonName(PairBufferPushRejectReason reason);

/// Diagnose why push would fail; vacuously succeeds when push may proceed.
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

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed.
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

/// Non-mutating dedupe launch predicate — inverse of `canSkipPairBufferDedupe` (B4.2 deepen pass).
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

/// Non-mutating sort launch predicate — inverse of sort preflight skip (B4.2 deepen pass).
bool shouldRunPairBufferSort(const PairBufferSoA& buffer);

} // namespace fuse::physics::broadphase
