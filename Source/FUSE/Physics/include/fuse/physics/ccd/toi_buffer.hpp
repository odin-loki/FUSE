#pragma once

#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// SoA TOI storage with clear/reuse for frame-to-frame CCD output (B4.6 deepen).
struct ToiBufferSoA {
    std::vector<f32> toiValues;
    std::vector<vec3> contactPoints;
    std::vector<vec3> contactNormals;
    std::vector<u32> bodyA;
    std::vector<u32> bodyB;
    std::vector<u8> validFlags;

    u32 activeCount = 0;
    u32 pairSlotCount = 0;
    u32 maxCapacity = 0;
    u32 droppedCount = 0;

    bool isEmpty() const { return activeCount == 0u; }
    bool hasValidTois() const { return activeCount > 0u; }
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when at most one valid TOI is present (sort is a no-op).
    bool canSkipSort() const { return canSkipSoAIteration() || activeCount <= 1u || isSortedByToi(); }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// True when compactAndSort has no valid TOIs to gather or sort.
    bool canSkipCompactAndSort() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    /// True when `maxCapacity` is set and no additional TOIs may be pushed.
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;
    /// True when post-pass truncation would drop TOIs.
    bool canApplyMaxCapacityClamp() const;
    /// True when `slot` lies within prepared pair slots or dense push storage.
    bool isPreparedSlot(u32 slot) const;
    bool slotIsValid(u32 slot) const;

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const TOIResult& result);
    void invalidateSlot(u32 slot);
    bool push(const TOIResult& result);
    void sortByToi();
    u32 compact();
    u32 applyMaxCapacityClamp();
    u32 compactAndSort();
    bool isSortedByToi() const;
    TOIResult earliestToi() const;
    TOIResult resultAt(u32 index) const;
    std::vector<TOIResult> toVector() const;
};

} // namespace fuse::physics
