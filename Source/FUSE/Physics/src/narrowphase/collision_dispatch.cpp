#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>

namespace fuse::physics::narrowphase {

void runNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    if (pairs.empty()) {
        buffer.preparePairSlots(0u);
        return;
    }

    const u32 pairCount = static_cast<u32>(pairs.size());
    buffer.preparePairSlots(pairCount);

    if (can_skip_narrowphase(pairs, bodies, shapes)) {
        buffer.compactAndClamp();
        return;
    }

    // Per-pair slots are job-safe (disjoint writes). Serial dispatch on the CPU stub avoids
    // scheduler reference-capture flakes seen when stacking parallel broadphase + narrowphase
    // under core::initialize(); the slot layout matches the future parallel_for kernel path.
    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        if (!should_run_contact_pair_deepen_dispatch(pairs[pairIndex], bodies, shapes)) {
            continue;
        }
        ContactManifold manifold = detect_contacts_pair(pairs[pairIndex], bodies, shapes);
        if (finalize_contact_manifold_with_preflight(manifold)) {
        const broadphase::CandidatePair& pair = pairs[pairIndex];
        if (should_skip_contact_pair_dispatch(pair, bodies, shapes)) {

        ContactManifold manifold = detect_contacts_pair(pair, bodies, shapes);
        if (generate_contact_manifold(manifold)) {
        if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {


        if (generate_contact_manifold_if_needed(manifold)) {
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

NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const ContactBufferSoA& buffer) {
    NarrowphaseDispatchPreflight preflight{};
    if (pairs.empty()) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.batch = preflight_narrowphase_batch(pairs, bodies, shapes);
    preflight.compaction = preflightContactBufferCompaction(buffer);
    preflight.clamp = preflightContactBufferClamp(buffer);
    return preflight;
}

bool can_skip_narrowphase_dispatch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return can_skip_narrowphase(pairs, bodies, shapes);
}

} // namespace fuse::physics::narrowphase
