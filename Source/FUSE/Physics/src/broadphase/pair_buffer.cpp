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

PairSlotPreflight preflightPairSlots(u32 slotCount, const PairBufferSoA& buffer) {
    PairSlotPreflight preflight{};
    preflight.slotCount = slotCount;
    if (slotCount == 0u) {
        preflight.skipped = true;
        return preflight;
    }
    if (buffer.maxCapacity > 0u && slotCount > buffer.maxCapacity) {
        preflight.exceedsBufferCapacity = true;

PairBufferDedupePreflight preflight_pair_buffer_dedupe(const PairBufferSoA& buffer) {
    PairBufferDedupePreflight preflight{};
    preflight.activeCount = buffer.activeCount;
    preflight.skipped = buffer.canSkipDedupe();

bool should_skip_pair_buffer_dedupe(const PairBufferSoA& buffer) {
    return !preflight_pair_buffer_dedupe(buffer).can_dedupe();
const char* pairBufferPushRejectReasonName(PairBufferPushRejectReason reason) {
    switch (reason) {
    case PairBufferPushRejectReason::None:
        return "None";
    case PairBufferPushRejectReason::InvalidPair:
        return "InvalidPair";
    case PairBufferPushRejectReason::AtCapacity:
        return "AtCapacity";
    return "Unknown";

const char* pairBufferCompactionRejectReasonName(PairBufferCompactionRejectReason reason) {
    case PairBufferCompactionRejectReason::None:
    case PairBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferCompactionRejectReason::AllValid:
        return "AllValid";

const char* pairBufferClampRejectReasonName(PairBufferClampRejectReason reason) {
    case PairBufferClampRejectReason::None:
    case PairBufferClampRejectReason::EmptyBuffer:
    case PairBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";

PairBufferPushRejectReason pairBufferPushRejectReason(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    if (!isValidCandidatePair(idxA, idxB)) {
        return PairBufferPushRejectReason::InvalidPair;
    if (buffer.isFull()) {
        return PairBufferPushRejectReason::AtCapacity;
    return PairBufferPushRejectReason::None;

bool pairBufferPushRejectsForReason(
    const PairBufferSoA& buffer,
    u32 idxA,
    u32 idxB,
    PairBufferPushRejectReason expected) {
    return pairBufferPushRejectReason(buffer, idxA, idxB) == expected;

PairBufferCompactionRejectReason pairBufferCompactionRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferCompactionRejectReason::EmptyBuffer;
    if (buffer.canSkipCompaction()) {
        return PairBufferCompactionRejectReason::AllValid;
    return PairBufferCompactionRejectReason::None;

bool pairBufferCompactionRejectsForReason(
    PairBufferCompactionRejectReason expected) {
    return pairBufferCompactionRejectReason(buffer) == expected;

PairBufferClampRejectReason pairBufferClampRejectReason(const PairBufferSoA& buffer) {
        return PairBufferClampRejectReason::EmptyBuffer;
    if (!buffer.canApplyMaxCapacityClamp()) {
        return PairBufferClampRejectReason::WithinCapacity;
    return PairBufferClampRejectReason::None;

bool pairBufferClampRejectsForReason(const PairBufferSoA& buffer, PairBufferClampRejectReason expected) {
    return pairBufferClampRejectReason(buffer) == expected;

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
    if (maxCapacity > 0u && activeCount > maxCapacity) {
        if (!isSortedCanonical()) {
            sortCanonical();
        }
        applyMaxCapacityClamp();
    }
}

void PairBufferSoA::clear() {
    activeCount = 0;
    pairSlotCount = 0;
    droppedCount = 0;
    lastRejectReason = CandidateRejectReason::None;
    lastRejectReason = CandidatePairRejectReason::None;
    bodyA.resize(0);
    bodyB.resize(0);
    validFlags.resize(0);
}

bool PairBufferSoA::hasInvalidSlots() const {
    if (canSkipSoAIteration()) {
        return false;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    for (u32 slot = 0; slot < scanCount; ++slot) {
        if (validFlags[slot] == 0u) {
            return true;
        }
    }
    return false;
}

bool PairBufferSoA::canSkipCompact() const {
    return canSkipSoAIteration() || !hasInvalidSlots();
}

void PairBufferSoA::preparePairSlots(u32 slotCount) {
    if (slotCount == 0u) {
        clear();
    if (!preflightPairBufferPrepareSlots(slotCount).canPrepare()) {
        pairSlotCount = 0u;
        activeCount = 0u;
        droppedCount = 0u;
        bodyA.clear();
        bodyB.clear();
        validFlags.clear();
    if (!preflightPairSlots(slotCount, *this).canPrepare()) {
        pairSlotCount = 0;
        activeCount = 0;
        droppedCount = 0;
        bodyA.resize(0);
        bodyB.resize(0);
        validFlags.resize(0);
        return;
    }

    pairSlotCount = slotCount;
    activeCount = 0;
    droppedCount = 0;
    bodyA.assign(slotCount, 0u);
    bodyB.assign(slotCount, 0u);
    validFlags.assign(slotCount, 0u);
}

void PairBufferSoA::writeSlot(u32 slot, u32 idxA, u32 idxB) {
    if (!preflightPairBufferWriteSlot(*this, slot, idxA, idxB).canWrite()) {
void PairBufferSoA::writeSlot(u32 slot, u32 idxA, u32 idxB, u32 bodyCount) {
    if (slot >= pairSlotCount) {
    if (!shouldRunPairBufferWriteSlot(*this, slot, idxA, idxB)) {
    if (!preflightPairBufferWrite(*this, slot, idxA, idxB).canWrite()) {
    if (!preflightPairBufferSlotWrite(*this, slot, idxA, idxB).canWrite()) {
        return;
    }

    const CandidateRejectReason rejectReason = candidatePairRejectReason(idxA, idxB);
    if (rejectReason != CandidateRejectReason::None) {
        lastRejectReason = rejectReason;
    if (slot >= pairSlotCount || !isValidCandidatePair(idxA, idxB, bodyCount)) {
    const CandidatePairRejectReason rejectReason = candidatePairRejectReason(idxA, idxB, bodyCount);
    if (rejectReason != CandidatePairRejectReason::None) {
    const CandidatePairRejectReason rejectReason = candidatePairRejectReason(idxA, idxB);
    if (!preflightPairBufferSlotWrite(*this, slot, idxA, idxB).canWrite()) {
    const PairBufferWriteSlotPreflight preflight = preflightPairBufferWriteSlot(*this, slot, idxA, idxB);
    if (!preflight.canWrite()) {
    if (!shouldRunPairBufferWriteSlot(*this, slot, idxA, idxB)) {
bool PairBufferSoA::writeSlot(u32 slot, u32 idxA, u32 idxB) {
        return false;
    }

    const CandidatePair pair = canonicalPair(idxA, idxB);
    bodyA[slot] = pair.bodyA;
    bodyB[slot] = pair.bodyB;
    validFlags[slot] = 1u;
    lastRejectReason = CandidatePairRejectReason::None;
    return true;
}

void PairBufferSoA::invalidateSlot(u32 slot) {
    if (!preflightPairBufferInvalidateSlot(*this, slot).canInvalidate()) {
        return;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
    if (!preflightPairBufferInvalidate(*this, slot).canInvalidate()) {
    if (!preflightPairBufferSlotInvalidate(*this, slot).canInvalidate()) {
    if (!shouldRunPairBufferInvalidateSlot(*this, slot)) {
        return;
    }
    validFlags[slot] = 0u;
}

u32 PairBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
bool PairBufferSoA::push(u32 idxA, u32 idxB) {
    const CandidateRejectReason rejectReason = candidatePairRejectReason(idxA, idxB);
    if (rejectReason != CandidateRejectReason::None) {
        lastRejectReason = rejectReason;
        return false;
    }
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;

bool PairBufferSoA::canAcceptPairs(u32 additionalCount) const {
    if (additionalCount == 0u) {
        return true;
    return activeCount + additionalCount <= maxCapacity;

bool PairBufferSoA::wouldRejectAdditionalPairs(u32 additionalCount) const {
    return !canAcceptPairs(additionalCount);
}

bool PairBufferSoA::canSkipDedupe() const {
    if (canSkipSoAIteration()) {

    const u32 validCount = countValidSlots();
    return validCount <= 1u;

bool PairBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;
    return preflightPairBufferAcceptPairs(*this, additionalCount).canAccept();

bool PairBufferSoA::canWriteSlot(u32 slot, u32 idxA, u32 idxB) const {
    return preflightPairBufferWriteSlot(*this, slot, idxA, idxB).canWrite();
}

bool PairBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
}

bool PairBufferSoA::wouldRejectPush(u32 idxA, u32 idxB) const {
    return !isValidCandidatePair(idxA, idxB) || isFull();
bool PairBufferSoA::canSkipDedupe() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    const u32 validCount = countValidSlots();
    return validCount <= 1u;

bool PairBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;
    return canSkipCompaction() && canSkipMaxCapacityClamp();

bool PairBufferSoA::push(u32 idxA, u32 idxB) {
    if (!isValidCandidatePair(idxA, idxB)) {
        return false;

    return countValidSlots() <= 1u;


bool PairBufferSoA::wouldRejectPush(u32 idxA, u32 idxB, u32 bodyCount) const {
    if (!isValidCandidatePair(idxA, idxB, bodyCount)) {
    const CandidatePairRejectReason rejectReason = candidatePairRejectReason(idxA, idxB);
    if (rejectReason != CandidatePairRejectReason::None) {
        lastRejectReason = rejectReason;
    return isFull();

bool PairBufferSoA::push(u32 idxA, u32 idxB, u32 bodyCount) {
    const CandidatePairRejectReason rejectReason = candidatePairRejectReason(idxA, idxB, bodyCount);

    const PairBufferPushPreflight preflight = preflightPairBufferPush(*this, idxA, idxB);
    if (!preflight.canPush()) {
    if (!shouldRunPairBufferPush(*this, idxA, idxB)) {
    if (pairSlotCount > 0u) {
        if (activeCount != validCount) {

        if (preflight.atCapacity) {
            ++droppedCount;
    if (maxCapacity > 0u && activeCount >= maxCapacity) {
        lastRejectReason = CandidateRejectReason::BufferFull;
    if (isFull()) {

    if (wouldRejectPush(idxA, idxB, bodyCount)) {
        if (isValidCandidatePair(idxA, idxB, bodyCount) && isFull()) {
        lastRejectReason = CandidatePairRejectReason::BufferFull;
        if (preflight.reason == PairBufferPushRejectReason::AtCapacity) {
    const PairBufferPushPreflight pushPreflight = preflightPairBufferPush(*this, idxA, idxB);
    if (!pushPreflight.canPush()) {
        if (pushPreflight.atCapacity) {
        if (pushPreflight.reason == PairBufferPushRejectReason::AtCapacity) {
        }
        return false;
    }

    const CandidatePair pair = canonicalPair(idxA, idxB);
    bodyA.push_back(pair.bodyA);
    bodyB.push_back(pair.bodyB);
    validFlags.push_back(1u);
    ++activeCount;
    pairSlotCount = activeCount;
    lastRejectReason = CandidateRejectReason::None;
    lastRejectReason = CandidatePairRejectReason::None;
    return true;
}

u32 PairBufferSoA::invalidateInvalidPairs(u32 bodyCount) {
    if (canSkipSoAIteration() || bodyCount == 0u) {
        return 0u;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 invalidated = 0u;
    for (u32 slot = 0; slot < scanCount; ++slot) {
        if (validFlags[slot] == 0u) {
            continue;
        }
        if (!isValidCandidatePair(bodyA[slot], bodyB[slot], bodyCount)) {
            validFlags[slot] = 0u;
            ++invalidated;
        }
    }
    return invalidated;
}

u32 PairBufferSoA::compact() {
    const PairBufferCompactionPreflight compactionPreflight = preflightPairBufferCompaction(*this);
    if (compactionPreflight.reason == PairBufferCompactionRejectReason::EmptyBuffer) {
    const PairBufferCompactionPreflight preflight = preflightPairBufferCompaction(*this);
    if (preflight.reason == PairBufferCompactionRejectReason::EmptyBuffer) {
    if (compactionPreflight.emptyBuffer) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }

    if (compactionPreflight.reason == PairBufferCompactionRejectReason::AllValid) {
    if (preflight.reason == PairBufferCompactionRejectReason::AllValid) {
    if (!preflight.needsCompaction()) {
    if (!compactionPreflight.needsCompaction()) {
    if (canSkipPairBufferCompaction(*this)) {
        if (preflight.emptyBuffer) {

        if (canSkipSoAIteration()) {

        activeCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 validCount = 0u;
    for (u32 i = 0u; i < scanCount; ++i) {
        if (validFlags[i] != 0u) {
            ++validCount;

    if (validCount == 0u) {
        bodyA.resize(0);
        bodyB.resize(0);
        validFlags.resize(0);

    bool alreadyPacked = true;
    for (u32 i = 0u; i < validCount; ++i) {
        if (validFlags[i] == 0u) {
            alreadyPacked = false;
            break;
    if (alreadyPacked) {
        for (u32 i = validCount; i < scanCount; ++i) {

        activeCount = validCount;
        pairSlotCount = activeCount;
        bodyA.resize(activeCount);
        bodyB.resize(activeCount);
        validFlags.resize(activeCount);
        } else {
    if (!shouldRunPairBufferCompaction(*this)) {


        return activeCount;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    if (canSkipCompact()) {
        activeCount = scanCount;
        pairSlotCount = scanCount;
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

bool PairBufferSoA::hasDuplicateCanonicalPairs() const {
bool PairBufferSoA::canSkipSortCanonical() const {
    return canSkipDedupe() || isSortedCanonical();
    if (!preflightPairBufferSort(*this).needsSort()) {
        return true;
    }
    return isSortedCanonical();

bool PairBufferSoA::isDuplicateFree() const {
    if (canSkipSoAIteration()) {

    for (u32 i = 0; i < activeCount; ++i) {
        if (!slotIsValid(i)) {
            continue;
        for (u32 j = i + 1u; j < activeCount; ++j) {
            if (!slotIsValid(j)) {
            if (bodyA[i] == bodyA[j] && bodyB[i] == bodyB[j]) {
                return false;

bool PairBufferSoA::canSkipDedupePass() const {
    return canSkipDedupe() || (isSortedCanonical() && isDuplicateFree());

    return canSkipDedupe() || isDuplicateFree();

void PairBufferSoA::sortCanonicalIfNeeded() {
    if (canSkipSortCanonical()) {
        return;
    sortCanonical();

void PairBufferSoA::sortCanonical() {
    if (canSkipDedupe()) {

    std::unordered_set<u64> seen;
    seen.reserve(activeCount * 2 + 1);
    for (u32 slot = 0; slot < activeCount; ++slot) {
        if (!slotIsValid(slot)) {
        const u64 key = (static_cast<u64>(bodyA[slot]) << 32) | bodyB[slot];
        if (!seen.insert(key).second) {

PairBufferSoA::DedupePreflight PairBufferSoA::preflight_dedupe() const {
    DedupePreflight preflight{};
        preflight.skipped = true;
        return preflight;

            ++preflight.duplicateCount;

bool PairBufferSoA::needsDedupe() const {
    return preflight_dedupe().needs_dedupe();

    if (!shouldRunPairBufferSort(*this)) {
    if (canSkipSort()) {
    if (canSkipSoAIteration() || activeCount <= 1u || isSortedCanonical()) {
    if (canSkipPairBufferSort(*this)) {
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

void PairBufferSoA::sortCanonicalIfNeeded() {
    if (!canSkipSortCanonical()) {
        sortCanonical();
    }
}

u32 PairBufferSoA::applyMaxCapacityClamp() {
    if (!preflightPairBufferClamp(*this).needsClamp()) {
    if (canSkipPairBufferClamp(*this)) {
    const PairBufferClampPreflight clampPreflight = preflightPairBufferClamp(*this);
    if (!clampPreflight.needsClamp()) {
    if (!shouldRunPairBufferClamp(*this)) {
        return activeCount;
    }

    if (!isSortedCanonical()) {
        sortCanonical();
    }
    sortCanonicalIfNeeded();

    const u32 excess = activeCount - maxCapacity;
    droppedCount += excess;
    activeCount = maxCapacity;
    pairSlotCount = activeCount;

    bodyA.resize(activeCount);
    bodyB.resize(activeCount);
    validFlags.resize(activeCount);
    return activeCount;
}

bool PairBufferSoA::canSkipDedupe() const {
    if (canSkipSoAIteration()) {
u32 PairBufferSoA::compactAndClamp() {
    if (pairBufferCompactAndClampRejectReason(*this) == PairBufferCompactAndClampRejectReason::EmptyBuffer) {
        return activeCount;
    }
    if (pairBufferCompactAndClampRejectReason(*this) == PairBufferCompactAndClampRejectReason::NoWork) {

    if (shouldRunPairBufferCompaction(*this)) {
        compact();
    if (shouldRunPairBufferClamp(*this)) {
        return applyMaxCapacityClamp();
bool PairBufferSoA::canSkipCompactAndClamp() const {
    return canSkipPairBufferCompactAndClamp(*this);

    if (preflightPairBufferCompactAndClamp(*this).reason == PairBufferCompactAndClampRejectReason::EmptyBuffer) {
        activeCount = 0u;
        pairSlotCount = 0u;

    if (!preflightPairBufferCompactAndClamp(*this).needsCompactAndClamp()) {


    const PairBufferCompactAndClampPreflight preflight = preflightPairBufferCompactAndClamp(*this);
    if (preflight.reason == PairBufferCompactAndClampRejectReason::EmptyBuffer) {
    if (preflight.reason == PairBufferCompactAndClampRejectReason::NoWork) {

    if (!shouldRunPairBufferCompactAndClamp(*this)) {


    if (!shouldRunPairBufferCompactClamp(*this)) {

    if (preflightPairBufferCompaction(*this).needsCompaction()) {
    if (preflightPairBufferClamp(*this).needsClamp()) {

bool PairBufferSoA::isSortedCanonical() const {
    if (canSkipSoAIteration() || activeCount <= 1u) {
        return true;
    }

    return countValidSlots() <= 1u;

bool PairBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;

bool PairBufferSoA::canSkipRefineIteration() const {
    return countValidSlots() == 0u;

u32 PairBufferSoA::compactAndClamp() {
    const PairBufferCompactAndClampPreflight preflight = preflightPairBufferCompactAndClamp(*this);
    if (preflight.reason == PairBufferCompactAndClampRejectReason::EmptyBuffer) {
    if (canSkipCompactAndClamp()) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    if (preflight.reason == PairBufferCompactAndClampRejectReason::NoWork) {
        return 0u;
    const PairBufferCompactionPreflight compactionPreflight = preflightPairBufferCompaction(*this);
    if (compactionPreflight.emptyBuffer) {
    if (canSkipPairBufferCompactAndClamp(*this)) {

    if (!preflightPairBufferCompactAndClamp(*this).needsCompactAndClamp()) {
    if (preflight.reason == PairBufferCompactAndClampRejectReason::AlreadyCompactAndWithinCapacity) {
    if (!preflightPairBufferCompactAndClamp(*this).needsWork()) {
    if (!shouldRunPairBufferCompactAndClamp(*this)) {
    if (preflightPairBufferCompaction(*this).needsCompaction()) {
        return false;
    if (!canSkipMaxCapacityClamp()) {
    // All-valid compaction may still resize when activeCount lags prepared slot storage.
    return pairSlotCount == 0u || activeCount == pairSlotCount;

        return activeCount;
    }

    compact();
    if (isEmpty()) {
        return 0u;
    }

    if (!isSortedCanonical()) {
        sortCanonical();
    return applyMaxCapacityClamp();
    if (shouldRunPairBufferClamp(*this)) {
    return activeCount;
}

bool PairBufferSoA::isSortedCanonical() const {
    if (canSkipSort()) {
bool PairBufferSoA::canSkipDedupe() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    const u32 validCount = countValidSlots();
    return validCount <= 1u;
}

bool PairBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;

bool PairBufferSoA::isSortedCanonical() const {
    if (canSkipSoAIteration()) {
        return true;

    const u32 validCount = countValidSlots();
    if (validCount <= 1u) {
        return true;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 prevBodyA = 0u;
    u32 prevBodyB = 0u;
    bool hasPrev = false;
    for (u32 i = 0; i < scanCount; ++i) {
        if (validFlags[i] == 0u) {
            continue;
        }

        const u32 currBodyA = bodyA[i];
        const u32 currBodyB = bodyB[i];
        if (hasPrev) {
            if (currBodyA < prevBodyA) {
                return false;
            }
            if (currBodyA == prevBodyA && currBodyB < prevBodyB) {
                return false;
            }
        }

        prevBodyA = currBodyA;
        prevBodyB = currBodyB;
        hasPrev = true;
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

u32 PairBufferSoA::countInvalidPairs(u32 bodyCount) const {
    if (canSkipSoAIteration()) {
        return 0u;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 invalidCount = 0u;
    for (u32 i = 0; i < scanCount; ++i) {
        if (validFlags[i] == 0u) {
            continue;
        }
        if (!isValidCandidatePair(bodyA[i], bodyB[i], bodyCount)) {
            ++invalidCount;
        }
    }
    return invalidCount;
}

u32 PairBufferSoA::pruneInvalidPairs(u32 bodyCount) {
    if (canSkipSoAIteration()) {
        return 0u;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    for (u32 i = 0; i < scanCount; ++i) {
        if (validFlags[i] == 0u) {
            continue;
        }
        if (!isValidCandidatePair(bodyA[i], bodyB[i], bodyCount)) {
            validFlags[i] = 0u;
        }
    }
    return compact();
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

bool PairBufferSoA::hasInvalidSlots() const {
    if (canSkipSoAIteration()) {
bool PairBufferSoA::canSkipPairBufferSort() const {
    return !preflightPairBufferSort(*this).needsSort();
}

bool PairBufferSoA::hasDuplicateCanonicalPairs() const {
    if (canSkipDedupe()) {
        return false;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    for (u32 slot = 0; slot < scanCount; ++slot) {
        if (validFlags[slot] == 0u) {
            return true;
    for (u32 outer = 0; outer < scanCount; ++outer) {
        if (!slotIsValid(outer)) {
            continue;
        }
        for (u32 inner = outer + 1u; inner < scanCount; ++inner) {
            if (!slotIsValid(inner)) {
            if (bodyA[outer] == bodyA[inner] && bodyB[outer] == bodyB[inner]) {
                continue;
            }
                return true;
        }
    }
    return false;
}

bool PairBufferSoA::canSkipCompaction() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    return !hasInvalidSlots();
}

bool PairBufferSoA::canSkipDedupeAndClamp() const {
    return canSkipDedupe() && !canApplyMaxCapacityClamp();

PairBufferDedupePreflight preflight_dedupe_pair_buffer(const PairBufferSoA& buffer) {
    PairBufferDedupePreflight preflight{};
    if (buffer.canSkipDedupe()) {
        preflight.skipped = true;
        return preflight;
    preflight.activePairCount = buffer.activeCount;

bool should_skip_dedupe_pair_buffer(const PairBufferSoA& buffer) {
    return buffer.canSkipDedupe();

PairBufferClampPreflight preflight_pair_buffer_clamp(const PairBufferSoA& buffer) {
    PairBufferClampPreflight preflight{};
    if (buffer.canSkipSoAIteration()) {
    preflight.maxCapacity = buffer.maxCapacity;
    if (buffer.canApplyMaxCapacityClamp()) {
        preflight.excessCount = buffer.activeCount - buffer.maxCapacity;




}

bool PairBufferSoA::canSkipCompactAndClamp() const {
    if (canSkipSoAIteration()) {
        return true;
    }
    return canSkipCompaction() && canSkipMaxCapacityClamp();
}

bool PairBufferSoA::canSkipCompactAndClamp() const {
    return !preflightPairBufferCompactAndClamp(*this).needsCompactAndClamp();
}

bool PairBufferSoA::hasDuplicateCanonicalPairs() const {
    if (canSkipSoAIteration() || activeCount <= 1u) {
        return false;
    }

    for (u32 i = 0; i < activeCount; ++i) {
        if (!slotIsValid(i)) {
            continue;
        }
        for (u32 j = i + 1u; j < activeCount; ++j) {
            if (!slotIsValid(j)) {
                continue;
            }
            if (bodyA[i] == bodyA[j] && bodyB[i] == bodyB[j]) {
                return true;
            }
        }
    }
    return false;
}

bool PairBufferSoA::canSkipCompactAndClamp() const {
    return canSkipPairBufferCompactAndClamp(*this);
}

bool PairBufferSoA::canSkipCompactAndClamp() const {
    return !preflightPairBufferCompactAndClamp(*this).needsCompactAndClamp();
}

bool PairBufferSoA::slotIsValid(u32 slot) const {
    if (slot >= validFlags.size()) {
        return false;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return false;
    }
    return validFlags[slot] != 0u;
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
    if (buffer.isFull()) {
        return PairBufferPushRejectReason::AtCapacity;
    return PairBufferPushRejectReason::None;
    }

bool pairBufferPushRejectsForReason(
    const PairBufferSoA& buffer,
    u32 idxA,
    u32 idxB,
    PairBufferPushRejectReason expected) {
    return pairBufferPushRejectReason(buffer, idxA, idxB) == expected;

const char* pairBufferCompactionRejectReasonName(PairBufferCompactionRejectReason reason) {
    case PairBufferCompactionRejectReason::None:
}

    switch (reason) {

        return "None";
    case PairBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferCompactionRejectReason::AllValid:
        return "AllValid";
    return "Unknown";

PairBufferCompactionRejectReason pairBufferCompactionRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferCompactionRejectReason::EmptyBuffer;
    if (buffer.canSkipCompaction()) {
        return PairBufferCompactionRejectReason::AllValid;
    return PairBufferCompactionRejectReason::None;

bool pairBufferCompactionRejectsForReason(
    PairBufferCompactionRejectReason expected) {
    return pairBufferCompactionRejectReason(buffer) == expected;

const char* pairBufferClampRejectReasonName(PairBufferClampRejectReason reason) {
    case PairBufferClampRejectReason::None:
    case PairBufferClampRejectReason::EmptyBuffer:
    case PairBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";

PairBufferClampRejectReason pairBufferClampRejectReason(const PairBufferSoA& buffer) {
        return PairBufferClampRejectReason::EmptyBuffer;
    if (!buffer.canApplyMaxCapacityClamp()) {
        return PairBufferClampRejectReason::WithinCapacity;
    return PairBufferClampRejectReason::None;

bool pairBufferClampRejectsForReason(const PairBufferSoA& buffer, PairBufferClampRejectReason expected) {
    return pairBufferClampRejectReason(buffer) == expected;

const char* pairBufferDedupeRejectReasonName(PairBufferDedupeRejectReason reason) {
    case PairBufferDedupeRejectReason::None:
    case PairBufferDedupeRejectReason::EmptyBuffer:
    case PairBufferDedupeRejectReason::SinglePair:
        return "SinglePair";

PairBufferDedupeRejectReason pairBufferDedupeRejectReason(const PairBufferSoA& buffer) {
        return PairBufferDedupeRejectReason::EmptyBuffer;
    if (buffer.activeCount <= 1u) {
        return PairBufferDedupeRejectReason::SinglePair;
    return PairBufferDedupeRejectReason::None;

bool pairBufferDedupeRejectsForReason(const PairBufferSoA& buffer, PairBufferDedupeRejectReason expected) {
    return pairBufferDedupeRejectReason(buffer) == expected;

    const PairBufferSoA& buffer,







const char* pairBufferSortRejectReasonName(PairBufferSortRejectReason reason) {
    case PairBufferSortRejectReason::None:
    case PairBufferSortRejectReason::EmptyBuffer:
    case PairBufferSortRejectReason::SinglePair:

PairBufferSortRejectReason pairBufferSortRejectReason(const PairBufferSoA& buffer) {
        return PairBufferSortRejectReason::EmptyBuffer;
        return PairBufferSortRejectReason::SinglePair;
    return PairBufferSortRejectReason::None;

bool pairBufferSortRejectsForReason(const PairBufferSoA& buffer, PairBufferSortRejectReason expected) {
    return pairBufferSortRejectReason(buffer) == expected;


















PairBufferPushPreflight preflightPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    return preflightPairBufferPush(buffer, idxA, idxB, 0u);
}

PairBufferPushPreflight preflightPairBufferPush(
    const PairBufferSoA& buffer,
    u32 idxA,
    u32 idxB,
    u32 bodyCount) {
    PairBufferPushPreflight preflight{};
    preflight.reason = pairBufferPushRejectReason(buffer, idxA, idxB);
    preflight.invalidPair = preflight.reason == PairBufferPushRejectReason::InvalidPair;
    preflight.atCapacity = preflight.reason == PairBufferPushRejectReason::AtCapacity;
    preflight.invalidPair = !isValidCandidatePair(idxA, idxB, bodyCount);
    preflight.atCapacity = buffer.isFull();
    return preflight;

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

bool pairBufferCompactionRejectsForReason(const PairBufferSoA& buffer, PairBufferCompactionRejectReason expected) {
    return pairBufferCompactionRejectReason(buffer) == expected;
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

bool pairBufferCompactionRejectsForReason(const PairBufferSoA& buffer, PairBufferCompactionRejectReason expected) {
    return pairBufferCompactionRejectReason(buffer) == expected;
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

PairBufferWriteSlotPreflight preflightPairBufferWriteSlot(
    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,
    u32 idxB) {
    return preflightPairBufferWriteSlot(buffer, slot, idxA, idxB, 0u);
}

PairBufferWriteSlotPreflight preflightPairBufferWriteSlot(
    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,
    u32 idxB,
    u32 bodyCount) {
    PairBufferWriteSlotPreflight preflight{};
    preflight.invalidSlot = slot >= buffer.pairSlotCount;
    preflight.invalidPair = !isValidCandidatePair(idxA, idxB, bodyCount);
    return preflight;
}

PairBufferMergePreflight preflightPairBufferMerge(const PairBufferSoA& buffer, u32 incomingPairCount) {
    PairBufferMergePreflight preflight{};
    preflight.incomingCount = incomingPairCount;
    preflight.emptyIncoming = incomingPairCount == 0u;
    preflight.remainingCapacity = buffer.remainingCapacity();
    preflight.bufferAtCapacity = !preflight.emptyIncoming && preflight.remainingCapacity == 0u;
    preflight.wouldTruncate =
        !preflight.emptyIncoming && incomingPairCount > preflight.remainingCapacity;
    return preflight;
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

bool canSkipPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    return !preflightPairBufferPush(buffer, idxA, idxB).canPush();
}

bool canSkipPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    return !preflightPairBufferPush(buffer, idxA, idxB).canPush();
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

bool canSkipPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    return !preflightPairBufferPush(buffer, idxA, idxB).canPush();
}

PairBufferCompactionPreflight preflightPairBufferCompaction(const PairBufferSoA& buffer) {
    PairBufferCompactionPreflight preflight{};
    preflight.reason = pairBufferCompactionRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == PairBufferCompactionRejectReason::AllValid;

bool canSkipPairBufferCompaction(const PairBufferSoA& buffer) {
    return !preflightPairBufferCompaction(buffer).needsCompaction();

bool shouldRunPairBufferCompaction(const PairBufferSoA& buffer) {
    return preflightPairBufferCompaction(buffer).needsCompaction();
    return preflight;
}



    return !shouldRunPairBufferCompaction(buffer);






const char* pairBufferClampRejectReasonName(PairBufferClampRejectReason reason) {
    switch (reason) {
    case PairBufferClampRejectReason::None:
        return "None";
    case PairBufferClampRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";
    return "Unknown";
    }

PairBufferClampRejectReason pairBufferClampRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferClampRejectReason::EmptyBuffer;
    if (!buffer.canApplyMaxCapacityClamp()) {
        return PairBufferClampRejectReason::WithinCapacity;
    return PairBufferClampRejectReason::None;

bool pairBufferClampRejectsForReason(const PairBufferSoA& buffer, PairBufferClampRejectReason expected) {
    return pairBufferClampRejectReason(buffer) == expected;

const char* pairBufferCompactionRejectReasonName(PairBufferCompactionRejectReason reason) {
    case PairBufferCompactionRejectReason::None:
    case PairBufferCompactionRejectReason::EmptyBuffer:
    case PairBufferCompactionRejectReason::AllValid:
        return "AllValid";

PairBufferCompactionRejectReason pairBufferCompactionRejectReason(const PairBufferSoA& buffer) {
        return PairBufferCompactionRejectReason::EmptyBuffer;
    if (buffer.canSkipCompaction()) {
        return PairBufferCompactionRejectReason::AllValid;
    return PairBufferCompactionRejectReason::None;

bool pairBufferCompactionRejectsForReason(const PairBufferSoA& buffer, PairBufferCompactionRejectReason expected) {
    return pairBufferCompactionRejectReason(buffer) == expected;




bool pairBufferCompactionRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferCompactionRejectReason expected) {

bool canSkipPairBufferCompaction(const PairBufferSoA& buffer) {
    return !preflightPairBufferCompaction(buffer).needsCompaction();
}

PairBufferClampPreflight preflightPairBufferClamp(const PairBufferSoA& buffer) {
    PairBufferClampPreflight preflight{};
    preflight.reason = pairBufferClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == PairBufferClampRejectReason::WithinCapacity;

bool canSkipPairBufferClamp(const PairBufferSoA& buffer) {
    return !preflightPairBufferClamp(buffer).needsClamp();

bool shouldRunPairBufferClamp(const PairBufferSoA& buffer) {
    return preflightPairBufferClamp(buffer).needsClamp();
    return preflight;
}



    return !shouldRunPairBufferClamp(buffer);



const char* pairBufferDedupeRejectReasonName(PairBufferDedupeRejectReason reason) {
    switch (reason) {
    case PairBufferDedupeRejectReason::None:
        return "None";
    case PairBufferDedupeRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferDedupeRejectReason::SinglePair:
        return "SinglePair";
    return "Unknown";
    }

PairBufferDedupeRejectReason pairBufferDedupeRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferDedupeRejectReason::EmptyBuffer;
    if (buffer.activeCount <= 1u) {
        return PairBufferDedupeRejectReason::SinglePair;
    return PairBufferDedupeRejectReason::None;

bool pairBufferDedupeRejectsForReason(const PairBufferSoA& buffer, PairBufferDedupeRejectReason expected) {
    return pairBufferDedupeRejectReason(buffer) == expected;



const char* pairBufferClampRejectReasonName(PairBufferClampRejectReason reason) {
    case PairBufferClampRejectReason::None:
    case PairBufferClampRejectReason::EmptyBuffer:
    case PairBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";

PairBufferClampRejectReason pairBufferClampRejectReason(const PairBufferSoA& buffer) {
        return PairBufferClampRejectReason::EmptyBuffer;
    if (!buffer.canApplyMaxCapacityClamp()) {
        return PairBufferClampRejectReason::WithinCapacity;
    return PairBufferClampRejectReason::None;

bool pairBufferClampRejectsForReason(const PairBufferSoA& buffer, PairBufferClampRejectReason expected) {
    return pairBufferClampRejectReason(buffer) == expected;




bool pairBufferClampRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferClampRejectReason expected) {

bool canSkipPairBufferClamp(const PairBufferSoA& buffer) {
    return !preflightPairBufferClamp(buffer).needsClamp();
}

PairBufferDedupePreflight preflightPairBufferDedupe(const PairBufferSoA& buffer) {
    PairBufferDedupePreflight preflight{};
    preflight.reason = pairBufferDedupeRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferDedupeRejectReason::EmptyBuffer;
    preflight.singlePair = preflight.reason == PairBufferDedupeRejectReason::SinglePair;
    return preflight;
}

bool shouldRunPairBufferDedupe(const PairBufferSoA& buffer) {
    return preflightPairBufferDedupe(buffer).canDedupe();
const char* pairBufferDedupeRejectReasonName(PairBufferDedupeRejectReason reason) {
    switch (reason) {
    case PairBufferDedupeRejectReason::None:
        return "None";
    case PairBufferDedupeRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferDedupeRejectReason::SinglePair:
        return "SinglePair";
    case PairBufferDedupeRejectReason::AlreadyUnique:
        return "AlreadyUnique";
    }
    return "Unknown";
}

PairBufferDedupeRejectReason pairBufferDedupeRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferDedupeRejectReason::EmptyBuffer;
    if (buffer.activeCount <= 1u) {
        return PairBufferDedupeRejectReason::SinglePair;
    }
    if (!buffer.hasDuplicateCanonicalPairs()) {
        return PairBufferDedupeRejectReason::AlreadyUnique;
    return PairBufferDedupeRejectReason::None;

bool pairBufferDedupeRejectsForReason(const PairBufferSoA& buffer, PairBufferDedupeRejectReason expected) {
    }

bool pairBufferDedupeRejectsForReason(
    const PairBufferSoA& buffer,
    PairBufferDedupeRejectReason expected) {
    return pairBufferDedupeRejectReason(buffer) == expected;
}

PairBufferPushPreflight preflightPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    PairBufferPushPreflight preflight{};
    preflight.reason = pairBufferPushRejectReason(buffer, idxA, idxB);
    preflight.invalidPair = preflight.reason == PairBufferPushRejectReason::InvalidPair;
    preflight.atCapacity = preflight.reason == PairBufferPushRejectReason::AtCapacity;
    return preflight;
}

bool canSkipPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    return !preflightPairBufferPush(buffer, idxA, idxB).canPush();

bool shouldRunPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    return preflightPairBufferPush(buffer, idxA, idxB).canPush();
const char* pairBufferAcceptPairsRejectReasonName(PairBufferAcceptPairsRejectReason reason) {
    switch (reason) {
    case PairBufferAcceptPairsRejectReason::None:
        return "None";
    case PairBufferAcceptPairsRejectReason::AtCapacity:
        return "AtCapacity";
    }
    return "Unknown";

PairBufferAcceptPairsRejectReason pairBufferAcceptPairsRejectReason(
    const PairBufferSoA& buffer,
    u32 additionalCount) {
    if (additionalCount == 0u || buffer.canAcceptPairs(additionalCount)) {
        return PairBufferAcceptPairsRejectReason::None;
    return PairBufferAcceptPairsRejectReason::AtCapacity;

bool pairBufferAcceptPairsRejectsForReason(
    u32 additionalCount,
    PairBufferAcceptPairsRejectReason expected) {
    return pairBufferAcceptPairsRejectReason(buffer, additionalCount) == expected;

PairBufferAcceptPairsPreflight preflightPairBufferAcceptPairs(const PairBufferSoA& buffer, u32 additionalCount) {
    PairBufferAcceptPairsPreflight preflight{};
    preflight.reason = pairBufferAcceptPairsRejectReason(buffer, additionalCount);
    preflight.atCapacity = preflight.reason == PairBufferAcceptPairsRejectReason::AtCapacity;
    return preflight;

bool canSkipPairBufferAcceptPairs(const PairBufferSoA& buffer, u32 additionalCount) {
    return !preflightPairBufferAcceptPairs(buffer, additionalCount).canAccept();

bool shouldRunPairBufferAcceptPairs(const PairBufferSoA& buffer, u32 additionalCount) {
    return preflightPairBufferAcceptPairs(buffer, additionalCount).canAccept();



const char* pairBufferWriteSlotRejectReasonName(PairBufferWriteSlotRejectReason reason) {
    case PairBufferWriteSlotRejectReason::None:
    case PairBufferWriteSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case PairBufferWriteSlotRejectReason::InvalidPair:
const char* pairBufferWriteRejectReasonName(PairBufferWriteRejectReason reason) {
    case PairBufferWriteRejectReason::None:
    case PairBufferWriteRejectReason::OutOfRangeSlot:
    case PairBufferWriteRejectReason::InvalidPair:
        return "InvalidPair";

PairBufferWriteSlotRejectReason pairBufferWriteSlotRejectReason(
PairBufferWriteRejectReason pairBufferWriteRejectReason(






    u32 slot,
    u32 idxA,
    u32 idxB) {
    if (slot >= buffer.pairSlotCount) {
        return PairBufferWriteSlotRejectReason::OutOfRangeSlot;
    if (!isValidCandidatePair(idxA, idxB)) {
        return PairBufferWriteSlotRejectReason::InvalidPair;
    return PairBufferWriteSlotRejectReason::None;

bool pairBufferWriteSlotRejectsForReason(
    u32 idxB,
    PairBufferWriteSlotRejectReason expected) {
    return pairBufferWriteSlotRejectReason(buffer, slot, idxA, idxB) == expected;

PairBufferWriteSlotPreflight preflightPairBufferWriteSlot(






    if (buffer.pairSlotCount > 0u && slot >= buffer.pairSlotCount) {




    }

    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,

    u32 idxB) {
    PairBufferWriteSlotPreflight preflight{};
    preflight.reason = pairBufferWriteSlotRejectReason(buffer, slot, idxA, idxB);
    preflight.outOfRangeSlot = preflight.reason == PairBufferWriteSlotRejectReason::OutOfRangeSlot;
    preflight.invalidPair = preflight.reason == PairBufferWriteSlotRejectReason::InvalidPair;





bool canSkipPairBufferWriteSlot(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    return !preflightPairBufferWriteSlot(buffer, slot, idxA, idxB).canWrite();


bool shouldRunPairBufferWriteSlot(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    return preflightPairBufferWriteSlot(buffer, slot, idxA, idxB).canWrite();


const char* pairBufferAcceptRejectReasonName(PairBufferAcceptRejectReason reason) {
    case PairBufferAcceptRejectReason::None:
    case PairBufferAcceptRejectReason::ExceedsCapacity:
        return "ExceedsCapacity";

PairBufferAcceptRejectReason pairBufferAcceptRejectReason(const PairBufferSoA& buffer, u32 additionalCount) {
        return PairBufferAcceptRejectReason::None;
    return PairBufferAcceptRejectReason::ExceedsCapacity;

bool pairBufferAcceptRejectsForReason(
    PairBufferAcceptRejectReason expected) {
    return pairBufferAcceptRejectReason(buffer, additionalCount) == expected;

PairBufferAcceptPreflight preflightPairBufferAccept(const PairBufferSoA& buffer, u32 additionalCount) {
    PairBufferAcceptPreflight preflight{};
    preflight.reason = pairBufferAcceptRejectReason(buffer, additionalCount);
    preflight.exceedsCapacity = preflight.reason == PairBufferAcceptRejectReason::ExceedsCapacity;

bool canSkipPairBufferAccept(const PairBufferSoA& buffer, u32 additionalCount) {
    return !preflightPairBufferAccept(buffer, additionalCount).canAccept();

bool shouldRunPairBufferAccept(const PairBufferSoA& buffer, u32 additionalCount) {
    return preflightPairBufferAccept(buffer, additionalCount).canAccept();
        return PairBufferWriteRejectReason::OutOfRangeSlot;
    if (slot >= buffer.validFlags.size()) {
        return PairBufferWriteRejectReason::InvalidPair;
    return PairBufferWriteRejectReason::None;

bool pairBufferWriteRejectsForReason(
    PairBufferWriteRejectReason expected) {
    return pairBufferWriteRejectReason(buffer, slot, idxA, idxB) == expected;

PairBufferWritePreflight preflightPairBufferWrite(
    if (buffer.pairSlotCount == 0u || slot >= buffer.pairSlotCount) {



PairBufferWritePreflight preflightPairBufferWrite(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    case PairBufferWriteRejectReason::UnpreparedSlots:
        return "UnpreparedSlots";

    if (buffer.pairSlotCount == 0u) {
        return PairBufferWriteRejectReason::UnpreparedSlots;


    }

    const PairBufferSoA& buffer,
    u32 slot,
    u32 idxA,



    u32 idxB) {
    PairBufferWritePreflight preflight{};
    preflight.reason = pairBufferWriteRejectReason(buffer, slot, idxA, idxB);
    preflight.outOfRangeSlot = preflight.reason == PairBufferWriteRejectReason::OutOfRangeSlot;
    preflight.invalidPair = preflight.reason == PairBufferWriteRejectReason::InvalidPair;

bool canSkipPairBufferWrite(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    return !preflightPairBufferWrite(buffer, slot, idxA, idxB).canWrite();

bool shouldRunPairBufferWrite(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    return preflightPairBufferWrite(buffer, slot, idxA, idxB).canWrite();

const char* pairBufferInvalidateRejectReasonName(PairBufferInvalidateRejectReason reason) {
    case PairBufferInvalidateRejectReason::None:
    case PairBufferInvalidateRejectReason::OutOfRangeSlot:
    case PairBufferInvalidateRejectReason::AlreadyInvalid:
        return "AlreadyInvalid";

PairBufferInvalidateRejectReason pairBufferInvalidateRejectReason(const PairBufferSoA& buffer, u32 slot) {
        return PairBufferInvalidateRejectReason::OutOfRangeSlot;
    if (buffer.validFlags[slot] == 0u) {
        return PairBufferInvalidateRejectReason::AlreadyInvalid;
    return PairBufferInvalidateRejectReason::None;

bool pairBufferInvalidateRejectsForReason(





    PairBufferInvalidateRejectReason expected) {
    return pairBufferInvalidateRejectReason(buffer, slot) == expected;





    preflight.unpreparedSlots = preflight.reason == PairBufferWriteRejectReason::UnpreparedSlots;





    case PairBufferWriteRejectReason::NoPreparedSlots:
        return "NoPreparedSlots";

        return PairBufferWriteRejectReason::NoPreparedSlots;


    preflight.noPreparedSlots = preflight.reason == PairBufferWriteRejectReason::NoPreparedSlots;



    case PairBufferInvalidateRejectReason::NoPreparedSlots:

        return PairBufferInvalidateRejectReason::NoPreparedSlots;
    if (slot >= buffer.validFlags.size() || slot >= buffer.pairSlotCount) {

    case PairBufferWriteRejectReason::NoSlotStorage:
        return "NoSlotStorage";

        return PairBufferWriteRejectReason::NoSlotStorage;


    preflight.noSlotStorage = preflight.reason == PairBufferWriteRejectReason::NoSlotStorage;



    case PairBufferInvalidateRejectReason::NoSlotStorage:

        return PairBufferInvalidateRejectReason::NoSlotStorage;
    if (slot >= buffer.pairSlotCount || slot >= buffer.validFlags.size()) {

    return preflight;



    switch (reason) {
        return "None";
        return "OutOfRangeSlot";
    return "Unknown";








PairBufferInvalidatePreflight preflightPairBufferInvalidate(const PairBufferSoA& buffer, u32 slot) {




    if (!buffer.slotIsValid(slot)) {


PairBufferInvalidatePreflight preflightPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    PairBufferInvalidatePreflight preflight{};
    preflight.reason = pairBufferInvalidateRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == PairBufferInvalidateRejectReason::OutOfRangeSlot;
    preflight.alreadyInvalid = preflight.reason == PairBufferInvalidateRejectReason::AlreadyInvalid;

bool canSkipPairBufferInvalidate(const PairBufferSoA& buffer, u32 slot) {
    return !preflightPairBufferInvalidate(buffer, slot).canInvalidate();

bool shouldRunPairBufferInvalidate(const PairBufferSoA& buffer, u32 slot) {
    return preflightPairBufferInvalidate(buffer, slot).canInvalidate();





const char* pairBufferInvalidateSlotRejectReasonName(PairBufferInvalidateSlotRejectReason reason) {
    case PairBufferInvalidateSlotRejectReason::None:
    case PairBufferInvalidateSlotRejectReason::OutOfRangeSlot:

PairBufferInvalidateSlotRejectReason pairBufferInvalidateSlotRejectReason(
    u32 slot) {
        return PairBufferInvalidateSlotRejectReason::OutOfRangeSlot;
    return PairBufferInvalidateSlotRejectReason::None;

bool pairBufferInvalidateSlotRejectsForReason(
    PairBufferInvalidateSlotRejectReason expected) {
    return pairBufferInvalidateSlotRejectReason(buffer, slot) == expected;
    case PairBufferWriteSlotRejectReason::UnpreparedBuffer:
        return "UnpreparedBuffer";

        return PairBufferWriteSlotRejectReason::UnpreparedBuffer;


    preflight.unpreparedBuffer = preflight.reason == PairBufferWriteSlotRejectReason::UnpreparedBuffer;









PairBufferInvalidateSlotRejectReason pairBufferInvalidateSlotRejectReason(const PairBufferSoA& buffer, u32 slot) {







    case PairBufferInvalidateSlotRejectReason::AlreadyInvalid:

    if (buffer.validFlags.empty() || slot >= buffer.validFlags.size()) {
        return PairBufferInvalidateSlotRejectReason::AlreadyInvalid;


PairBufferInvalidateSlotPreflight preflightPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    PairBufferInvalidateSlotPreflight preflight{};
    preflight.reason = pairBufferInvalidateSlotRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == PairBufferInvalidateSlotRejectReason::OutOfRangeSlot;

bool canSkipPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    return !preflightPairBufferInvalidateSlot(buffer, slot).canInvalidate();

bool shouldRunPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    return preflightPairBufferInvalidateSlot(buffer, slot).canInvalidate();




    return preflight;
}






    preflight.noPreparedSlots = preflight.reason == PairBufferInvalidateRejectReason::NoPreparedSlots;


    preflight.noSlotStorage = preflight.reason == PairBufferInvalidateRejectReason::NoSlotStorage;


    preflight.alreadyInvalid = preflight.reason == PairBufferInvalidateSlotRejectReason::AlreadyInvalid;









PairBufferCompactionPreflight preflightPairBufferCompaction(const PairBufferSoA& buffer) {
    PairBufferCompactionPreflight preflight{};
    preflight.reason = pairBufferCompactionRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == PairBufferCompactionRejectReason::AllValid;

bool canSkipPairBufferCompaction(const PairBufferSoA& buffer) {
    return !preflightPairBufferCompaction(buffer).needsCompaction();

bool shouldRunPairBufferCompaction(const PairBufferSoA& buffer) {
    return preflightPairBufferCompaction(buffer).needsCompaction();
    if (preflightPairBufferCompaction(buffer).needsCompaction()) {
        return true;
    }
    // All-valid prepared slots still need compact to sync dense activeCount.
    return buffer.pairSlotCount > 0u && buffer.activeCount != buffer.countValidSlots();

PairBufferClampPreflight preflightPairBufferClamp(const PairBufferSoA& buffer) {
    PairBufferClampPreflight preflight{};
    preflight.reason = pairBufferClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == PairBufferClampRejectReason::WithinCapacity;

bool canSkipPairBufferClamp(const PairBufferSoA& buffer) {
    return !preflightPairBufferClamp(buffer).needsClamp();

bool shouldRunPairBufferClamp(const PairBufferSoA& buffer) {
    return preflightPairBufferClamp(buffer).needsClamp();

PairBufferDedupePreflight preflightPairBufferDedupe(const PairBufferSoA& buffer) {
    PairBufferDedupePreflight preflight{};
    preflight.reason = pairBufferDedupeRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferDedupeRejectReason::EmptyBuffer;
    preflight.singlePair = preflight.reason == PairBufferDedupeRejectReason::SinglePair;

PairBufferSortPreflight preflightPairBufferSort(const PairBufferSoA& buffer) {
    PairBufferSortPreflight preflight{};
    preflight.reason = pairBufferSortRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferSortRejectReason::EmptyBuffer;
    preflight.singlePair = preflight.reason == PairBufferSortRejectReason::SinglePair;
    preflight.alreadyUnique = preflight.reason == PairBufferDedupeRejectReason::AlreadyUnique;
    return preflight;
}

bool canSkipPairBufferDedupe(const PairBufferSoA& buffer) {
    return !preflightPairBufferDedupe(buffer).canDedupe();

bool shouldRunPairBufferDedupe(const PairBufferSoA& buffer) {
    return preflightPairBufferDedupe(buffer).canDedupe();

const char* pairBufferSortRejectReasonName(PairBufferSortRejectReason reason) {
    case PairBufferSortRejectReason::None:
    case PairBufferSortRejectReason::EmptyBuffer:
    case PairBufferSortRejectReason::SinglePair:

PairBufferSortRejectReason pairBufferSortRejectReason(const PairBufferSoA& buffer) {
        return PairBufferSortRejectReason::EmptyBuffer;
        return PairBufferSortRejectReason::SinglePair;
    return PairBufferSortRejectReason::None;

bool pairBufferSortRejectsForReason(const PairBufferSoA& buffer, PairBufferSortRejectReason expected) {
    return pairBufferSortRejectReason(buffer) == expected;

    return !shouldRunPairBufferDedupe(buffer);
}

    switch (reason) {
        return "None";
        return "EmptyBuffer";
        return "SinglePair";
    return "Unknown";

    if (buffer.canSkipSoAIteration()) {
    if (buffer.activeCount <= 1u) {

}

bool shouldRunPairBufferDedupe(const PairBufferSoA& buffer) {
    return preflightPairBufferDedupe(buffer).canDedupe();
}

bool shouldRunPairBufferDedupe(const PairBufferSoA& buffer) {
    return !canSkipPairBufferDedupe(buffer);
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

const char* pairBufferSortRejectReasonName(PairBufferSortRejectReason reason) {
    switch (reason) {
    case PairBufferSortRejectReason::None:
        return "None";
    case PairBufferSortRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferSortRejectReason::SinglePair:
        return "SinglePair";
    case PairBufferSortRejectReason::AlreadySorted:
        return "AlreadySorted";
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
    if (buffer.isSortedCanonical()) {
        return PairBufferSortRejectReason::AlreadySorted;
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
    preflight.alreadySorted = preflight.reason == PairBufferSortRejectReason::AlreadySorted;
    return preflight;
}

bool canSkipPairBufferSort(const PairBufferSoA& buffer) {
    return !preflightPairBufferSort(buffer).needsSort();

bool shouldRunPairBufferSort(const PairBufferSoA& buffer) {
    return preflightPairBufferSort(buffer).needsSort();

const char* pairBufferCompactAndClampRejectReasonName(PairBufferCompactAndClampRejectReason reason) {
    case PairBufferCompactAndClampRejectReason::None:
    case PairBufferCompactAndClampRejectReason::EmptyBuffer:
    case PairBufferCompactAndClampRejectReason::NoWork:
        return "NoWork";

PairBufferCompactAndClampRejectReason pairBufferCompactAndClampRejectReason(const PairBufferSoA& buffer) {
        return PairBufferCompactAndClampRejectReason::EmptyBuffer;
    if (!shouldRunPairBufferCompaction(buffer) && !shouldRunPairBufferClamp(buffer)) {
        const u32 validCount = buffer.countValidSlots();
        if (buffer.pairSlotCount > 0u && buffer.activeCount != validCount) {
            return PairBufferCompactAndClampRejectReason::None;
        if (buffer.activeCount != validCount) {
        return PairBufferCompactAndClampRejectReason::NoWork;

bool pairBufferCompactAndClampRejectsForReason(
    PairBufferCompactAndClampRejectReason expected) {
    return pairBufferCompactAndClampRejectReason(buffer) == expected;

PairBufferCompactAndClampPreflight preflightPairBufferCompactAndClamp(const PairBufferSoA& buffer) {
    PairBufferCompactAndClampPreflight preflight{};
    preflight.reason = pairBufferCompactAndClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == PairBufferCompactAndClampRejectReason::NoWork;

bool canSkipPairBufferCompactAndClamp(const PairBufferSoA& buffer) {
    return !preflightPairBufferCompactAndClamp(buffer).needsCompactAndClamp();

bool shouldRunPairBufferCompactAndClamp(const PairBufferSoA& buffer) {
    return preflightPairBufferCompactAndClamp(buffer).needsCompactAndClamp();

const char* pairBufferWriteSlotRejectReasonName(PairBufferWriteSlotRejectReason reason) {
    case PairBufferWriteSlotRejectReason::None:
    case PairBufferWriteSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case PairBufferWriteSlotRejectReason::InvalidPair:

PairBufferWriteSlotRejectReason pairBufferWriteSlotRejectReason(
    u32 slot,
    u32 idxB) {
    if (slot >= buffer.pairSlotCount) {
        return PairBufferWriteSlotRejectReason::OutOfRangeSlot;
        return PairBufferWriteSlotRejectReason::InvalidPair;
    return PairBufferWriteSlotRejectReason::None;

bool pairBufferWriteSlotRejectsForReason(
    PairBufferWriteSlotRejectReason expected) {
    return pairBufferWriteSlotRejectReason(buffer, slot, idxA, idxB) == expected;

PairBufferWriteSlotPreflight preflightPairBufferWriteSlot(
bool shouldRunPairBufferDedupe(const PairBufferSoA& buffer) {
    return preflightPairBufferDedupe(buffer).canDedupe();
}

    switch (reason) {
        return "None";
        return "InvalidPair";
    return "Unknown";

    const PairBufferSoA& buffer,
    u32 idxA,
    if (!isValidCandidatePair(idxA, idxB)) {

    u32 idxB,

const char* pairBufferWriteSlotRejectReasonName(PairBufferWriteSlotRejectReason reason) {
    case PairBufferWriteSlotRejectReason::None:
    case PairBufferWriteSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case PairBufferWriteSlotRejectReason::InvalidPair:
    }

PairBufferWriteSlotRejectReason pairBufferWriteSlotRejectReason(
    u32 slot,
    u32 idxB) {
    if (slot >= buffer.pairSlotCount) {
        return PairBufferWriteSlotRejectReason::OutOfRangeSlot;
        return PairBufferWriteSlotRejectReason::InvalidPair;
    return PairBufferWriteSlotRejectReason::None;

bool pairBufferWriteSlotRejectsForReason(
    PairBufferWriteSlotRejectReason expected) {
    return pairBufferWriteSlotRejectReason(buffer, slot, idxA, idxB) == expected;

PairBufferWriteSlotPreflight preflightPairBufferWriteSlot(



















    if (buffer.pairSlotCount > 0u && slot >= buffer.pairSlotCount) {
    if (slot >= buffer.validFlags.size()) {












    if (buffer.pairSlotCount == 0u || slot >= buffer.pairSlotCount) {























    PairBufferWriteSlotPreflight preflight{};
    preflight.reason = pairBufferWriteSlotRejectReason(buffer, slot, idxA, idxB);
    preflight.outOfRangeSlot = preflight.reason == PairBufferWriteSlotRejectReason::OutOfRangeSlot;
    preflight.invalidPair = preflight.reason == PairBufferWriteSlotRejectReason::InvalidPair;

bool canSkipPairBufferWriteSlot(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    return !preflightPairBufferWriteSlot(buffer, slot, idxA, idxB).canWrite();

bool shouldRunPairBufferWriteSlot(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    return preflightPairBufferWriteSlot(buffer, slot, idxA, idxB).canWrite();

bool wouldSkipPairBufferWriteSlot(
    PairBufferWriteSlotRejectReason* reason) {
    const PairBufferWriteSlotRejectReason reject = pairBufferWriteSlotRejectReason(buffer, slot, idxA, idxB);
    if (reason != nullptr) {
        *reason = reject;
    return reject != PairBufferWriteSlotRejectReason::None;

const char* pairBufferInvalidateSlotRejectReasonName(PairBufferInvalidateSlotRejectReason reason) {
    case PairBufferInvalidateSlotRejectReason::None:
    case PairBufferInvalidateSlotRejectReason::OutOfRangeSlot:
    case PairBufferInvalidateSlotRejectReason::AlreadyInvalid:
        return "AlreadyInvalid";

PairBufferInvalidateSlotRejectReason pairBufferInvalidateSlotRejectReason(const PairBufferSoA& buffer, u32 slot) {
    if (slot >= buffer.pairSlotCount || slot >= buffer.validFlags.size()) {
        return PairBufferInvalidateSlotRejectReason::OutOfRangeSlot;
    if (buffer.validFlags[slot] == 0u) {
        return PairBufferInvalidateSlotRejectReason::AlreadyInvalid;
    return PairBufferInvalidateSlotRejectReason::None;

bool pairBufferInvalidateSlotRejectsForReason(
    PairBufferInvalidateSlotRejectReason expected) {
    return pairBufferInvalidateSlotRejectReason(buffer, slot) == expected;
    return preflight;









PairBufferInvalidateSlotRejectReason pairBufferInvalidateSlotRejectReason(
    u32 slot) {
    if (!buffer.slotIsValid(slot)) {

    case PairBufferWriteSlotRejectReason::OutOfSlot:
        return "OutOfSlot";

        return PairBufferWriteSlotRejectReason::OutOfSlot;


    preflight.outOfSlot = preflight.reason == PairBufferWriteSlotRejectReason::OutOfSlot;



    case PairBufferInvalidateSlotRejectReason::OutOfSlot:

        return PairBufferInvalidateSlotRejectReason::OutOfSlot;

















const char* pairBufferSlotWriteRejectReasonName(PairBufferSlotWriteRejectReason reason) {
    case PairBufferSlotWriteRejectReason::None:
    case PairBufferSlotWriteRejectReason::OutOfRangeSlot:
    case PairBufferSlotWriteRejectReason::InvalidPair:

PairBufferSlotWriteRejectReason pairBufferSlotWriteRejectReason(
        return PairBufferSlotWriteRejectReason::OutOfRangeSlot;
        return PairBufferSlotWriteRejectReason::InvalidPair;
    return PairBufferSlotWriteRejectReason::None;

bool pairBufferSlotWriteRejectsForReason(
    PairBufferSlotWriteRejectReason expected) {
    return pairBufferSlotWriteRejectReason(buffer, slot, idxA, idxB) == expected;

PairBufferSlotWritePreflight preflightPairBufferSlotWrite(



    PairBufferSlotWritePreflight preflight{};
    preflight.reason = pairBufferSlotWriteRejectReason(buffer, slot, idxA, idxB);
    preflight.outOfRangeSlot = preflight.reason == PairBufferSlotWriteRejectReason::OutOfRangeSlot;
    preflight.invalidPair = preflight.reason == PairBufferSlotWriteRejectReason::InvalidPair;
















































    case PairBufferWriteSlotRejectReason::UnpreparedBuffer:
        return "UnpreparedBuffer";

    if (buffer.pairSlotCount == 0u) {
        return PairBufferWriteSlotRejectReason::UnpreparedBuffer;


    preflight.unpreparedBuffer = preflight.reason == PairBufferWriteSlotRejectReason::UnpreparedBuffer;




    if (buffer.validFlags.empty() || slot >= buffer.validFlags.size()) {

}



    switch (reason) {
        return "None";
        return "OutOfRangeSlot";
    return "Unknown";

    const PairBufferSoA& buffer,
    if (slot >= buffer.validFlags.size()) {
    if (buffer.pairSlotCount > 0u && slot >= buffer.pairSlotCount) {

    u32 slot,






PairBufferInvalidateSlotPreflight preflightPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    PairBufferInvalidateSlotPreflight preflight{};
    preflight.reason = pairBufferInvalidateSlotRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == PairBufferInvalidateSlotRejectReason::OutOfRangeSlot;
    preflight.alreadyInvalid = preflight.reason == PairBufferInvalidateSlotRejectReason::AlreadyInvalid;

bool canSkipPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    return !preflightPairBufferInvalidateSlot(buffer, slot).canInvalidate();

bool shouldRunPairBufferInvalidateSlot(const PairBufferSoA& buffer, u32 slot) {
    return preflightPairBufferInvalidateSlot(buffer, slot).canInvalidate();

bool wouldSkipPairBufferInvalidateSlot(
    PairBufferInvalidateSlotRejectReason* reason) {
    const PairBufferInvalidateSlotRejectReason reject = pairBufferInvalidateSlotRejectReason(buffer, slot);
    return reject != PairBufferInvalidateSlotRejectReason::None;

const char* pairBufferToVectorRejectReasonName(PairBufferToVectorRejectReason reason) {
    case PairBufferToVectorRejectReason::None:
    case PairBufferToVectorRejectReason::EmptyBuffer:

PairBufferToVectorRejectReason pairBufferToVectorRejectReason(const PairBufferSoA& buffer) {
        return PairBufferToVectorRejectReason::EmptyBuffer;
    return PairBufferToVectorRejectReason::None;

bool pairBufferToVectorRejectsForReason(const PairBufferSoA& buffer, PairBufferToVectorRejectReason expected) {
    return pairBufferToVectorRejectReason(buffer) == expected;

PairBufferToVectorPreflight preflightPairBufferToVector(const PairBufferSoA& buffer) {
    PairBufferToVectorPreflight preflight{};
    preflight.reason = pairBufferToVectorRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferToVectorRejectReason::EmptyBuffer;

bool canSkipPairBufferToVector(const PairBufferSoA& buffer) {
    return !preflightPairBufferToVector(buffer).canExport();

bool shouldRunPairBufferToVector(const PairBufferSoA& buffer) {
    return preflightPairBufferToVector(buffer).canExport();






bool PairBufferPreflight::can_push(u32 additionalCount) const {
    if (skipped || additionalCount == 0u) {
        return !skipped;
    if (maxCapacity == 0u) {
        return true;
    return activeCount + additionalCount <= maxCapacity;

PairBufferPreflight preflight_pair_buffer(const PairBufferSoA& buffer) {
    PairBufferPreflight preflight{};
    preflight.activeCount = buffer.activeCount;
    preflight.maxCapacity = buffer.maxCapacity;
    preflight.droppedCount = buffer.droppedCount;
    preflight.remainingCapacity = buffer.remainingCapacity();
    preflight.empty = buffer.canSkipSoAIteration();
    preflight.full = buffer.isFull();
    preflight.skipDedupe = buffer.canSkipDedupe();
    preflight.skipCompaction = buffer.canSkipCompaction();
    preflight.needsClamp = buffer.needsMaxCapacityClamp();
    preflight.skipped = preflight.empty;

bool should_skip_pair_buffer_dedupe(const PairBufferSoA& buffer) {
    return preflight_pair_buffer(buffer).skipDedupe;

bool should_skip_pair_buffer_compaction(const PairBufferSoA& buffer) {
    return preflight_pair_buffer(buffer).skipCompaction;
    preflight.invalidPair = !isValidCandidatePair(idxA, idxB);
    preflight.atCapacity = buffer.isFull();

    preflight.emptyBuffer = buffer.canSkipSoAIteration();
    preflight.allValid = !preflight.emptyBuffer && buffer.canSkipCompaction();

    preflight.withinCapacity = preflight.emptyBuffer || !buffer.canApplyMaxCapacityClamp();


    return !shouldRunPairBufferSort(buffer);
const char* pairBufferPushRejectReasonName(PairBufferPushRejectReason reason) {
    case PairBufferPushRejectReason::None:
    case PairBufferPushRejectReason::InvalidPair:
    case PairBufferPushRejectReason::AtCapacity:
        return "AtCapacity";

PairBufferPushRejectReason pairBufferPushRejectReason(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
        return PairBufferPushRejectReason::InvalidPair;
    if (buffer.isFull()) {
        return PairBufferPushRejectReason::AtCapacity;
    return PairBufferPushRejectReason::None;

bool pairBufferPushRejectsForReason(
    PairBufferPushRejectReason expected) {
    return pairBufferPushRejectReason(buffer, idxA, idxB) == expected;

const char* pairBufferCompactionRejectReasonName(PairBufferCompactionRejectReason reason) {
    case PairBufferCompactionRejectReason::None:

    case PairBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case PairBufferCompactionRejectReason::AllValid:
        return "AllValid";

PairBufferCompactionRejectReason pairBufferCompactionRejectReason(const PairBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return PairBufferCompactionRejectReason::EmptyBuffer;
    if (buffer.canSkipCompaction()) {
        return PairBufferCompactionRejectReason::AllValid;
    return PairBufferCompactionRejectReason::None;

bool pairBufferCompactionRejectsForReason(
    PairBufferCompactionRejectReason expected) {
    return pairBufferCompactionRejectReason(buffer) == expected;

bool canSkipPairBufferCompaction(const PairBufferSoA& buffer) {
    return !preflightPairBufferCompaction(buffer).needsCompaction();

const char* pairBufferClampRejectReasonName(PairBufferClampRejectReason reason) {
    case PairBufferClampRejectReason::None:
    case PairBufferClampRejectReason::EmptyBuffer:
    case PairBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";

PairBufferClampRejectReason pairBufferClampRejectReason(const PairBufferSoA& buffer) {
        return PairBufferClampRejectReason::EmptyBuffer;
    if (buffer.canSkipMaxCapacityClamp()) {
        return PairBufferClampRejectReason::WithinCapacity;
    return PairBufferClampRejectReason::None;

bool pairBufferClampRejectsForReason(const PairBufferSoA& buffer, PairBufferClampRejectReason expected) {
    return pairBufferClampRejectReason(buffer) == expected;

bool canSkipPairBufferClamp(const PairBufferSoA& buffer) {
    return !preflightPairBufferClamp(buffer).needsClamp();

const char* pairBufferDedupeRejectReasonName(PairBufferDedupeRejectReason reason) {
    case PairBufferDedupeRejectReason::None:
    case PairBufferDedupeRejectReason::EmptyBuffer:
    case PairBufferDedupeRejectReason::SinglePair:
        return "SinglePair";

PairBufferDedupeRejectReason pairBufferDedupeRejectReason(const PairBufferSoA& buffer) {
        return PairBufferDedupeRejectReason::EmptyBuffer;
    if (buffer.activeCount <= 1u) {
        return PairBufferDedupeRejectReason::SinglePair;
    return PairBufferDedupeRejectReason::None;

bool pairBufferDedupeRejectsForReason(const PairBufferSoA& buffer, PairBufferDedupeRejectReason expected) {
    return pairBufferDedupeRejectReason(buffer) == expected;

        preflight.emptyBuffer = true;

    preflight.compactionNeeded = !buffer.canSkipCompaction();
    const u32 projectedCount =
        buffer.canSkipCompaction() ? buffer.countValidSlots() : buffer.countValidSlots();
    preflight.clampNeeded = buffer.maxCapacity > 0u && projectedCount > buffer.maxCapacity;


    const PairBufferCompactionPreflight compactionPreflight = preflightPairBufferCompaction(buffer);
    const PairBufferClampPreflight clampPreflight = preflightPairBufferClamp(buffer);
    preflight.emptyBuffer = compactionPreflight.emptyBuffer;
    preflight.needsCompaction = compactionPreflight.needsCompaction();
    preflight.needsClamp = clampPreflight.needsClamp();
    preflight.outOfRangeSlot = slot >= buffer.pairSlotCount;

PairBufferPrepareSlotsPreflight preflightPairBufferPrepareSlots(u32 slotCount) {
    PairBufferPrepareSlotsPreflight preflight{};
    preflight.zeroSlots = slotCount == 0u;

PairBufferMergePreflight preflightPairBufferMerge(const PairBufferSoA& buffer, u32 incomingCount) {
    PairBufferMergePreflight preflight{};
    preflight.emptyIncoming = incomingCount == 0u;
    if (preflight.emptyIncoming || preflight.atCapacity) {
        preflight.rejectedCount = incomingCount;

    const u32 remaining = buffer.remainingCapacity();
    if (remaining == UINT32_MAX) {
        preflight.acceptedCount = incomingCount;

    preflight.acceptedCount = incomingCount <= remaining ? incomingCount : remaining;
    preflight.rejectedCount = incomingCount - preflight.acceptedCount;


BroadphaseMergeIntoBufferPreflight preflightBroadphaseMergeIntoBuffer(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    u32 incomingPairCount) {
    BroadphaseMergeIntoBufferPreflight preflight{};
    preflight.merge = preflightBroadphaseMerge(bodies, shapes);
    preflight.incomingPairCount = incomingPairCount;
    preflight.buffer = preflightPairBufferMerge(buffer, incomingPairCount);

const char* pairBufferSortRejectReasonName(PairBufferSortRejectReason reason) {
    case PairBufferSortRejectReason::None:
    case PairBufferSortRejectReason::EmptyBuffer:
    case PairBufferSortRejectReason::SinglePair:

PairBufferSortRejectReason pairBufferSortRejectReason(const PairBufferSoA& buffer) {
        return PairBufferSortRejectReason::EmptyBuffer;
        return PairBufferSortRejectReason::SinglePair;
    return PairBufferSortRejectReason::None;

bool pairBufferSortRejectsForReason(
    PairBufferSortRejectReason expected) {
    return pairBufferSortRejectReason(buffer) == expected;

    return preflightPairBufferCompactAndClamp(buffer).emptyBuffer;

    return buffer.canSkipCompaction();

    return buffer.canSkipMaxCapacityClamp();













    const u32 projectedCount = buffer.countValidSlots();

    preflight.compaction = preflightPairBufferCompaction(buffer);
    preflight.clamp = preflightPairBufferClamp(buffer);

    return !preflightPairBufferCompactAndClamp(buffer).needsWork();




PairSlotPreflight preflightPairSlots(u32 slotCount, const PairBufferSoA& buffer) {
    PairSlotPreflight preflight{};
    preflight.slotCount = slotCount;
    if (slotCount == 0u) {
        preflight.skipped = true;
    if (buffer.maxCapacity > 0u && slotCount > buffer.maxCapacity) {
        preflight.exceedsBufferCapacity = true;

    case PairBufferCompactAndClampRejectReason::NoWorkNeeded:
        return "NoWorkNeeded";

    if (buffer.canSkipCompaction() && buffer.canSkipMaxCapacityClamp()) {
        return PairBufferCompactAndClampRejectReason::NoWorkNeeded;


    preflight.noWorkNeeded = preflight.reason == PairBufferCompactAndClampRejectReason::NoWorkNeeded;





    case PairBufferCompactAndClampRejectReason::AlreadyCompactAndWithinCapacity:
        return "AlreadyCompactAndWithinCapacity";

    if (buffer.canSkipCompactAndClamp()) {
        return PairBufferCompactAndClampRejectReason::AlreadyCompactAndWithinCapacity;


    preflight.alreadyCompactAndWithinCapacity =
        preflight.reason == PairBufferCompactAndClampRejectReason::AlreadyCompactAndWithinCapacity;















bool pairBufferSortRejectsForReason(const PairBufferSoA& buffer, PairBufferSortRejectReason expected) {
































const char* pairBufferSlotReservationRejectReasonName(PairBufferSlotReservationRejectReason reason) {
    case PairBufferSlotReservationRejectReason::None:


const char* pairBufferSlotInvalidateRejectReasonName(PairBufferSlotInvalidateRejectReason reason) {

    case PairBufferSlotInvalidateRejectReason::None:
    case PairBufferSlotInvalidateRejectReason::OutOfRangeSlot:

PairBufferSlotInvalidateRejectReason pairBufferSlotInvalidateRejectReason(const PairBufferSoA& buffer, u32 slot) {
        return PairBufferSlotInvalidateRejectReason::OutOfRangeSlot;
    return PairBufferSlotInvalidateRejectReason::None;

PairBufferSlotInvalidateRejectReason pairBufferSlotInvalidateRejectReason(

bool pairBufferSlotInvalidateRejectsForReason(
    PairBufferSlotInvalidateRejectReason expected) {
    return pairBufferSlotInvalidateRejectReason(buffer, slot) == expected;

PairBufferSlotInvalidatePreflight preflightPairBufferSlotInvalidate(const PairBufferSoA& buffer, u32 slot) {
    PairBufferSlotInvalidatePreflight preflight{};
    preflight.reason = pairBufferSlotInvalidateRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == PairBufferSlotInvalidateRejectReason::OutOfRangeSlot;

bool canSkipPairBufferSlotInvalidate(const PairBufferSoA& buffer, u32 slot) {
    return !preflightPairBufferSlotInvalidate(buffer, slot).canInvalidate();

bool shouldRunPairBufferSlotInvalidate(const PairBufferSoA& buffer, u32 slot) {
    return preflightPairBufferSlotInvalidate(buffer, slot).canInvalidate();




    return preflight;


    u32 slot) {


PairBufferSlotInvalidatePreflight preflightPairBufferSlotInvalidate(



    case PairBufferSlotReservationRejectReason::ZeroSlots:
        return "ZeroSlots";
    case PairBufferSlotReservationRejectReason::ExceedsCapacity:
        return "ExceedsCapacity";

PairBufferSlotReservationRejectReason pairBufferSlotReservationRejectReason(
    u32 slotCount) {
        return PairBufferSlotReservationRejectReason::ZeroSlots;
        return PairBufferSlotReservationRejectReason::ExceedsCapacity;
    return PairBufferSlotReservationRejectReason::None;

bool pairBufferSlotReservationRejectsForReason(
    u32 slotCount,
    PairBufferSlotReservationRejectReason expected) {
    return pairBufferSlotReservationRejectReason(buffer, slotCount) == expected;

PairBufferSlotReservationPreflight preflightPairBufferSlotReservation(












    }
    return "Unknown";

    const PairBufferSoA& buffer,


    PairBufferSlotReservationPreflight preflight{};
    preflight.requestedSlots = slotCount;
    preflight.reason = pairBufferSlotReservationRejectReason(buffer, slotCount);
    preflight.zeroSlots = preflight.reason == PairBufferSlotReservationRejectReason::ZeroSlots;
    preflight.exceedsCapacity = preflight.reason == PairBufferSlotReservationRejectReason::ExceedsCapacity;

bool canSkipPairBufferSlotReservation(const PairBufferSoA& buffer, u32 slotCount) {
    return !preflightPairBufferSlotReservation(buffer, slotCount).canReserve();

bool shouldRunPairBufferSlotReservation(const PairBufferSoA& buffer, u32 slotCount) {
    return preflightPairBufferSlotReservation(buffer, slotCount).canReserve();























    if (!preflightPairBufferCompaction(buffer).needsCompaction() &&
        !preflightPairBufferClamp(buffer).needsClamp()) {




    if (buffer.activeCount > 0u && canSkipPairBufferCompaction(buffer) && canSkipPairBufferClamp(buffer)) {








    const bool needsCompaction = shouldRunPairBufferCompaction(buffer);
    bool needsClamp = shouldRunPairBufferClamp(buffer);
    if (!needsClamp && buffer.maxCapacity > 0u && buffer.pairSlotCount > 0u) {
        needsClamp = buffer.countValidSlots() > buffer.maxCapacity;
    if (!needsCompaction && !needsClamp) {




    return preflightPairBufferCompactAndClamp(buffer).needsWork();








    preflight.needsCompaction = shouldRunPairBufferCompaction(buffer);
    preflight.needsClamp = shouldRunPairBufferClamp(buffer);






























const char* pairBufferMergeIntoRejectReasonName(PairBufferMergeIntoRejectReason reason) {
    case PairBufferMergeIntoRejectReason::None:
    case PairBufferMergeIntoRejectReason::EmptyInput:
        return "EmptyInput";
    case PairBufferMergeIntoRejectReason::BufferFull:
        return "BufferFull";

PairBufferMergeIntoRejectReason pairBufferMergeIntoRejectReason(
    u32 pairCount) {
    if (pairCount == 0u) {
        return PairBufferMergeIntoRejectReason::EmptyInput;
        return PairBufferMergeIntoRejectReason::BufferFull;
    return PairBufferMergeIntoRejectReason::None;

bool pairBufferMergeIntoRejectsForReason(
    u32 pairCount,
    PairBufferMergeIntoRejectReason expected) {
    return pairBufferMergeIntoRejectReason(buffer, pairCount) == expected;

PairBufferMergeIntoPreflight preflightPairBufferMergeInto(const PairBufferSoA& buffer, u32 pairCount) {
    PairBufferMergeIntoPreflight preflight{};
    preflight.reason = pairBufferMergeIntoRejectReason(buffer, pairCount);
    preflight.emptyInput = preflight.reason == PairBufferMergeIntoRejectReason::EmptyInput;
    preflight.bufferFull = preflight.reason == PairBufferMergeIntoRejectReason::BufferFull;

bool canSkipPairBufferMergeInto(const PairBufferSoA& buffer, u32 pairCount) {
    return !preflightPairBufferMergeInto(buffer, pairCount).canMerge();

bool shouldRunPairBufferMergeInto(const PairBufferSoA& buffer, u32 pairCount) {
    return preflightPairBufferMergeInto(buffer, pairCount).canMerge();




const char* pairBufferCompactClampRejectReasonName(PairBufferCompactClampRejectReason reason) {
    case PairBufferCompactClampRejectReason::None:
    case PairBufferCompactClampRejectReason::EmptyBuffer:
    case PairBufferCompactClampRejectReason::NoWork:

PairBufferCompactClampRejectReason pairBufferCompactClampRejectReason(const PairBufferSoA& buffer) {
        return PairBufferCompactClampRejectReason::EmptyBuffer;
        return PairBufferCompactClampRejectReason::NoWork;
    return PairBufferCompactClampRejectReason::None;

bool pairBufferCompactClampRejectsForReason(
    PairBufferCompactClampRejectReason expected) {
    return pairBufferCompactClampRejectReason(buffer) == expected;

PairBufferCompactClampPreflight preflightPairBufferCompactClamp(const PairBufferSoA& buffer) {
    PairBufferCompactClampPreflight preflight{};
    preflight.reason = pairBufferCompactClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferCompactClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == PairBufferCompactClampRejectReason::NoWork;
    preflight.needsCompaction = preflightPairBufferCompaction(buffer).needsCompaction();
    preflight.needsClamp = preflightPairBufferClamp(buffer).needsClamp();

bool canSkipPairBufferCompactClamp(const PairBufferSoA& buffer) {
    return !preflightPairBufferCompactClamp(buffer).canRun();

bool shouldRunPairBufferCompactClamp(const PairBufferSoA& buffer) {
    return preflightPairBufferCompactClamp(buffer).canRun();










    case PairBufferWriteSlotRejectReason::InvalidSlot:
        return "InvalidSlot";

        return PairBufferWriteSlotRejectReason::InvalidSlot;


    preflight.invalidSlot = preflight.reason == PairBufferWriteSlotRejectReason::InvalidSlot;







const char* pairBufferMergeRejectReasonName(PairBufferMergeRejectReason reason) {
    case PairBufferMergeRejectReason::None:
    case PairBufferMergeRejectReason::EmptyPairs:
        return "EmptyPairs";
    case PairBufferMergeRejectReason::AtCapacity:

PairBufferMergeRejectReason pairBufferMergeRejectReason(const PairBufferSoA& buffer, u32 pairCount) {
        return PairBufferMergeRejectReason::EmptyPairs;
        return PairBufferMergeRejectReason::AtCapacity;
    return PairBufferMergeRejectReason::None;

bool pairBufferMergeRejectsForReason(
    PairBufferMergeRejectReason expected) {
    return pairBufferMergeRejectReason(buffer, pairCount) == expected;

PairBufferMergePreflight preflightPairBufferMerge(const PairBufferSoA& buffer, u32 pairCount) {
    preflight.reason = pairBufferMergeRejectReason(buffer, pairCount);
    preflight.emptyPairs = preflight.reason == PairBufferMergeRejectReason::EmptyPairs;
    preflight.atCapacity = preflight.reason == PairBufferMergeRejectReason::AtCapacity;

bool canSkipPairBufferMerge(const PairBufferSoA& buffer, u32 pairCount) {
    return !preflightPairBufferMerge(buffer, pairCount).canMerge();

bool shouldRunPairBufferMerge(const PairBufferSoA& buffer, u32 pairCount) {
    return preflightPairBufferMerge(buffer, pairCount).canMerge();






const char* pairBufferAcceptPairsRejectReasonName(PairBufferAcceptPairsRejectReason reason) {
    case PairBufferAcceptPairsRejectReason::None:
    case PairBufferAcceptPairsRejectReason::ExceedsCapacity:
    case PairBufferAcceptPairsRejectReason::AtCapacity:

PairBufferAcceptPairsRejectReason pairBufferAcceptPairsRejectReason(
    u32 additionalCount) {
    if (additionalCount == 0u) {
        return PairBufferAcceptPairsRejectReason::None;
    if (buffer.maxCapacity > 0u && buffer.activeCount + additionalCount > buffer.maxCapacity) {
        return PairBufferAcceptPairsRejectReason::ExceedsCapacity;

bool pairBufferAcceptPairsRejectsForReason(
    u32 additionalCount,
    PairBufferAcceptPairsRejectReason expected) {
    return pairBufferAcceptPairsRejectReason(buffer, additionalCount) == expected;

PairBufferAcceptPairsPreflight preflightPairBufferAcceptPairs(
    PairBufferAcceptPairsPreflight preflight{};
    preflight.additionalCount = additionalCount;
    preflight.reason = pairBufferAcceptPairsRejectReason(buffer, additionalCount);
    preflight.exceedsCapacity = preflight.reason == PairBufferAcceptPairsRejectReason::ExceedsCapacity;

bool canSkipPairBufferAcceptPairs(const PairBufferSoA& buffer, u32 additionalCount) {
    return !preflightPairBufferAcceptPairs(buffer, additionalCount).canAccept();

bool shouldAcceptPairBufferPairs(const PairBufferSoA& buffer, u32 additionalCount) {
    return preflightPairBufferAcceptPairs(buffer, additionalCount).canAccept();






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

bool canSkipPairBufferPrepareSlots(u32 slotCount) {
    return !preflightPairBufferPrepareSlots(slotCount).canPrepare();

bool shouldRunPairBufferPrepareSlots(u32 slotCount) {
    return preflightPairBufferPrepareSlots(slotCount).canPrepare();
        return PairBufferAcceptPairsRejectReason::AtCapacity;


    preflight.atCapacity = preflight.reason == PairBufferAcceptPairsRejectReason::AtCapacity;


bool shouldRunPairBufferAcceptPairs(const PairBufferSoA& buffer, u32 additionalCount) {












    preflight.outOfSlot = preflight.reason == PairBufferInvalidateSlotRejectReason::OutOfSlot;





















const char* pairBufferWriteRejectReasonName(PairBufferWriteRejectReason reason) {
    case PairBufferWriteRejectReason::None:
    case PairBufferWriteRejectReason::InvalidPair:
    case PairBufferWriteRejectReason::OutOfRangeSlot:
    case PairBufferWriteRejectReason::UnpreparedBuffer:
        return "UnpreparedBuffer";

PairBufferWriteRejectReason pairBufferWriteRejectReason(
    if (buffer.pairSlotCount == 0u) {
        return PairBufferWriteRejectReason::UnpreparedBuffer;
        return PairBufferWriteRejectReason::OutOfRangeSlot;
        return PairBufferWriteRejectReason::InvalidPair;
    return PairBufferWriteRejectReason::None;

bool pairBufferWriteRejectsForReason(
    PairBufferWriteRejectReason expected) {
    return pairBufferWriteRejectReason(buffer, slot, idxA, idxB) == expected;

PairBufferWritePreflight preflightPairBufferWrite(
    PairBufferWritePreflight preflight{};
    preflight.reason = pairBufferWriteRejectReason(buffer, slot, idxA, idxB);
    preflight.unpreparedBuffer = preflight.reason == PairBufferWriteRejectReason::UnpreparedBuffer;
    preflight.outOfRangeSlot = preflight.reason == PairBufferWriteRejectReason::OutOfRangeSlot;
    preflight.invalidPair = preflight.reason == PairBufferWriteRejectReason::InvalidPair;

bool canSkipPairBufferWrite(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    return !preflightPairBufferWrite(buffer, slot, idxA, idxB).canWrite();

bool shouldRunPairBufferWrite(const PairBufferSoA& buffer, u32 slot, u32 idxA, u32 idxB) {
    return preflightPairBufferWrite(buffer, slot, idxA, idxB).canWrite();

const char* pairBufferInvalidateRejectReasonName(PairBufferInvalidateRejectReason reason) {
    case PairBufferInvalidateRejectReason::None:
    case PairBufferInvalidateRejectReason::OutOfRangeSlot:
    case PairBufferInvalidateRejectReason::AlreadyInvalid:

PairBufferInvalidateRejectReason pairBufferInvalidateRejectReason(const PairBufferSoA& buffer, u32 slot) {
        return PairBufferInvalidateRejectReason::OutOfRangeSlot;
        return PairBufferInvalidateRejectReason::AlreadyInvalid;
    return PairBufferInvalidateRejectReason::None;

bool pairBufferInvalidateRejectsForReason(
    PairBufferInvalidateRejectReason expected) {
    return pairBufferInvalidateRejectReason(buffer, slot) == expected;









    if (slot >= buffer.validFlags.size() || (buffer.pairSlotCount > 0u && slot >= buffer.pairSlotCount)) {
























PairBufferInvalidatePreflight preflightPairBufferInvalidate(const PairBufferSoA& buffer, u32 slot) {
    PairBufferInvalidatePreflight preflight{};
    preflight.reason = pairBufferInvalidateRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == PairBufferInvalidateRejectReason::OutOfRangeSlot;
    preflight.alreadyInvalid = preflight.reason == PairBufferInvalidateRejectReason::AlreadyInvalid;

bool canSkipPairBufferInvalidate(const PairBufferSoA& buffer, u32 slot) {
    return !preflightPairBufferInvalidate(buffer, slot).canInvalidate();

bool shouldRunPairBufferInvalidate(const PairBufferSoA& buffer, u32 slot) {
    return preflightPairBufferInvalidate(buffer, slot).canInvalidate();

































} // namespace fuse::physics::broadphase
