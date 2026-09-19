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
    if (!preflightPairBufferWriteSlot(*this, slot, idxA, idxB).canWrite()) {
        return;
    }

    const CandidatePair pair = canonicalPair(idxA, idxB);
    bodyA[slot] = pair.bodyA;
    bodyB[slot] = pair.bodyB;
    validFlags[slot] = 1u;
}

void PairBufferSoA::invalidateSlot(u32 slot) {
    if (!preflightPairBufferInvalidateSlot(*this, slot).canInvalidate()) {
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
        if (preflight.atCapacity) {
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
    const PairBufferCompactionPreflight compactionPreflight = preflightPairBufferCompaction(*this);
    if (compactionPreflight.reason == PairBufferCompactionRejectReason::EmptyBuffer) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }

    if (compactionPreflight.reason == PairBufferCompactionRejectReason::AllValid) {
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
    if (!shouldRunPairBufferSort(*this)) {
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
    const PairBufferCompactAndClampPreflight preflight = preflightPairBufferCompactAndClamp(*this);
    if (preflight.reason == PairBufferCompactAndClampRejectReason::EmptyBuffer) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }
    if (preflight.reason == PairBufferCompactAndClampRejectReason::NoWork) {
        return activeCount;
    }

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
    if (!preflightPairBufferToVector(*this).canExport()) {
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

bool canSkipPairBufferCompaction(const PairBufferSoA& buffer) {
    return !preflightPairBufferCompaction(buffer).needsCompaction();
}

bool shouldRunPairBufferCompaction(const PairBufferSoA& buffer) {
    return preflightPairBufferCompaction(buffer).needsCompaction();
}

PairBufferClampPreflight preflightPairBufferClamp(const PairBufferSoA& buffer) {
    PairBufferClampPreflight preflight{};
    preflight.reason = pairBufferClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == PairBufferClampRejectReason::WithinCapacity;
    return preflight;
}

bool canSkipPairBufferClamp(const PairBufferSoA& buffer) {
    return !preflightPairBufferClamp(buffer).needsClamp();
}

bool shouldRunPairBufferClamp(const PairBufferSoA& buffer) {
    return preflightPairBufferClamp(buffer).needsClamp();
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

PairBufferSortPreflight preflightPairBufferSort(const PairBufferSoA& buffer) {
    PairBufferSortPreflight preflight{};
    preflight.reason = pairBufferSortRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferSortRejectReason::EmptyBuffer;
    preflight.singlePair = preflight.reason == PairBufferSortRejectReason::SinglePair;
    return preflight;
}

bool canSkipPairBufferSort(const PairBufferSoA& buffer) {
    return !preflightPairBufferSort(buffer).needsSort();
}

bool shouldRunPairBufferSort(const PairBufferSoA& buffer) {
    return preflightPairBufferSort(buffer).needsSort();
}

const char* pairBufferCompactAndClampRejectReasonName(PairBufferCompactAndClampRejectReason reason) {
    switch (reason) {
    case PairBufferCompactAndClampRejectReason::None:
        return "None";
    case PairBufferCompactAndClampRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferCompactAndClampRejectReason::NoWork:
        return "NoWork";
    }
    return "Unknown";
}

PairBufferCompactAndClampRejectReason pairBufferCompactAndClampRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferCompactAndClampRejectReason::EmptyBuffer;
    }
    if (!shouldRunPairBufferCompaction(buffer) && !shouldRunPairBufferClamp(buffer)) {
        const u32 validCount = buffer.countValidSlots();
        if (buffer.pairSlotCount > 0u && buffer.activeCount != validCount) {
            return PairBufferCompactAndClampRejectReason::None;
        }
        if (buffer.activeCount != validCount) {
            return PairBufferCompactAndClampRejectReason::None;
        }
        return PairBufferCompactAndClampRejectReason::NoWork;
    }
    return PairBufferCompactAndClampRejectReason::None;
}

bool pairBufferCompactAndClampRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferCompactAndClampRejectReason expected) {
    return pairBufferCompactAndClampRejectReason(buffer) == expected;
}

PairBufferCompactAndClampPreflight preflightPairBufferCompactAndClamp(const PairBufferSoA& buffer) {
    PairBufferCompactAndClampPreflight preflight{};
    preflight.reason = pairBufferCompactAndClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == PairBufferCompactAndClampRejectReason::NoWork;
    return preflight;
}

bool canSkipPairBufferCompactAndClamp(const PairBufferSoA& buffer) {
    return !preflightPairBufferCompactAndClamp(buffer).needsCompactAndClamp();
}

bool shouldRunPairBufferCompactAndClamp(const PairBufferSoA& buffer) {
    return preflightPairBufferCompactAndClamp(buffer).needsCompactAndClamp();
}

const char* pairBufferWriteSlotRejectReasonName(PairBufferWriteSlotRejectReason reason) {
    switch (reason) {
    case PairBufferWriteSlotRejectReason::None:
        return "None";
    case PairBufferWriteSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case PairBufferWriteSlotRejectReason::InvalidPair:
        return "InvalidPair";
    }
    return "Unknown";
}

PairBufferWriteSlotRejectReason pairBufferWriteSlotRejectReason(
    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,
    u32 idxB) {
    if (slot >= buffer.pairSlotCount) {
        return PairBufferWriteSlotRejectReason::OutOfRangeSlot;
    }
    if (!isValidCandidatePair(idxA, idxB)) {
        return PairBufferWriteSlotRejectReason::InvalidPair;
    }
    return PairBufferWriteSlotRejectReason::None;
}

bool pairBufferWriteSlotRejectsForReason(
    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,
    u32 idxB,
    PairBufferWriteSlotRejectReason expected) {
    return pairBufferWriteSlotRejectReason(buffer, slot, idxA, idxB) == expected;
}

PairBufferWriteSlotPreflight preflightPairBufferWriteSlot(
    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,
    u32 idxB) {
    PairBufferWriteSlotPreflight preflight{};
    preflight.reason = pairBufferWriteSlotRejectReason(buffer, slot, idxA, idxB);
    preflight.outOfRangeSlot = preflight.reason == PairBufferWriteSlotRejectReason::OutOfRangeSlot;
    preflight.invalidPair = preflight.reason == PairBufferWriteSlotRejectReason::InvalidPair;
    return preflight;
}

bool canSkipPairBufferWriteSlot(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    return !preflightPairBufferWriteSlot(buffer, slot, idxA, idxB).canWrite();
}

bool shouldRunPairBufferWriteSlot(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    return preflightPairBufferWriteSlot(buffer, slot, idxA, idxB).canWrite();
}

bool wouldSkipPairBufferWriteSlot(
    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,
    u32 idxB,
    PairBufferWriteSlotRejectReason* reason) {
    const PairBufferWriteSlotRejectReason reject = pairBufferWriteSlotRejectReason(buffer, slot, idxA, idxB);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject != PairBufferWriteSlotRejectReason::None;
}

const char* pairBufferInvalidateSlotRejectReasonName(PairBufferInvalidateSlotRejectReason reason) {
    switch (reason) {
    case PairBufferInvalidateSlotRejectReason::None:
        return "None";
    case PairBufferInvalidateSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case PairBufferInvalidateSlotRejectReason::AlreadyInvalid:
        return "AlreadyInvalid";
    }
    return "Unknown";
}

PairBufferInvalidateSlotRejectReason pairBufferInvalidateSlotRejectReason(const PairBufferSoA& buffer, u32 slot) {
    if (slot >= buffer.pairSlotCount || slot >= buffer.validFlags.size()) {
        return PairBufferInvalidateSlotRejectReason::OutOfRangeSlot;
    }
    if (buffer.validFlags[slot] == 0u) {
        return PairBufferInvalidateSlotRejectReason::AlreadyInvalid;
    }
    return PairBufferInvalidateSlotRejectReason::None;
}

bool pairBufferInvalidateSlotRejectsForReason(
    const PairBufferSoA& buffer,
    u32 slot,
    PairBufferInvalidateSlotRejectReason expected) {
    return pairBufferInvalidateSlotRejectReason(buffer, slot) == expected;
}

PairBufferInvalidateSlotPreflight preflightPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    PairBufferInvalidateSlotPreflight preflight{};
    preflight.reason = pairBufferInvalidateSlotRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == PairBufferInvalidateSlotRejectReason::OutOfRangeSlot;
    preflight.alreadyInvalid = preflight.reason == PairBufferInvalidateSlotRejectReason::AlreadyInvalid;
    return preflight;
}

bool canSkipPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    return !preflightPairBufferInvalidateSlot(buffer, slot).canInvalidate();
}

bool shouldRunPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    return preflightPairBufferInvalidateSlot(buffer, slot).canInvalidate();
}

bool wouldSkipPairBufferInvalidateSlot(
    const PairBufferSoA& buffer,
    u32 slot,
    PairBufferInvalidateSlotRejectReason* reason) {
    const PairBufferInvalidateSlotRejectReason reject = pairBufferInvalidateSlotRejectReason(buffer, slot);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject != PairBufferInvalidateSlotRejectReason::None;
}

const char* pairBufferToVectorRejectReasonName(PairBufferToVectorRejectReason reason) {
    switch (reason) {
    case PairBufferToVectorRejectReason::None:
        return "None";
    case PairBufferToVectorRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    }
    return "Unknown";
}

PairBufferToVectorRejectReason pairBufferToVectorRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferToVectorRejectReason::EmptyBuffer;
    }
    return PairBufferToVectorRejectReason::None;
}

bool pairBufferToVectorRejectsForReason(const PairBufferSoA& buffer, PairBufferToVectorRejectReason expected) {
    return pairBufferToVectorRejectReason(buffer) == expected;
}

PairBufferToVectorPreflight preflightPairBufferToVector(const PairBufferSoA& buffer) {
    PairBufferToVectorPreflight preflight{};
    preflight.reason = pairBufferToVectorRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferToVectorRejectReason::EmptyBuffer;
    return preflight;
}

bool canSkipPairBufferToVector(const PairBufferSoA& buffer) {
    return !preflightPairBufferToVector(buffer).canExport();
}

bool shouldRunPairBufferToVector(const PairBufferSoA& buffer) {
    return preflightPairBufferToVector(buffer).canExport();
}

const char* pairBufferInvalidateSlotRejectReasonName(PairBufferInvalidateSlotRejectReason reason) {
    switch (reason) {
    case PairBufferInvalidateSlotRejectReason::None:
        return "None";
    case PairBufferInvalidateSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    }
    return "Unknown";
}

PairBufferInvalidateSlotRejectReason pairBufferInvalidateSlotRejectReason(const PairBufferSoA& buffer, u32 slot) {
    if (slot >= buffer.validFlags.size()) {
        return PairBufferInvalidateSlotRejectReason::OutOfRangeSlot;
    }
    return PairBufferInvalidateSlotRejectReason::None;
}

bool pairBufferInvalidateSlotRejectsForReason(
    const PairBufferSoA& buffer,
    u32 slot,
    PairBufferInvalidateSlotRejectReason expected) {
    return pairBufferInvalidateSlotRejectReason(buffer, slot) == expected;
}

PairBufferInvalidateSlotPreflight preflightPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    PairBufferInvalidateSlotPreflight preflight{};
    preflight.reason = pairBufferInvalidateSlotRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == PairBufferInvalidateSlotRejectReason::OutOfRangeSlot;
    return preflight;
}

bool canSkipPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    return !preflightPairBufferInvalidateSlot(buffer, slot).canInvalidate();
}

bool shouldRunPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    return preflightPairBufferInvalidateSlot(buffer, slot).canInvalidate();
}

} // namespace fuse::physics::broadphase

// --- deepen additive from deepen-b4-broadphase-guards-fcb2 ---
    lastRejectReason = CandidateRejectReason::None;
    const CandidateRejectReason rejectReason = candidatePairRejectReason(idxA, idxB);
    if (rejectReason != CandidateRejectReason::None) {
        lastRejectReason = rejectReason;
        lastRejectReason = CandidateRejectReason::BufferFull;

// --- deepen additive from deepen-b4-broadphase-guards-b3ad ---
    lastRejectReason = CandidatePairRejectReason::None;
    const CandidatePairRejectReason rejectReason = candidatePairRejectReason(idxA, idxB);
    if (rejectReason != CandidatePairRejectReason::None) {
        lastRejectReason = CandidatePairRejectReason::BufferFull;

// --- deepen additive from deepen-b4-broadphase-preflights-82c3 ---
PairBufferSoA::DedupePreflight PairBufferSoA::preflight_dedupe() const {

// --- deepen additive from deepen-b4-broadphase-preflights-ec03 ---
PairBufferDedupePreflight preflight_dedupe_pair_buffer(const PairBufferSoA& buffer) {
bool should_skip_dedupe_pair_buffer(const PairBufferSoA& buffer) {
PairBufferClampPreflight preflight_pair_buffer_clamp(const PairBufferSoA& buffer) {

// --- deepen additive from b4-broadphase-deepen-guards-1b87 ---
PairSlotPreflight preflightPairSlots(u32 slotCount, const PairBufferSoA& buffer) {
    PairSlotPreflight preflight{};

// --- deepen additive from deepen-b4-broadphase-preflights-76e1 ---
PairBufferDedupePreflight preflight_pair_buffer_dedupe(const PairBufferSoA& buffer) {
bool should_skip_pair_buffer_dedupe(const PairBufferSoA& buffer) {

// --- deepen additive from deepen-b4-broadphase-preflights-4247 ---
bool PairBufferPreflight::can_push(u32 additionalCount) const {
PairBufferPreflight preflight_pair_buffer(const PairBufferSoA& buffer) {
    PairBufferPreflight preflight{};
bool should_skip_pair_buffer_compaction(const PairBufferSoA& buffer) {

// --- deepen additive from deepen-b4-broadphase-guards-4311 ---
        if (preflight.reason == PairBufferPushRejectReason::AtCapacity) {
    const PairBufferCompactionPreflight preflight = preflightPairBufferCompaction(*this);
    if (preflight.reason == PairBufferCompactionRejectReason::EmptyBuffer) {
    if (preflight.reason == PairBufferCompactionRejectReason::AllValid) {
    if (!preflightPairBufferSort(*this).needsSort()) {

// --- deepen additive from deepen-b4-broadphase-guards-bd20 ---
bool pairBufferCompactionRejectsForReason(const PairBufferSoA& buffer, PairBufferCompactionRejectReason expected) {

// --- deepen additive from deepen-b4-broadphase-guards-9b1a ---
    const PairBufferPushPreflight pushPreflight = preflightPairBufferPush(*this, idxA, idxB);
    if (!pushPreflight.canPush()) {
        if (pushPreflight.atCapacity) {
    if (compactionPreflight.emptyBuffer) {
    if (!compactionPreflight.needsCompaction()) {
    const PairBufferClampPreflight clampPreflight = preflightPairBufferClamp(*this);
    if (!clampPreflight.needsClamp()) {
    const PairBufferCompactionPreflight compactionPreflight = preflightPairBufferCompaction(buffer);
    const PairBufferClampPreflight clampPreflight = preflightPairBufferClamp(buffer);
    preflight.emptyBuffer = compactionPreflight.emptyBuffer;
    preflight.needsCompaction = compactionPreflight.needsCompaction();
    preflight.needsClamp = clampPreflight.needsClamp();

// --- deepen additive from deepen-b4-broadphase-guards-7162 ---
    if (!preflightPairBufferPrepareSlots(slotCount).canPrepare()) {
    return !preflightPairBufferSort(*this).needsSort();
PairBufferPrepareSlotsPreflight preflightPairBufferPrepareSlots(u32 slotCount) {
    PairBufferPrepareSlotsPreflight preflight{};
PairBufferMergePreflight preflightPairBufferMerge(const PairBufferSoA& buffer, u32 incomingCount) {
    PairBufferMergePreflight preflight{};
BroadphaseMergeIntoBufferPreflight preflightBroadphaseMergeIntoBuffer(
    BroadphaseMergeIntoBufferPreflight preflight{};
    preflight.merge = preflightBroadphaseMerge(bodies, shapes);
    preflight.buffer = preflightPairBufferMerge(buffer, incomingPairCount);

// --- deepen additive from deepen-b4-broadphase-guards-603e ---
        if (pushPreflight.reason == PairBufferPushRejectReason::AtCapacity) {

// --- deepen additive from deepen-b4-broadphase-guards-9072 ---
    return preflightPairBufferPush(buffer, idxA, idxB, 0u);
    return preflightPairBufferWriteSlot(buffer, slot, idxA, idxB, 0u);
PairBufferMergePreflight preflightPairBufferMerge(const PairBufferSoA& buffer, u32 incomingPairCount) {

// --- deepen additive from deepen-b4-broadphase-guards-f83b ---
    return !preflightPairBufferPush(buffer, idxA, idxB).canPush();

// --- deepen additive from deepen-b4-broadphase-guards-cbb3 ---
    preflight.compaction = preflightPairBufferCompaction(buffer);
    preflight.clamp = preflightPairBufferClamp(buffer);
    return !preflightPairBufferCompactAndClamp(buffer).needsWork();

// --- deepen additive from deepen-b4-broadphase-guards-14d5 ---
    return preflightPairBufferCompactAndClamp(buffer).emptyBuffer;

// --- deepen additive from b4-broadphase-deepen-guards-727e ---
    if (!preflightPairBufferCompactAndClamp(*this).needsCompactAndClamp()) {
    case PairBufferCompactAndClampRejectReason::NoWorkNeeded:
        return PairBufferCompactAndClampRejectReason::NoWorkNeeded;
    preflight.noWorkNeeded = preflight.reason == PairBufferCompactAndClampRejectReason::NoWorkNeeded;

// --- deepen additive from deepen-b4-broadphase-guards-03cf ---
    if (preflight.reason == PairBufferCompactAndClampRejectReason::AlreadyCompactAndWithinCapacity) {
    case PairBufferCompactAndClampRejectReason::AlreadyCompactAndWithinCapacity:
        return PairBufferCompactAndClampRejectReason::AlreadyCompactAndWithinCapacity;
        preflight.reason == PairBufferCompactAndClampRejectReason::AlreadyCompactAndWithinCapacity;

// --- deepen additive from deepen-b4-broadphase-guards-b64e ---
const char* pairBufferSlotWriteRejectReasonName(PairBufferSlotWriteRejectReason reason) {
    case PairBufferSlotWriteRejectReason::None:
    case PairBufferSlotWriteRejectReason::OutOfRangeSlot:
    case PairBufferSlotWriteRejectReason::InvalidPair:
PairBufferSlotWriteRejectReason pairBufferSlotWriteRejectReason(
        return PairBufferSlotWriteRejectReason::OutOfRangeSlot;
        return PairBufferSlotWriteRejectReason::InvalidPair;
    return PairBufferSlotWriteRejectReason::None;
    PairBufferSlotWriteRejectReason expected) {
    return pairBufferSlotWriteRejectReason(buffer, slot, idxA, idxB) == expected;
PairBufferSlotWritePreflight preflightPairBufferSlotWrite(
    PairBufferSlotWritePreflight preflight{};
    preflight.reason = pairBufferSlotWriteRejectReason(buffer, slot, idxA, idxB);
    preflight.outOfRangeSlot = preflight.reason == PairBufferSlotWriteRejectReason::OutOfRangeSlot;
    preflight.invalidPair = preflight.reason == PairBufferSlotWriteRejectReason::InvalidPair;
const char* pairBufferSlotReservationRejectReasonName(PairBufferSlotReservationRejectReason reason) {
    case PairBufferSlotReservationRejectReason::None:
    case PairBufferSlotReservationRejectReason::ZeroSlots:
    case PairBufferSlotReservationRejectReason::ExceedsCapacity:
PairBufferSlotReservationRejectReason pairBufferSlotReservationRejectReason(
        return PairBufferSlotReservationRejectReason::ZeroSlots;
        return PairBufferSlotReservationRejectReason::ExceedsCapacity;
    return PairBufferSlotReservationRejectReason::None;
    PairBufferSlotReservationRejectReason expected) {
    return pairBufferSlotReservationRejectReason(buffer, slotCount) == expected;
PairBufferSlotReservationPreflight preflightPairBufferSlotReservation(
    PairBufferSlotReservationPreflight preflight{};
    preflight.reason = pairBufferSlotReservationRejectReason(buffer, slotCount);
    preflight.zeroSlots = preflight.reason == PairBufferSlotReservationRejectReason::ZeroSlots;
    preflight.exceedsCapacity = preflight.reason == PairBufferSlotReservationRejectReason::ExceedsCapacity;
    return !preflightPairBufferSlotReservation(buffer, slotCount).canReserve();
    return preflightPairBufferSlotReservation(buffer, slotCount).canReserve();

// --- deepen additive from deepen-b4-broadphase-guards-c372 ---
    if (!preflightPairBufferCompactAndClamp(*this).needsWork()) {
    if (!preflightPairBufferCompaction(buffer).needsCompaction() &&
        !preflightPairBufferClamp(buffer).needsClamp()) {
    return preflightPairBufferCompactAndClamp(buffer).needsWork();

// --- deepen additive from deepen-b4-broadphase-guards-1d11 ---
    return !preflightPairBufferCompactAndClamp(*this).needsCompactAndClamp();

// --- deepen additive from b4-broadphase-deepen-a14d ---
    if (preflightPairBufferCompaction(*this).needsCompaction()) {

// --- deepen additive from deepen-b4-broadphase-guards-b86e ---
    if (pairBufferCompactAndClampRejectReason(*this) == PairBufferCompactAndClampRejectReason::EmptyBuffer) {
    if (pairBufferCompactAndClampRejectReason(*this) == PairBufferCompactAndClampRejectReason::NoWork) {

// --- deepen additive from deepen-b4-broadphase-guards-3ca3 ---
    if (preflightPairBufferCompactAndClamp(*this).reason == PairBufferCompactAndClampRejectReason::EmptyBuffer) {

// --- deepen additive from b4-broadphase-deepen-guards-5209 ---
    if (!preflightPairSlots(slotCount, *this).canPrepare()) {

// --- deepen additive from b4-broadphase-deepen-guards-ca26 ---
    return preflightPairBufferPush(buffer, idxA, idxB).canPush();

// --- deepen additive from deepen-b4-broadphase-guards-15cc ---
    if (!preflightPairBufferSlotWrite(*this, slot, idxA, idxB).canWrite()) {
const char* pairBufferMergeIntoRejectReasonName(PairBufferMergeIntoRejectReason reason) {
    case PairBufferMergeIntoRejectReason::None:
    case PairBufferMergeIntoRejectReason::EmptyInput:
    case PairBufferMergeIntoRejectReason::BufferFull:
PairBufferMergeIntoRejectReason pairBufferMergeIntoRejectReason(
        return PairBufferMergeIntoRejectReason::EmptyInput;
        return PairBufferMergeIntoRejectReason::BufferFull;
    return PairBufferMergeIntoRejectReason::None;
    PairBufferMergeIntoRejectReason expected) {
    return pairBufferMergeIntoRejectReason(buffer, pairCount) == expected;
PairBufferMergeIntoPreflight preflightPairBufferMergeInto(const PairBufferSoA& buffer, u32 pairCount) {
    PairBufferMergeIntoPreflight preflight{};
    preflight.reason = pairBufferMergeIntoRejectReason(buffer, pairCount);
    preflight.emptyInput = preflight.reason == PairBufferMergeIntoRejectReason::EmptyInput;
    preflight.bufferFull = preflight.reason == PairBufferMergeIntoRejectReason::BufferFull;
    return !preflightPairBufferMergeInto(buffer, pairCount).canMerge();
    return preflightPairBufferMergeInto(buffer, pairCount).canMerge();

// --- deepen additive from deepen-b4-broadphase-guards-e578 ---
    if (preflightPairBufferClamp(*this).needsClamp()) {
const char* pairBufferCompactClampRejectReasonName(PairBufferCompactClampRejectReason reason) {
    case PairBufferCompactClampRejectReason::None:
    case PairBufferCompactClampRejectReason::EmptyBuffer:
    case PairBufferCompactClampRejectReason::NoWork:
PairBufferCompactClampRejectReason pairBufferCompactClampRejectReason(const PairBufferSoA& buffer) {
        return PairBufferCompactClampRejectReason::EmptyBuffer;
        return PairBufferCompactClampRejectReason::NoWork;
    return PairBufferCompactClampRejectReason::None;
    PairBufferCompactClampRejectReason expected) {
    return pairBufferCompactClampRejectReason(buffer) == expected;
PairBufferCompactClampPreflight preflightPairBufferCompactClamp(const PairBufferSoA& buffer) {
    PairBufferCompactClampPreflight preflight{};
    preflight.reason = pairBufferCompactClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferCompactClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == PairBufferCompactClampRejectReason::NoWork;
    preflight.needsCompaction = preflightPairBufferCompaction(buffer).needsCompaction();
    preflight.needsClamp = preflightPairBufferClamp(buffer).needsClamp();
    return !preflightPairBufferCompactClamp(buffer).canRun();
    return preflightPairBufferCompactClamp(buffer).canRun();

// --- deepen additive from deepen-b4-broadphase-guards-ce99 ---
    const PairBufferWriteSlotPreflight preflight = preflightPairBufferWriteSlot(*this, slot, idxA, idxB);

// --- deepen additive from deepen-b4-broadphase-guards-a65f ---
    case PairBufferWriteSlotRejectReason::InvalidSlot:
        return PairBufferWriteSlotRejectReason::InvalidSlot;
    preflight.invalidSlot = preflight.reason == PairBufferWriteSlotRejectReason::InvalidSlot;
const char* pairBufferMergeRejectReasonName(PairBufferMergeRejectReason reason) {
    case PairBufferMergeRejectReason::None:
    case PairBufferMergeRejectReason::EmptyPairs:
    case PairBufferMergeRejectReason::AtCapacity:
PairBufferMergeRejectReason pairBufferMergeRejectReason(const PairBufferSoA& buffer, u32 pairCount) {
        return PairBufferMergeRejectReason::EmptyPairs;
        return PairBufferMergeRejectReason::AtCapacity;
    return PairBufferMergeRejectReason::None;
    PairBufferMergeRejectReason expected) {
    return pairBufferMergeRejectReason(buffer, pairCount) == expected;
PairBufferMergePreflight preflightPairBufferMerge(const PairBufferSoA& buffer, u32 pairCount) {
    preflight.reason = pairBufferMergeRejectReason(buffer, pairCount);
    preflight.emptyPairs = preflight.reason == PairBufferMergeRejectReason::EmptyPairs;
    preflight.atCapacity = preflight.reason == PairBufferMergeRejectReason::AtCapacity;
    return !preflightPairBufferMerge(buffer, pairCount).canMerge();
    return preflightPairBufferMerge(buffer, pairCount).canMerge();

// --- deepen additive from deepen-b4-broadphase-guards-ff66 ---
    return preflightPairBufferAcceptPairs(*this, additionalCount).canAccept();
const char* pairBufferAcceptPairsRejectReasonName(PairBufferAcceptPairsRejectReason reason) {
    case PairBufferAcceptPairsRejectReason::None:
    case PairBufferAcceptPairsRejectReason::ExceedsCapacity:
PairBufferAcceptPairsRejectReason pairBufferAcceptPairsRejectReason(
        return PairBufferAcceptPairsRejectReason::None;
        return PairBufferAcceptPairsRejectReason::ExceedsCapacity;
    PairBufferAcceptPairsRejectReason expected) {
    return pairBufferAcceptPairsRejectReason(buffer, additionalCount) == expected;
PairBufferAcceptPairsPreflight preflightPairBufferAcceptPairs(
    PairBufferAcceptPairsPreflight preflight{};
    preflight.reason = pairBufferAcceptPairsRejectReason(buffer, additionalCount);
    preflight.exceedsCapacity = preflight.reason == PairBufferAcceptPairsRejectReason::ExceedsCapacity;
    return !preflightPairBufferAcceptPairs(buffer, additionalCount).canAccept();
    return preflightPairBufferAcceptPairs(buffer, additionalCount).canAccept();

// --- deepen additive from deepen-b4-broadphase-guards-5597 ---
    case PairBufferDedupeRejectReason::AlreadyUnique:
        return PairBufferDedupeRejectReason::AlreadyUnique;
    preflight.alreadyUnique = preflight.reason == PairBufferDedupeRejectReason::AlreadyUnique;
    case PairBufferSortRejectReason::AlreadySorted:
        return PairBufferSortRejectReason::AlreadySorted;
    preflight.alreadySorted = preflight.reason == PairBufferSortRejectReason::AlreadySorted;

// --- deepen additive from deepen-b4-broadphase-guards-7f22 ---
const char* pairBufferPrepareSlotsRejectReasonName(PairBufferPrepareSlotsRejectReason reason) {
    case PairBufferPrepareSlotsRejectReason::None:
    case PairBufferPrepareSlotsRejectReason::ZeroSlots:
PairBufferPrepareSlotsRejectReason pairBufferPrepareSlotsRejectReason(u32 slotCount) {
        return PairBufferPrepareSlotsRejectReason::ZeroSlots;
    return PairBufferPrepareSlotsRejectReason::None;
bool pairBufferPrepareSlotsRejectsForReason(u32 slotCount, PairBufferPrepareSlotsRejectReason expected) {
    return pairBufferPrepareSlotsRejectReason(slotCount) == expected;
    preflight.reason = pairBufferPrepareSlotsRejectReason(slotCount);
    preflight.zeroSlots = preflight.reason == PairBufferPrepareSlotsRejectReason::ZeroSlots;
    return !preflightPairBufferPrepareSlots(slotCount).canPrepare();
    return preflightPairBufferPrepareSlots(slotCount).canPrepare();

// --- deepen additive from deepen-b4-broadphase-guards-6421 ---
    if (preflightPairBufferCompaction(buffer).needsCompaction()) {

// --- deepen additive from deepen-b4-broadphase-guards-1618 ---
    case PairBufferAcceptPairsRejectReason::AtCapacity:
    return PairBufferAcceptPairsRejectReason::AtCapacity;
PairBufferAcceptPairsPreflight preflightPairBufferAcceptPairs(const PairBufferSoA& buffer, u32 additionalCount) {
    preflight.atCapacity = preflight.reason == PairBufferAcceptPairsRejectReason::AtCapacity;

// --- deepen additive from deepen-b4-broadphase-guards-6a82 ---
    return preflightPairBufferWriteSlot(*this, slot, idxA, idxB).canWrite();

// --- deepen additive from deepen-b4-broadphase-guards-e86f ---
const char* pairBufferAcceptRejectReasonName(PairBufferAcceptRejectReason reason) {
    case PairBufferAcceptRejectReason::None:
    case PairBufferAcceptRejectReason::ExceedsCapacity:
PairBufferAcceptRejectReason pairBufferAcceptRejectReason(const PairBufferSoA& buffer, u32 additionalCount) {
        return PairBufferAcceptRejectReason::None;
    return PairBufferAcceptRejectReason::ExceedsCapacity;
    PairBufferAcceptRejectReason expected) {
    return pairBufferAcceptRejectReason(buffer, additionalCount) == expected;
PairBufferAcceptPreflight preflightPairBufferAccept(const PairBufferSoA& buffer, u32 additionalCount) {
    PairBufferAcceptPreflight preflight{};
    preflight.reason = pairBufferAcceptRejectReason(buffer, additionalCount);
    preflight.exceedsCapacity = preflight.reason == PairBufferAcceptRejectReason::ExceedsCapacity;
    return !preflightPairBufferAccept(buffer, additionalCount).canAccept();
    return preflightPairBufferAccept(buffer, additionalCount).canAccept();

// --- deepen additive from deepen-b4-broadphase-guards-18e5 ---
    case PairBufferWriteSlotRejectReason::OutOfSlot:
        return PairBufferWriteSlotRejectReason::OutOfSlot;
    preflight.outOfSlot = preflight.reason == PairBufferWriteSlotRejectReason::OutOfSlot;
    case PairBufferInvalidateSlotRejectReason::OutOfSlot:
        return PairBufferInvalidateSlotRejectReason::OutOfSlot;
    preflight.outOfSlot = preflight.reason == PairBufferInvalidateSlotRejectReason::OutOfSlot;

// --- deepen additive from deepen-b4-broadphase-guards-36be ---
const char* pairBufferSlotInvalidateRejectReasonName(PairBufferSlotInvalidateRejectReason reason) {
    case PairBufferSlotInvalidateRejectReason::None:
    case PairBufferSlotInvalidateRejectReason::OutOfRangeSlot:
PairBufferSlotInvalidateRejectReason pairBufferSlotInvalidateRejectReason(const PairBufferSoA& buffer, u32 slot) {
        return PairBufferSlotInvalidateRejectReason::OutOfRangeSlot;
    return PairBufferSlotInvalidateRejectReason::None;
    PairBufferSlotInvalidateRejectReason expected) {
    return pairBufferSlotInvalidateRejectReason(buffer, slot) == expected;
PairBufferSlotInvalidatePreflight preflightPairBufferSlotInvalidate(const PairBufferSoA& buffer, u32 slot) {
    PairBufferSlotInvalidatePreflight preflight{};
    preflight.reason = pairBufferSlotInvalidateRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == PairBufferSlotInvalidateRejectReason::OutOfRangeSlot;
    return !preflightPairBufferSlotInvalidate(buffer, slot).canInvalidate();
    return preflightPairBufferSlotInvalidate(buffer, slot).canInvalidate();

// --- deepen additive from b4-broadphase-deepen-guards-fff1 ---
    if (!preflightPairBufferWrite(*this, slot, idxA, idxB).canWrite()) {
    if (!preflightPairBufferInvalidate(*this, slot).canInvalidate()) {
const char* pairBufferWriteRejectReasonName(PairBufferWriteRejectReason reason) {
    case PairBufferWriteRejectReason::None:
    case PairBufferWriteRejectReason::OutOfRangeSlot:
    case PairBufferWriteRejectReason::InvalidPair:
PairBufferWriteRejectReason pairBufferWriteRejectReason(
        return PairBufferWriteRejectReason::OutOfRangeSlot;
        return PairBufferWriteRejectReason::InvalidPair;
    return PairBufferWriteRejectReason::None;
    PairBufferWriteRejectReason expected) {
    return pairBufferWriteRejectReason(buffer, slot, idxA, idxB) == expected;
PairBufferWritePreflight preflightPairBufferWrite(
    PairBufferWritePreflight preflight{};
    preflight.reason = pairBufferWriteRejectReason(buffer, slot, idxA, idxB);
    preflight.outOfRangeSlot = preflight.reason == PairBufferWriteRejectReason::OutOfRangeSlot;
    preflight.invalidPair = preflight.reason == PairBufferWriteRejectReason::InvalidPair;
    return !preflightPairBufferWrite(buffer, slot, idxA, idxB).canWrite();
    return preflightPairBufferWrite(buffer, slot, idxA, idxB).canWrite();
const char* pairBufferInvalidateRejectReasonName(PairBufferInvalidateRejectReason reason) {
    case PairBufferInvalidateRejectReason::None:
    case PairBufferInvalidateRejectReason::OutOfRangeSlot:
    case PairBufferInvalidateRejectReason::AlreadyInvalid:
PairBufferInvalidateRejectReason pairBufferInvalidateRejectReason(const PairBufferSoA& buffer, u32 slot) {
        return PairBufferInvalidateRejectReason::OutOfRangeSlot;
        return PairBufferInvalidateRejectReason::AlreadyInvalid;
    return PairBufferInvalidateRejectReason::None;
    PairBufferInvalidateRejectReason expected) {
    return pairBufferInvalidateRejectReason(buffer, slot) == expected;
PairBufferInvalidatePreflight preflightPairBufferInvalidate(const PairBufferSoA& buffer, u32 slot) {
    PairBufferInvalidatePreflight preflight{};
    preflight.reason = pairBufferInvalidateRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == PairBufferInvalidateRejectReason::OutOfRangeSlot;
    preflight.alreadyInvalid = preflight.reason == PairBufferInvalidateRejectReason::AlreadyInvalid;
    return !preflightPairBufferInvalidate(buffer, slot).canInvalidate();
    return preflightPairBufferInvalidate(buffer, slot).canInvalidate();

// --- deepen additive from deepen-b4-broadphase-guards-a547 ---
    if (!preflightPairBufferSlotInvalidate(*this, slot).canInvalidate()) {

// --- deepen additive from b4-broadphase-deepen-guards-7a6f ---
PairBufferWritePreflight preflightPairBufferWrite(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {

// --- deepen additive from deepen-b4-broadphase-guards-0d2e ---
    case PairBufferWriteRejectReason::UnpreparedBuffer:
        return PairBufferWriteRejectReason::UnpreparedBuffer;
    preflight.unpreparedBuffer = preflight.reason == PairBufferWriteRejectReason::UnpreparedBuffer;

// --- deepen additive from deepen-b4-broadphase-guards-b5b2 ---
    case PairBufferWriteSlotRejectReason::UnpreparedBuffer:
        return PairBufferWriteSlotRejectReason::UnpreparedBuffer;
    preflight.unpreparedBuffer = preflight.reason == PairBufferWriteSlotRejectReason::UnpreparedBuffer;

// --- deepen additive from deepen-b4-broadphase-guards-900d ---
    case PairBufferWriteRejectReason::UnpreparedSlots:
        return PairBufferWriteRejectReason::UnpreparedSlots;
    preflight.unpreparedSlots = preflight.reason == PairBufferWriteRejectReason::UnpreparedSlots;

// --- deepen additive from deepen-b4-broadphase-guards-2345 ---
    case PairBufferWriteRejectReason::NoPreparedSlots:
        return PairBufferWriteRejectReason::NoPreparedSlots;
    preflight.noPreparedSlots = preflight.reason == PairBufferWriteRejectReason::NoPreparedSlots;
    case PairBufferInvalidateRejectReason::NoPreparedSlots:
        return PairBufferInvalidateRejectReason::NoPreparedSlots;
    preflight.noPreparedSlots = preflight.reason == PairBufferInvalidateRejectReason::NoPreparedSlots;

// --- deepen additive from deepen-b4-broadphase-guards-dc4d ---
    case PairBufferWriteRejectReason::NoSlotStorage:
        return PairBufferWriteRejectReason::NoSlotStorage;
    preflight.noSlotStorage = preflight.reason == PairBufferWriteRejectReason::NoSlotStorage;
    case PairBufferInvalidateRejectReason::NoSlotStorage:
        return PairBufferInvalidateRejectReason::NoSlotStorage;
    preflight.noSlotStorage = preflight.reason == PairBufferInvalidateRejectReason::NoSlotStorage;

// --- deepen additive from b4-broadphase-deepen-guards-ed9f ---
PairBufferInvalidatePreflight preflightPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {

// --- deepen additive from b4-broadphase-deepen-guards-0ec6 ---
PairBufferWritePreflight preflightPairBufferWriteSlot(

// --- deepen additive from deepen-b4-broadphase-guards-1159 ---
    (void)invalidateSlotWithPreflight(slot);
bool PairBufferSoA::invalidateSlotWithPreflight(u32 slot) {
void dedupePairBufferSoAWithPreflight(PairBufferSoA& buffer) {

// --- deepen additive from deepen-b4-broadphase-guards-9ddb ---
    case PairBufferInvalidateSlotRejectReason::EmptyBuffer:
        return PairBufferInvalidateSlotRejectReason::EmptyBuffer;
    preflight.emptyBuffer = preflight.reason == PairBufferInvalidateSlotRejectReason::EmptyBuffer;
bool invalidateSlotWithPreflight(PairBufferSoA& buffer, u32 slot) {

// --- deepen additive from deepen-b4-broadphase-wouldskip-4c27 ---
    const PairBufferWriteSlotRejectReason rejectReason = pairBufferWriteSlotRejectReason(buffer, slot, idxA, idxB);
    return rejectReason != PairBufferWriteSlotRejectReason::None;
    const PairBufferInvalidateSlotRejectReason rejectReason = pairBufferInvalidateSlotRejectReason(buffer, slot);
    return rejectReason != PairBufferInvalidateSlotRejectReason::None;
bool wouldSkipPairBufferPush(
    PairBufferPushRejectReason* reason) {
    const PairBufferPushRejectReason rejectReason = pairBufferPushRejectReason(buffer, idxA, idxB);
    return rejectReason != PairBufferPushRejectReason::None;
