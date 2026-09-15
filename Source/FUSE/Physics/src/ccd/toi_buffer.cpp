#include <fuse/physics/ccd/toi_buffer.hpp>

namespace fuse::physics {

void ToiBufferSoA::reserve(u32 capacity) {
    toiValues.reserve(capacity);
    contactPoints.reserve(capacity);
    contactNormals.reserve(capacity);
    bodyA.reserve(capacity);
    bodyB.reserve(capacity);
    validFlags.reserve(capacity);
}

void ToiBufferSoA::clear() {
    activeCount = 0;
    pairSlotCount = 0;
}

void ToiBufferSoA::preparePairSlots(u32 pairCount) {
    pairSlotCount = pairCount;
    activeCount = 0;
    toiValues.assign(pairCount, 0.f);
    contactPoints.assign(pairCount, {});
    contactNormals.assign(pairCount, {});
    bodyA.assign(pairCount, 0u);
    bodyB.assign(pairCount, 0u);
    validFlags.assign(pairCount, 0u);
}

void ToiBufferSoA::writeSlot(u32 slot, const TOIResult& result) {
    if (slot >= pairSlotCount || !result.valid) {
        return;
    }

    toiValues[slot] = result.toi;
    contactPoints[slot] = result.contactPoint;
    contactNormals[slot] = result.contactNormal;
    bodyA[slot] = result.bodyA;
    bodyB[slot] = result.bodyB;
    validFlags[slot] = 1u;
}

u32 ToiBufferSoA::compact() {
    u32 writeIndex = 0;
    for (u32 readIndex = 0; readIndex < pairSlotCount; ++readIndex) {
        if (validFlags[readIndex] == 0u) {
            continue;
        }
        if (writeIndex != readIndex) {
            toiValues[writeIndex] = toiValues[readIndex];
            contactPoints[writeIndex] = contactPoints[readIndex];
            contactNormals[writeIndex] = contactNormals[readIndex];
            bodyA[writeIndex] = bodyA[readIndex];
            bodyB[writeIndex] = bodyB[readIndex];
            validFlags[writeIndex] = 1u;
        }
        ++writeIndex;
    }

    activeCount = writeIndex;
    for (u32 i = activeCount; i < pairSlotCount; ++i) {
        validFlags[i] = 0u;
    }
    return activeCount;
}

TOIResult ToiBufferSoA::resultAt(u32 index) const {
    TOIResult result{};
    if (index >= activeCount || validFlags[index] == 0u) {
        return result;
    }

    result.toi = toiValues[index];
    result.contactPoint = contactPoints[index];
    result.contactNormal = contactNormals[index];
    result.bodyA = bodyA[index];
    result.bodyB = bodyB[index];
    result.valid = true;
    return result;
}

std::vector<TOIResult> ToiBufferSoA::toVector() const {
    std::vector<TOIResult> results;
    results.reserve(activeCount);
    for (u32 i = 0; i < activeCount; ++i) {
        if (validFlags[i] != 0u) {
            results.push_back(resultAt(i));
        }
    }
    return results;
}

} // namespace fuse::physics
