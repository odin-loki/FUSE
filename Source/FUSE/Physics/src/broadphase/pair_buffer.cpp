#include <fuse/physics/broadphase/pair_buffer.hpp>

#include <algorithm>

namespace fuse::physics::broadphase {

namespace {

CandidatePair canonicalPair(u32 idxA, u32 idxB) {
    if (idxA > idxB) {
        std::swap(idxA, idxB);
    }
    return {idxA, idxB};
}

} // namespace

void PairBufferSoA::reserve(u32 capacity) {
    bodyA.reserve(capacity);
    bodyB.reserve(capacity);
    validFlags.reserve(capacity);
}

void PairBufferSoA::setMaxCapacity(u32 capacity) {
    maxCapacity = capacity;
}

void PairBufferSoA::clear() {
    activeCount = 0;
    pairSlotCount = 0;
    droppedCount = 0;
    bodyA.resize(0);
    bodyB.resize(0);
    validFlags.resize(0);
}

void PairBufferSoA::preparePairSlots(u32 slotCount) {
    pairSlotCount = slotCount;
    activeCount = 0;
    droppedCount = 0;
    bodyA.assign(slotCount, 0u);
    bodyB.assign(slotCount, 0u);
    validFlags.assign(slotCount, 0u);
}

void PairBufferSoA::writeSlot(u32 slot, u32 idxA, u32 idxB) {
    if (slot >= pairSlotCount || !isValidCandidatePair(idxA, idxB)) {
        return;
    }

    const CandidatePair pair = canonicalPair(idxA, idxB);
    bodyA[slot] = pair.bodyA;
    bodyB[slot] = pair.bodyB;
    validFlags[slot] = 1u;
}

void PairBufferSoA::invalidateSlot(u32 slot) {
    if (slot >= validFlags.size()) {
        return;
    }
    validFlags[slot] = 0u;
}

bool PairBufferSoA::push(u32 idxA, u32 idxB) {
    if (!isValidCandidatePair(idxA, idxB)) {
        return false;
    }

    if (maxCapacity > 0u && activeCount >= maxCapacity) {
        ++droppedCount;
        return false;
    }

    const CandidatePair pair = canonicalPair(idxA, idxB);
    bodyA.push_back(pair.bodyA);
    bodyB.push_back(pair.bodyB);
    validFlags.push_back(1u);
    ++activeCount;
    pairSlotCount = activeCount;
    return true;
}

u32 PairBufferSoA::compact() {
    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    if (scanCount == 0u) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }
    if (scanCount == 0u) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }

    u32 writeIndex = 0;
    for (u32 readIndex = 0; readIndex < scanCount; ++readIndex) {
        if (validFlags[readIndex] == 0u) {
            continue;
        }
        if (writeIndex != readIndex) {
            bodyA[writeIndex] = bodyA[readIndex];
            bodyB[writeIndex] = bodyB[readIndex];
            validFlags[writeIndex] = 1u;
        }
        ++writeIndex;
    }

    activeCount = writeIndex;
    pairSlotCount = activeCount;
    bodyA.resize(activeCount);
    bodyB.resize(activeCount);
    validFlags.resize(activeCount);
    return activeCount;
}

void PairBufferSoA::sortCanonical() {
    if (canSkipSoAIteration() || activeCount <= 1u) {
        return;
    }

    std::vector<u32> order(activeCount);
    for (u32 i = 0; i < activeCount; ++i) {
        order[i] = i;
    }

    std::sort(order.begin(), order.end(), [&](u32 lhs, u32 rhs) {
        if (bodyA[lhs] != bodyA[rhs]) {
            return bodyA[lhs] < bodyA[rhs];
        }
        return bodyB[lhs] < bodyB[rhs];
    });

    std::vector<u32> sortedBodyA(activeCount);
    std::vector<u32> sortedBodyB(activeCount);
    std::vector<u8> sortedValidFlags(activeCount);
    for (u32 i = 0; i < activeCount; ++i) {
        const u32 src = order[i];
        sortedBodyA[i] = bodyA[src];
        sortedBodyB[i] = bodyB[src];
        sortedValidFlags[i] = validFlags[src];
    }

    bodyA.swap(sortedBodyA);
    bodyB.swap(sortedBodyB);
    validFlags.swap(sortedValidFlags);
}

u32 PairBufferSoA::applyMaxCapacityClamp() {
    if (canSkipSoAIteration() || maxCapacity == 0u || activeCount <= maxCapacity) {
        return activeCount;
    }

    sortCanonical();

    const u32 excess = activeCount - maxCapacity;
    droppedCount += excess;
    activeCount = maxCapacity;
    pairSlotCount = activeCount;

    bodyA.resize(activeCount);
    bodyB.resize(activeCount);
    validFlags.resize(activeCount);
    return activeCount;
}

u32 PairBufferSoA::compactAndClamp() {
    compact();
    return applyMaxCapacityClamp();
}

bool PairBufferSoA::isSortedCanonical() const {
    if (canSkipSoAIteration() || activeCount <= 1u) {
        return true;
    }

    for (u32 i = 1; i < activeCount; ++i) {
        if (validFlags[i] == 0u || validFlags[i - 1u] == 0u) {
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

bool PairBufferSoA::containsCanonicalPair(u32 idxA, u32 idxB) const {
    if (canSkipSoAIteration() || !isValidCandidatePair(idxA, idxB)) {
        return false;
    }

    if (idxA > idxB) {
        std::swap(idxA, idxB);
    }
    for (u32 i = 0; i < activeCount; ++i) {
        if (validFlags[i] == 0u) {
            continue;
        }
        if (bodyA[i] == idxA && bodyB[i] == idxB) {
            return true;
        }
    }
    return false;
}

bool PairBufferSoA::slotIsValid(u32 slot) const {
    return slot < validFlags.size() && validFlags[slot] != 0u;
}

CandidatePair PairBufferSoA::pairAt(u32 index) const {
    if (canSkipSoAIteration() || index >= activeCount || !slotIsValid(index)) {
        return {};
    }
    return {bodyA[index], bodyB[index]};
}

std::vector<CandidatePair> PairBufferSoA::toVector() const {
    if (canSkipSoAIteration()) {
        return {};
    }

    std::vector<CandidatePair> pairs;
    pairs.reserve(activeCount);
    for (u32 i = 0; i < activeCount; ++i) {
        if (!slotIsValid(i)) {
            continue;
        }
        pairs.push_back(pairAt(i));
    }
    return pairs;
}

} // namespace fuse::physics::broadphase
