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

void PairBufferSoA::reserveForUniqueBodies(u32 uniqueBodyCount) {
    reserve(estimatePairCountForUniqueBodies(uniqueBodyCount));
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
    const PairBufferPushPreflight preflight = preflightPairBufferPush(*this, idxA, idxB);
    if (!preflight.canPush()) {
        if (preflight.reason == PairBufferPushRejectReason::AtCapacity) {
            ++droppedCount;
        }
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
    const PairBufferCompactionPreflight preflight = preflightPairBufferCompaction(*this);
    if (preflight.reason == PairBufferCompactionRejectReason::EmptyBuffer) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }

    if (preflight.reason == PairBufferCompactionRejectReason::AllValid) {
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

void PairBufferSoA::sortCanonical() {
    if (!preflightPairBufferSort(*this).needsSort()) {
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
    if (!preflightPairBufferClamp(*this).needsClamp()) {
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

const char* pairBufferPushRejectReasonName(PairBufferPushRejectReason reason) {
    switch (reason) {
    case PairBufferPushRejectReason::None:
        return "None";
    case PairBufferPushRejectReason::InvalidPair:
        return "InvalidPair";
    case PairBufferPushRejectReason::AtCapacity:
        return "AtCapacity";
    }
    return "Unknown";
}

PairBufferPushRejectReason pairBufferPushRejectReason(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    if (!isValidCandidatePair(idxA, idxB)) {
        return PairBufferPushRejectReason::InvalidPair;
    }
    if (buffer.isFull()) {
        return PairBufferPushRejectReason::AtCapacity;
    }
    return PairBufferPushRejectReason::None;
}

bool pairBufferPushRejectsForReason(
    const PairBufferSoA& buffer,
    u32 idxA,
    u32 idxB,
    PairBufferPushRejectReason expected) {
    return pairBufferPushRejectReason(buffer, idxA, idxB) == expected;
}

const char* pairBufferCompactionRejectReasonName(PairBufferCompactionRejectReason reason) {
    switch (reason) {
    case PairBufferCompactionRejectReason::None:
        return "None";
    case PairBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferCompactionRejectReason::AllValid:
        return "AllValid";
    }
    return "Unknown";
}

PairBufferCompactionRejectReason pairBufferCompactionRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferCompactionRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipCompaction()) {
        return PairBufferCompactionRejectReason::AllValid;
    }
    return PairBufferCompactionRejectReason::None;
}

bool pairBufferCompactionRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferCompactionRejectReason expected) {
    return pairBufferCompactionRejectReason(buffer) == expected;
}

const char* pairBufferClampRejectReasonName(PairBufferClampRejectReason reason) {
    switch (reason) {
    case PairBufferClampRejectReason::None:
        return "None";
    case PairBufferClampRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";
    }
    return "Unknown";
}

PairBufferClampRejectReason pairBufferClampRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferClampRejectReason::EmptyBuffer;
    }
    if (!buffer.canApplyMaxCapacityClamp()) {
        return PairBufferClampRejectReason::WithinCapacity;
    }
    return PairBufferClampRejectReason::None;
}

bool pairBufferClampRejectsForReason(const PairBufferSoA& buffer, PairBufferClampRejectReason expected) {
    return pairBufferClampRejectReason(buffer) == expected;
}

const char* pairBufferDedupeRejectReasonName(PairBufferDedupeRejectReason reason) {
    switch (reason) {
    case PairBufferDedupeRejectReason::None:
        return "None";
    case PairBufferDedupeRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferDedupeRejectReason::SinglePair:
        return "SinglePair";
    }
    return "Unknown";
}

PairBufferDedupeRejectReason pairBufferDedupeRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferDedupeRejectReason::EmptyBuffer;
    }
    if (buffer.activeCount <= 1u) {
        return PairBufferDedupeRejectReason::SinglePair;
    }
    return PairBufferDedupeRejectReason::None;
}

bool pairBufferDedupeRejectsForReason(const PairBufferSoA& buffer, PairBufferDedupeRejectReason expected) {
    return pairBufferDedupeRejectReason(buffer) == expected;
}

const char* pairBufferSortRejectReasonName(PairBufferSortRejectReason reason) {
    switch (reason) {
    case PairBufferSortRejectReason::None:
        return "None";
    case PairBufferSortRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferSortRejectReason::SinglePair:
        return "SinglePair";
    }
    return "Unknown";
}

PairBufferSortRejectReason pairBufferSortRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferSortRejectReason::EmptyBuffer;
    }
    if (buffer.activeCount <= 1u) {
        return PairBufferSortRejectReason::SinglePair;
    }
    return PairBufferSortRejectReason::None;
}

bool pairBufferSortRejectsForReason(const PairBufferSoA& buffer, PairBufferSortRejectReason expected) {
    return pairBufferSortRejectReason(buffer) == expected;
}

PairBufferPushPreflight preflightPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    PairBufferPushPreflight preflight{};
    preflight.reason = pairBufferPushRejectReason(buffer, idxA, idxB);
    preflight.invalidPair = preflight.reason == PairBufferPushRejectReason::InvalidPair;
    preflight.atCapacity = preflight.reason == PairBufferPushRejectReason::AtCapacity;
    return preflight;
}

PairBufferCompactionPreflight preflightPairBufferCompaction(const PairBufferSoA& buffer) {
    PairBufferCompactionPreflight preflight{};
    preflight.reason = pairBufferCompactionRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == PairBufferCompactionRejectReason::AllValid;
    return preflight;
}

PairBufferClampPreflight preflightPairBufferClamp(const PairBufferSoA& buffer) {
    PairBufferClampPreflight preflight{};
    preflight.reason = pairBufferClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == PairBufferClampRejectReason::WithinCapacity;
    return preflight;
}

PairBufferDedupePreflight preflightPairBufferDedupe(const PairBufferSoA& buffer) {
    PairBufferDedupePreflight preflight{};
    preflight.reason = pairBufferDedupeRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferDedupeRejectReason::EmptyBuffer;
    preflight.singlePair = preflight.reason == PairBufferDedupeRejectReason::SinglePair;
    return preflight;
}

bool canSkipPairBufferDedupe(const PairBufferSoA& buffer) {
    return !preflightPairBufferDedupe(buffer).canDedupe();
}

bool shouldRunPairBufferDedupe(const PairBufferSoA& buffer) {
    return preflightPairBufferDedupe(buffer).canDedupe();
}

PairBufferSortPreflight preflightPairBufferSort(const PairBufferSoA& buffer) {
    PairBufferSortPreflight preflight{};
    preflight.reason = pairBufferSortRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferSortRejectReason::EmptyBuffer;
    preflight.singlePair = preflight.reason == PairBufferSortRejectReason::SinglePair;
    return preflight;
}

bool shouldRunPairBufferSort(const PairBufferSoA& buffer) {
    return preflightPairBufferSort(buffer).needsSort();
}

} // namespace fuse::physics::broadphase
