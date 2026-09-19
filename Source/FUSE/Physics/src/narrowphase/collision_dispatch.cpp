#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>

namespace fuse::physics::narrowphase {

namespace {

void runNarrowphaseIntoBufferImpl(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer,
    bool useDeepenReject) {
    const u32 pairCount = static_cast<u32>(pairs.size());
    buffer.preparePairSlots(pairCount);

    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        ContactManifold manifold = useDeepenReject
            ? detect_contacts_pair_deepen(pairs[pairIndex], bodies, shapes)
            : detect_contacts_pair(pairs[pairIndex], bodies, shapes);
        const bool finalized = useDeepenReject
            ? generate_contact_manifold_deepen(manifold)
            : generate_contact_manifold(manifold);
        if (finalized) {
            buffer.writeSlot(pairIndex, manifold);
        }
    }

    buffer.compactAndClamp();
}

} // namespace

bool can_skip_narrowphase_buffer_dispatch(const std::vector<broadphase::CandidatePair>& pairs) {
    return pairs.empty();
}

bool can_skip_narrowphase_buffer_dispatch_deepen(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflight_narrowphase_buffer_dispatch(pairs, bodies, shapes, true).skipped;
}

NarrowphaseBufferDispatchPreflight preflight_narrowphase_buffer_dispatch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    bool useDeepenReject) {
    NarrowphaseBufferDispatchPreflight preflight{};
    preflight.pairCount = static_cast<u32>(pairs.size());
    if (preflight.pairCount == 0u) {
        preflight.skipped = true;
        preflight.emptyPairs = true;
        return preflight;
    }

    if (useDeepenReject) {
        preflight.dispatchableCount = count_dispatchable_contact_pairs(pairs, bodies, shapes);
        if (preflight.dispatchableCount == 0u) {
            preflight.skipped = true;
            preflight.allRejected = true;
        }
    } else {
        preflight.dispatchableCount = preflight.pairCount;
    }

    return preflight;
}

void runNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    if (can_skip_narrowphase_buffer_dispatch(pairs)) {
        buffer.preparePairSlots(0u);
        return;
    }

    runNarrowphaseIntoBufferImpl(pairs, bodies, shapes, buffer, false);
}

void runNarrowphaseIntoBufferDeepen(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    const NarrowphaseBufferDispatchPreflight preflight =
        preflight_narrowphase_buffer_dispatch(pairs, bodies, shapes, true);
    if (preflight.skipped) {
        buffer.preparePairSlots(preflight.pairCount);
        return;
    }

    runNarrowphaseIntoBufferImpl(pairs, bodies, shapes, buffer, true);
}

std::vector<ContactManifold> runNarrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    ContactBufferSoA buffer;
    buffer.reserve(static_cast<u32>(pairs.size()));
    runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);
    return buffer.toVector();
}

} // namespace fuse::physics::narrowphase
