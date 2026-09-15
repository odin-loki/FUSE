#include <fuse/physics/ccd/toi_buffer.hpp>

#include <algorithm>

namespace fuse::physics {

void ToiBufferSoA::reserve(u32 capacity) {
    toiValues.reserve(capacity);
    contactPoints.reserve(capacity);
    contactNormals.reserve(capacity);
    bodyA.reserve(capacity);
    bodyB.reserve(capacity);
    validFlags.reserve(capacity);
}

void ToiBufferSoA::setMaxCapacity(u32 capacity) {
    maxCapacity = capacity;
}

void ToiBufferSoA::clear() {
    activeCount = 0;
    pairSlotCount = 0;
    droppedCount = 0;
}

void ToiBufferSoA::preparePairSlots(u32 pairCount) {
    pairSlotCount = pairCount;
    activeCount = 0;
    droppedCount = 0;
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

bool ToiBufferSoA::push(const TOIResult& result) {
    if (!result.valid) {
        return false;
    }

    if (maxCapacity > 0u && activeCount >= maxCapacity) {
        ++droppedCount;
        return false;
    }

    toiValues.push_back(result.toi);
    contactPoints.push_back(result.contactPoint);
    contactNormals.push_back(result.contactNormal);
    bodyA.push_back(result.bodyA);
    bodyB.push_back(result.bodyB);
    validFlags.push_back(1u);
    ++activeCount;
    return true;
}

void ToiBufferSoA::sortByToi() {
    if (activeCount <= 1u) {
        return;
    }

    std::vector<u32> order(activeCount);
    for (u32 i = 0; i < activeCount; ++i) {
        order[i] = i;
    }

    std::sort(order.begin(), order.end(), [&](u32 lhs, u32 rhs) {
        if (toiValues[lhs] != toiValues[rhs]) {
            return toiValues[lhs] < toiValues[rhs];
        }
        if (bodyA[lhs] != bodyA[rhs]) {
            return bodyA[lhs] < bodyA[rhs];
        }
        return bodyB[lhs] < bodyB[rhs];
    });

    const auto reorder = [&](auto& values) {
        using Value = typename std::decay_t<decltype(values)>::value_type;
        std::vector<Value> sorted(activeCount);
        for (u32 i = 0; i < activeCount; ++i) {
            sorted[i] = values[order[i]];
        }
        for (u32 i = 0; i < activeCount; ++i) {
            values[i] = sorted[i];
        }
    };

    reorder(toiValues);
    reorder(contactPoints);
    reorder(contactNormals);
    reorder(bodyA);
    reorder(bodyB);
    reorder(validFlags);
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

u32 ToiBufferSoA::compactAndSort() {
    const u32 count = compact();
    sortByToi();
    return count;
}

bool ToiBufferSoA::isSortedByToi() const {
    if (activeCount <= 1u) {
        return true;
    }

    for (u32 i = 1; i < activeCount; ++i) {
        if (validFlags[i] == 0u || validFlags[i - 1u] == 0u) {
            continue;
        }

        const f32 prevToi = toiValues[i - 1u];
        const f32 currToi = toiValues[i];
        if (currToi < prevToi) {
            return false;
        }
        if (currToi != prevToi) {
            continue;
        }

        const u32 prevBodyA = bodyA[i - 1u];
        const u32 currBodyA = bodyA[i];
        if (currBodyA < prevBodyA) {
            return false;
        }
        if (currBodyA != prevBodyA) {
            continue;
        }

        if (bodyB[i] < bodyB[i - 1u]) {
            return false;
        }
    }

    return true;
}

TOIResult ToiBufferSoA::earliestToi() const {
    return resultAt(0u);
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
