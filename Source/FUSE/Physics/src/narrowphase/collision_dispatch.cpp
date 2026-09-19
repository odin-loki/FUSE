#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>

namespace fuse::physics::narrowphase {

NarrowphaseIntoBufferPreflight preflight_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphaseIntoBufferPreflight preflight{};
    preflight.batch = preflight_narrowphase_batch(pairs, bodies, shapes);
    preflight.skipped = preflight.batch.can_skip();
    return preflight;
}

bool can_skip_narrowphase_into_buffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflight_narrowphase_into_buffer(pairs, bodies, shapes).skipped;
}

void run_narrowphase_into_buffer_with_preflight(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    if (can_skip_narrowphase_into_buffer(pairs, bodies, shapes)) {
        buffer.clear();
        return;
    }
    runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);
}

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

} // namespace fuse::physics::narrowphase
