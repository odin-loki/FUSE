#include <fuse/physics/broadphase/pair_buffer.hpp>

#include <algorithm>
#include <unordered_set>

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

u32 PairBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    }
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
}

bool PairBufferSoA::canAcceptPairs(u32 additionalCount) const {
    if (additionalCount == 0u) {
        return true;
    }
    if (maxCapacity == 0u) {
        return true;
    }
    return activeCount + additionalCount <= maxCapacity;
}

bool PairBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
}

bool PairBufferSoA::push(u32 idxA, u32 idxB) {
    if (!isValidCandidatePair(idxA, idxB)) {
        return false;
    }

    if (isFull()) {
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
    if (canSkipSoAIteration()) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }

    if (canSkipCompaction()) {
        activeCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
        pairSlotCount = activeCount;
        bodyA.resize(activeCount);
        bodyB.resize(activeCount);
        validFlags.resize(activeCount);
        return activeCount;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;

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

bool PairBufferSoA::hasDuplicateCanonicalPairs() const {
    if (canSkipDedupe()) {
        return false;
    }

    std::unordered_set<u64> seen;
    seen.reserve(activeCount * 2 + 1);
    for (u32 slot = 0; slot < activeCount; ++slot) {
        if (!slotIsValid(slot)) {
            continue;
        }
        const u64 key = (static_cast<u64>(bodyA[slot]) << 32) | bodyB[slot];
        if (!seen.insert(key).second) {
            return true;
        }
    }
    return false;
}

PairBufferSoA::DedupePreflight PairBufferSoA::preflight_dedupe() const {
    DedupePreflight preflight{};
    if (canSkipDedupe()) {
        preflight.skipped = true;
        return preflight;
    }

    std::unordered_set<u64> seen;
    seen.reserve(activeCount * 2 + 1);
    for (u32 slot = 0; slot < activeCount; ++slot) {
        if (!slotIsValid(slot)) {
            continue;
        }
        const u64 key = (static_cast<u64>(bodyA[slot]) << 32) | bodyB[slot];
        if (!seen.insert(key).second) {
            ++preflight.duplicateCount;
        }
    }
    return preflight;
}

bool PairBufferSoA::needsDedupe() const {
    return preflight_dedupe().needs_dedupe();
}

void PairBufferSoA::sortCanonical() {
    if (canSkipDedupe()) {
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
    if (!canApplyMaxCapacityClamp()) {
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

u32 PairBufferSoA::countValidSlots() const {
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

bool PairBufferSoA::canSkipCompaction() const {
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
