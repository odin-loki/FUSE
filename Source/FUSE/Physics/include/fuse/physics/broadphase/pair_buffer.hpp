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
    CandidatePairRejectReason lastRejectReason = CandidatePairRejectReason::None;

    bool isEmpty() const { return activeCount == 0u; }
    bool hasValidPairs() const { return activeCount > 0u; }
    /// True when `maxCapacity` is set and no additional pairs may be pushed.
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;
    /// True when post-pass truncation would drop pairs.
    bool canApplyMaxCapacityClamp() const;
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when AABB refine can be skipped (no pairs to test).
    bool canSkipRefine() const { return canSkipSoAIteration(); }
    /// True when at most one canonical pair is present (dedupe is a no-op).
    bool canSkipDedupe() const { return canSkipSoAIteration() || activeCount <= 1u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    /// True when any prepared slot has been invalidated.
    bool hasInvalidSlots() const;
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

} // namespace fuse::physics::broadphase
