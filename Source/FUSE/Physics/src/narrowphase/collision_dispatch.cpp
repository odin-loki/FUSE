#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>

namespace fuse::physics::narrowphase {

void runNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    const u32 pairCount = static_cast<u32>(pairs.size());
    buffer.preparePairSlots(pairCount);

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

ContactManifold detect_contacts_pair_deepen(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
        return invalidContactManifold();
    }
    return detect_contacts_pair(pair, bodies, shapes);
}

bool finalize_contact_manifold_if_needed(ContactManifold& manifold) {
    return generate_contact_manifold_if_needed(manifold);
}

void runNarrowphaseDeepenIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    const u32 pairCount = static_cast<u32>(pairs.size());
    buffer.preparePairSlots(pairCount);

    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        ContactManifold manifold = detect_contacts_pair_deepen(pairs[pairIndex], bodies, shapes);
        if (finalize_contact_manifold_if_needed(manifold)) {
            buffer.writeSlot(pairIndex, manifold);
        }
    }

    buffer.compactAndClamp();
}

} // namespace fuse::physics::narrowphase
