#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>

namespace fuse::physics::narrowphase {

NarrowphasePairSlotPreflight preflight_narrowphase_pair_slot(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphasePairSlotPreflight preflight{};
    if (should_skip_contact_pair_dispatch(pair, bodies, shapes)) {
        preflight.skipped = true;
        preflight.pairRejected = true;
        return preflight;
    }

    preflight.canDispatch = true;
    preflight.pairPreflight = preflight_contact_pair(pair, bodies, shapes);
    preflight.skipped = !preflight.pairPreflight.can_dispatch();
    return preflight;
}

bool should_skip_narrowphase_pair_slot(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !preflight_narrowphase_pair_slot(pair, bodies, shapes).can_process();
    return !preflight_narrowphase_pair_slot(pair, bodies, shapes).can_dispatch();
}

bool should_run_narrowphase_pair_slot(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_narrowphase_pair_slot(pair, bodies, shapes);
}

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
        if (should_skip_narrowphase_pair_slot(pairs[pairIndex], bodies, shapes)) {
        if (should_skip_contact_pair_dispatch(pairs[pairIndex], bodies, shapes)) {

        ContactManifold manifold = detect_contacts_pair(pairs[pairIndex], bodies, shapes);
        if (finalize_contact_manifold_with_preflight(manifold)) {
        const broadphase::CandidatePair& pair = pairs[pairIndex];
        if (should_skip_contact_pair_dispatch(pair, bodies, shapes)) {
        if (should_skip_narrowphase_pair_slot(pair, bodies, shapes)) {

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

bool can_skip_narrowphase_dispatch(
    const CollisionShapeSoA& shapes) {
    return can_skip_narrowphase(pairs, bodies, shapes);
ContactManifold detect_contacts_pair_deepen(
    const broadphase::CandidatePair& pair,
    if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
        return invalidContactManifold();
    return detect_contacts_pair(pair, bodies, shapes);

bool finalize_contact_manifold_if_needed(ContactManifold& manifold) {
    return generate_contact_manifold_if_needed(manifold);

void runNarrowphaseDeepenIntoBuffer(
void runNarrowphaseIntoBufferDeepen(
    ContactBufferSoA& buffer) {
    const u32 pairCount = static_cast<u32>(pairs.size());
    buffer.preparePairSlots(pairCount);

    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        ContactManifold manifold = detect_contacts_pair_deepen(pairs[pairIndex], bodies, shapes);
        if (finalize_contact_manifold_if_needed(manifold)) {
            buffer.writeSlot(pairIndex, manifold);

    buffer.compactAndClamp();
    preflight.pairCount = static_cast<u32>(pairs.size());
    preflight.emptyPairs = pairs.empty();
    if (preflight.emptyPairs) {
        preflight.allPairsDeepenRejected = true;

    preflight.dispatchablePairCount = count_dispatchable_contact_pairs(pairs, bodies, shapes);
    preflight.allPairsDeepenRejected = preflight.dispatchablePairCount == 0u;

bool should_skip_narrowphase_pair_slot(
    const RigidBodySoA& bodies,
    return should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
    if (can_skip_narrowphase(pairs, bodies, shapes)) {
        return;
    }

        if (generate_contact_manifold(manifold)) {

}

} // namespace fuse::physics::narrowphase
