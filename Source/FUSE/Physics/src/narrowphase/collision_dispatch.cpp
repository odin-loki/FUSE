#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>

namespace fuse::physics::narrowphase {

NarrowphaseIntoBufferPreflight preflight_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphaseIntoBufferPreflight preflight{};
    preflight.pairCount = static_cast<u32>(pairs.size());
    preflight.emptyPairs = pairs.empty();
    preflight.batchPreflight = preflight_narrowphase_batch(pairs, bodies, shapes);
    return preflight;
}

bool can_skip_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflight_narrowphase_into_buffer(pairs, bodies, shapes).can_skip();
}

bool should_run_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !can_skip_narrowphase_into_buffer(pairs, bodies, shapes);
}

void runNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    const NarrowphaseIntoBufferPreflight preflight =
        preflight_narrowphase_into_buffer(pairs, bodies, shapes);
    const u32 pairCount = preflight.pairCount;
    buffer.preparePairSlots(pairCount);

    if (preflight.can_skip()) {
        if (!can_skip_contact_buffer_compact_and_clamp(buffer)) {
            buffer.compactAndClamp();
        }
        return;
    }

    // Per-pair slots are job-safe (disjoint writes). Serial dispatch on the CPU stub avoids
    // scheduler reference-capture flakes seen when stacking parallel broadphase + narrowphase
    // under core::initialize(); the slot layout matches the future parallel_for kernel path.
    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
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
