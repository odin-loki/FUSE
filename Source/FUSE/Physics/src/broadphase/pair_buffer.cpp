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
    bodyA.clear();
    bodyB.clear();
    validFlags.clear();
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
    if (slot >= pairSlotCount || idxA == idxB) {
        return;
    }

    const CandidatePair pair = canonicalPair(idxA, idxB);
    bodyA[slot] = pair.bodyA;
    bodyB[slot] = pair.bodyB;
    validFlags[slot] = 1u;
}

bool PairBufferSoA::push(u32 idxA, u32 idxB) {
    if (idxA == idxB) {
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
    return true;
}

u32 PairBufferSoA::compact() {
    u32 writeIndex = 0;
    for (u32 readIndex = 0; readIndex < pairSlotCount; ++readIndex) {
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
    for (u32 i = activeCount; i < pairSlotCount; ++i) {
        validFlags[i] = 0u;
    }
    return activeCount;
}

CandidatePair PairBufferSoA::pairAt(u32 index) const {
    if (index >= activeCount || validFlags[index] == 0u) {
        return {};
    }
    return {bodyA[index], bodyB[index]};
}

std::vector<CandidatePair> PairBufferSoA::toVector() const {
    std::vector<CandidatePair> pairs;
    pairs.reserve(activeCount);
    for (u32 i = 0; i < activeCount; ++i) {
        if (validFlags[i] != 0u) {
            pairs.push_back(pairAt(i));
        }
    }
    return pairs;
}

} // namespace fuse::physics::broadphase
