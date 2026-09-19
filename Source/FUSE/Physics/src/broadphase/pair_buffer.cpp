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
        return;
    }

    const CandidateRejectReason rejectReason = candidatePairRejectReason(idxA, idxB);
    if (rejectReason != CandidateRejectReason::None) {
        lastRejectReason = rejectReason;
void PairBufferSoA::writeSlot(u32 slot, u32 idxA, u32 idxB, u32 bodyCount) {
    if (slot >= pairSlotCount || !isValidCandidatePair(idxA, idxB, bodyCount)) {
    const CandidatePairRejectReason rejectReason = candidatePairRejectReason(idxA, idxB, bodyCount);
    if (rejectReason != CandidatePairRejectReason::None) {
    const CandidatePairRejectReason rejectReason = candidatePairRejectReason(idxA, idxB);
        return;
    }

    const CandidatePair pair = canonicalPair(idxA, idxB);
    bodyA[slot] = pair.bodyA;
    bodyB[slot] = pair.bodyB;
    validFlags[slot] = 1u;
    lastRejectReason = CandidatePairRejectReason::None;
}

void PairBufferSoA::invalidateSlot(u32 slot) {
    if (!preflightPairBufferInvalidateSlot(*this, slot).canInvalidate()) {
        return;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
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
        return true;
    }

    const u32 validCount = countValidSlots();
    return validCount <= 1u;
}

bool PairBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;
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

bool PairBufferSoA::push(u32 idxA, u32 idxB) {
    if (!isValidCandidatePair(idxA, idxB)) {
        return false;
    }

    return countValidSlots() <= 1u;

bool PairBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;

bool PairBufferSoA::push(u32 idxA, u32 idxB) {
    if (!isValidCandidatePair(idxA, idxB)) {
bool PairBufferSoA::wouldRejectPush(u32 idxA, u32 idxB, u32 bodyCount) const {
    if (!isValidCandidatePair(idxA, idxB, bodyCount)) {
        return true;
    const CandidatePairRejectReason rejectReason = candidatePairRejectReason(idxA, idxB);
    if (rejectReason != CandidatePairRejectReason::None) {
        lastRejectReason = rejectReason;
        return false;
    }
    return isFull();

bool PairBufferSoA::push(u32 idxA, u32 idxB, u32 bodyCount) {
    const CandidatePairRejectReason rejectReason = candidatePairRejectReason(idxA, idxB, bodyCount);
    if (rejectReason != CandidatePairRejectReason::None) {
        lastRejectReason = rejectReason;
        return false;

    const PairBufferPushPreflight preflight = preflightPairBufferPush(*this, idxA, idxB);
    if (!preflight.canPush()) {
        if (preflight.atCapacity) {
            ++droppedCount;
    if (maxCapacity > 0u && activeCount >= maxCapacity) {
        lastRejectReason = CandidateRejectReason::BufferFull;
    if (isFull()) {
bool PairBufferSoA::wouldRejectPush(u32 idxA, u32 idxB, u32 bodyCount) const {
    if (!isValidCandidatePair(idxA, idxB, bodyCount)) {
    return isFull();

bool PairBufferSoA::push(u32 idxA, u32 idxB, u32 bodyCount) {
    if (wouldRejectPush(idxA, idxB, bodyCount)) {
        if (isValidCandidatePair(idxA, idxB, bodyCount) && isFull()) {
        ++droppedCount;
        lastRejectReason = CandidatePairRejectReason::BufferFull;
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
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }

    if (compactionPreflight.reason == PairBufferCompactionRejectReason::AllValid) {
        activeCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 validCount = 0u;
    for (u32 i = 0u; i < scanCount; ++i) {
        if (validFlags[i] != 0u) {
            ++validCount;
        }

    if (validCount == 0u) {
        activeCount = 0u;
        pairSlotCount = 0u;
        bodyA.resize(0);
        bodyB.resize(0);
        validFlags.resize(0);
        return activeCount;

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
    if (!shouldRunPairBufferSort(*this)) {
    if (canSkipSort()) {
    if (canSkipSoAIteration() || activeCount <= 1u || isSortedCanonical()) {
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

    if (!isSortedCanonical()) {
        sortCanonical();
    }

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
        return true;
    }

    return countValidSlots() <= 1u;
}

bool PairBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;
}

bool PairBufferSoA::canSkipRefineIteration() const {
    return countValidSlots() == 0u;
}

u32 PairBufferSoA::compactAndClamp() {
    const PairBufferCompactAndClampPreflight preflight = preflightPairBufferCompactAndClamp(*this);
    if (preflight.reason == PairBufferCompactAndClampRejectReason::EmptyBuffer) {
    if (canSkipCompactAndClamp()) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }
    if (preflight.reason == PairBufferCompactAndClampRejectReason::NoWork) {
    if (canSkipSoAIteration()) {
        return 0u;
    }

    compact();
    if (isEmpty()) {
        return 0u;
    }

    if (!isSortedCanonical()) {
        sortCanonical();
    }
    return applyMaxCapacityClamp();
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

PairBufferPushPreflight preflightPairBufferPush(const PairBufferSoA& buffer, u32 idxA, u32 idxB) {
    PairBufferPushPreflight preflight{};
    preflight.reason = pairBufferPushRejectReason(buffer, idxA, idxB);
    preflight.invalidPair = preflight.reason == PairBufferPushRejectReason::InvalidPair;
    preflight.atCapacity = preflight.reason == PairBufferPushRejectReason::AtCapacity;
    return preflight;

PairBufferCompactionPreflight preflightPairBufferCompaction(const PairBufferSoA& buffer) {
    PairBufferCompactionPreflight preflight{};
    preflight.reason = pairBufferCompactionRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == PairBufferCompactionRejectReason::AllValid;

bool canSkipPairBufferCompaction(const PairBufferSoA& buffer) {
    return !preflightPairBufferCompaction(buffer).needsCompaction();

bool shouldRunPairBufferCompaction(const PairBufferSoA& buffer) {
    return preflightPairBufferCompaction(buffer).needsCompaction();

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

PairBufferSortPreflight preflightPairBufferSort(const PairBufferSoA& buffer) {
    PairBufferSortPreflight preflight{};
    preflight.reason = pairBufferSortRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == PairBufferSortRejectReason::EmptyBuffer;
    preflight.singlePair = preflight.reason == PairBufferSortRejectReason::SinglePair;

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


    if (slot >= buffer.validFlags.size()) {




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
}

} // namespace fuse::physics::broadphase
