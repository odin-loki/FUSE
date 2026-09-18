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

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    bool writeSlot(u32 slot, const TOIResult& result);
    void invalidateSlot(u32 slot);
    bool push(const TOIResult& result);
    void sortByToi();
    void sortIfNeeded();
    u32 compact();
    u32 compactIfNeeded();
    u32 applyMaxCapacityClamp();
    u32 compactAndSort();
    bool isEmpty() const { return activeCount == 0u; }
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    bool hasValidTois() const { return activeCount > 0u; }
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    bool slotIsValid(u32 slot) const;
    bool needsCompact() const;
    bool isSortedByToi() const;
    TOIResult earliestToi() const;
    TOIResult resultAt(u32 index) const;
    TOIResult resultAtSlot(u32 slot) const;
    std::vector<TOIResult> toVector() const;
};

} // namespace fuse::physics
