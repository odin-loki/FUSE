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
    void writeSlot(u32 slot, const TOIResult& result);
    bool push(const TOIResult& result);
    void sortByToi();
    u32 compact();
    u32 applyMaxCapacityClamp();
    u32 compactAndSort();
    bool isEmpty() const { return activeCount == 0u; }
    bool isSortedByToi() const;
    TOIResult earliestToi() const;
    TOIResult resultAt(u32 index) const;
    std::vector<TOIResult> toVector() const;
};

} // namespace fuse::physics
