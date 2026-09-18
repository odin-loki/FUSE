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
    if (maxCapacity > 0u && activeCount > maxCapacity) {
        if (!isSortedByToi()) {
            sortByToi();
        }
        applyMaxCapacityClamp();
    }
}

void ToiBufferSoA::clear() {
    activeCount = 0;
    pairSlotCount = 0;
    droppedCount = 0;
}

void ToiBufferSoA::preparePairSlots(u32 pairCount) {
    if (pairCount == 0u) {
        clear();
        return;
    }

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
    if (slot >= pairSlotCount || !result.valid || !isToiInWindow(result.toi)) {
        return;
    }

    toiValues[slot] = result.toi;
    contactPoints[slot] = result.contactPoint;
    contactNormals[slot] = result.contactNormal;
    bodyA[slot] = result.bodyA;
    bodyB[slot] = result.bodyB;
    validFlags[slot] = 1u;
}

void ToiBufferSoA::invalidateSlot(u32 slot) {
    if (slot >= validFlags.size()) {
        return;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return;
    }
    validFlags[slot] = 0u;
}

bool ToiBufferSoA::slotIsValid(u32 slot) const {
    if (slot >= validFlags.size()) {
        return false;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return false;
    }
    return validFlags[slot] != 0u;
}

u32 ToiBufferSoA::countValidSlots() const {
    if (canSkipSoAIteration()) {
        return 0u;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 validCount = 0u;
    for (u32 slot = 0; slot < scanCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
        }
    }
    return validCount;
}

bool ToiBufferSoA::canSkipCompaction() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    if (scanCount == 0u) {
        return true;
    }

    for (u32 slot = 0; slot < scanCount; ++slot) {
        if (validFlags[slot] == 0u) {
            return false;
        }
    }
    return true;
}

bool ToiBufferSoA::canSkipSort() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    const u32 validCount = countValidSlots();
    if (validCount <= 1u) {
        return true;
    }

    return isSortedByToi();
}

bool ToiBufferSoA::canSkipCompactAndSort() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;
}

u32 ToiBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    }
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
}

bool ToiBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
}

bool ToiBufferSoA::push(const TOIResult& result) {
    if (!result.valid || !isToiInWindow(result.toi)) {
        return false;
    }

    if (isFull()) {
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
    if (canSkipSort()) {
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
    if (canSkipSoAIteration()) {
        return 0u;
    }

    if (pairSlotCount == 0u) {
        return activeCount;
    }

    const u32 validCount = countValidSlots();
    if (validCount == 0u) {
        activeCount = 0u;
        return activeCount;
    }

    if (canSkipCompaction()) {
        activeCount = pairSlotCount;
        return activeCount;
    }

    bool alreadyPacked = true;
    for (u32 i = 0u; i < validCount; ++i) {
        if (validFlags[i] == 0u) {
            alreadyPacked = false;
            break;
        }
    }
    if (alreadyPacked) {
        for (u32 i = validCount; i < pairSlotCount; ++i) {
            if (validFlags[i] != 0u) {
                alreadyPacked = false;
                break;
            }
        }
    }

    if (alreadyPacked) {
        activeCount = validCount;
        for (u32 i = activeCount; i < pairSlotCount; ++i) {
            validFlags[i] = 0u;
        }
        return activeCount;
    }

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

u32 ToiBufferSoA::applyMaxCapacityClamp() {
    if (!canApplyMaxCapacityClamp()) {
        return activeCount;
    }

    if (!isSortedByToi()) {
        sortByToi();
    }

    const u32 excess = activeCount - maxCapacity;
    droppedCount += excess;
    activeCount = maxCapacity;

    if (pairSlotCount == 0u) {
        toiValues.resize(activeCount);
        contactPoints.resize(activeCount);
        contactNormals.resize(activeCount);
        bodyA.resize(activeCount);
        bodyB.resize(activeCount);
        validFlags.resize(activeCount);
        return activeCount;
    }

    for (u32 i = activeCount; i < pairSlotCount; ++i) {
        validFlags[i] = 0u;
    }

    return activeCount;
}

u32 ToiBufferSoA::compactAndSort() {
    if (canSkipCompactAndSort()) {
        activeCount = 0u;
        return 0u;
    }

    compact();
    if (isEmpty()) {
        return 0u;
    }

    if (!isSortedByToi()) {
        sortByToi();
    }
    return applyMaxCapacityClamp();
}

bool ToiBufferSoA::isSortedByToi() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    const u32 validCount = countValidSlots();
    if (validCount <= 1u) {
        return true;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    f32 prevToi = 0.f;
    u32 prevBodyA = 0u;
    u32 prevBodyB = 0u;
    bool hasPrev = false;
    for (u32 i = 0; i < scanCount; ++i) {
        if (validFlags[i] == 0u) {
            continue;
        }

        const f32 currToi = toiValues[i];
        const u32 currBodyA = bodyA[i];
        const u32 currBodyB = bodyB[i];
        if (hasPrev) {
            if (currToi < prevToi) {
                return false;
            }
            if (currToi == prevToi) {
                if (currBodyA < prevBodyA) {
                    return false;
                }
                if (currBodyA == prevBodyA && currBodyB < prevBodyB) {
                    return false;
                }
            }
        }

        prevToi = currToi;
        prevBodyA = currBodyA;
        prevBodyB = currBodyB;
        hasPrev = true;
    }

    return true;
}

TOIResult ToiBufferSoA::earliestToi() const {
    if (canSkipSoAIteration() || isEmpty()) {
        return {};
    }
    return resultAt(0u);
}

TOIResult ToiBufferSoA::resultAt(u32 index) const {
    TOIResult result{};
    if (canSkipSoAIteration() || index >= activeCount || !slotIsValid(index)) {
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
    if (canSkipSoAIteration()) {
        return {};
    }

    std::vector<TOIResult> results;
    results.reserve(activeCount);
    for (u32 i = 0; i < activeCount; ++i) {
        if (slotIsValid(i)) {
            results.push_back(resultAt(i));
        }
    }
    return results;
}

} // namespace fuse::physics
