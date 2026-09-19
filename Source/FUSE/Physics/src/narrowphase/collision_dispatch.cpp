#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>

namespace fuse::physics::narrowphase {

NarrowphaseIntoBufferPreflight preflightNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphaseIntoBufferPreflight preflight{};
    preflight.pairCount = static_cast<u32>(pairs.size());
    preflight.emptyPairs = pairs.empty();
    if (!preflight.emptyPairs) {
        const NarrowphaseBatchPreflight batchPreflight = preflight_narrowphase_batch(pairs, bodies, shapes);
        preflight.dispatchableCount = batchPreflight.dispatchableCount;
    }
    return preflight;
}

bool should_skip_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !preflightNarrowphaseIntoBuffer(pairs, bodies, shapes).can_run();
}

void runNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    const NarrowphaseIntoBufferPreflight preflight = preflightNarrowphaseIntoBuffer(pairs, bodies, shapes);
    if (!preflight.can_run()) {
        buffer.preparePairSlots(0u);
        return;
    }

    const u32 pairCount = preflight.pairCount;
    buffer.preparePairSlots(pairCount);

    // Per-pair slots are job-safe (disjoint writes). Serial dispatch on the CPU stub avoids
    // scheduler reference-capture flakes seen when stacking parallel broadphase + narrowphase
    // under core::initialize(); the slot layout matches the future parallel_for kernel path.
    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        if (should_skip_narrowphase_pair_slot(pairIndex, pairs[pairIndex], bodies, shapes)) {
            continue;
        }
        ContactManifold manifold = detect_contacts_pair(pairs[pairIndex], bodies, shapes);
        if (generate_contact_manifold(manifold)) {
            buffer.writeSlot(pairIndex, manifold);
        }
    }

    buffer.compactAndClamp();
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
