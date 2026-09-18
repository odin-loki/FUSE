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
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when at most one canonical pair is present (dedupe is a no-op).
    bool canSkipDedupe() const { return canSkipSoAIteration() || activeCount <= 1u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// True when dedupe followed by clamp would be a no-op (B4.2 deepen pass).
    bool canSkipDedupeAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;

    void reserve(u32 capacity);
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

/// Empty-output guard: true when broadphase produced no active pairs (B4.2 deepen pass).
FUSE_PHYSICS_INLINE bool isEmptyBroadphaseOutput(const PairBufferSoA& buffer) {
    return buffer.isEmpty() || buffer.canSkipSoAIteration();
}

/// Const preflight for pair-buffer dedupe dispatch (B4.2 deepen pass).
struct PairBufferDedupePreflight {
    u32 activePairCount = 0;
    bool skipped = false;

    bool needs_dedupe() const { return !skipped && activePairCount > 1u; }
    bool can_dedupe() const { return needs_dedupe(); }
};

/// Populate dedupe preflight without sorting pair slots (B4.2 deepen pass).
PairBufferDedupePreflight preflight_dedupe_pair_buffer(const PairBufferSoA& buffer);

/// Returns true when dedupe should skip before sort/unique pass (B4.2 deepen pass).
bool should_skip_dedupe_pair_buffer(const PairBufferSoA& buffer);

/// Const preflight for pair-buffer capacity clamp (B4.2 deepen pass).
struct PairBufferClampPreflight {
    u32 activePairCount = 0;
    u32 maxCapacity = 0;
    u32 excessCount = 0;
    bool skipped = false;

    bool needs_clamp() const { return !skipped && maxCapacity > 0u && activePairCount > maxCapacity; }
    bool can_clamp() const { return needs_clamp(); }
};

/// Populate clamp preflight without truncating pair slots (B4.2 deepen pass).
PairBufferClampPreflight preflight_pair_buffer_clamp(const PairBufferSoA& buffer);

} // namespace fuse::physics::broadphase
