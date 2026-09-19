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
        if (!should_run_contact_pair_deepen_dispatch(pairs[pairIndex], bodies, shapes)) {
            continue;
        }
        ContactManifold manifold = detect_contacts_pair(pairs[pairIndex], bodies, shapes);
        if (finalize_contact_manifold_with_preflight(manifold)) {
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

// --- deepen additive from b4-narrowphase-guards-deepen-2074 ---
        if (should_skip_contact_pair_dispatch(pair, bodies, shapes)) {

// --- deepen additive from deepen-b4-narrowphase-guards-7360 ---
        if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {

// --- deepen additive from b4-narrowphase-deepen-guards-046d ---
NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(
    NarrowphaseDispatchPreflight preflight{};
    preflight.compaction = preflightContactBufferCompaction(buffer);
    preflight.clamp = preflightContactBufferClamp(buffer);

// --- deepen additive from deepen-b4-narrowphase-guards-b130 ---
NarrowphasePairSlotPreflight preflight_narrowphase_pair_slot(
    NarrowphasePairSlotPreflight preflight{};
bool should_skip_narrowphase_pair_slot(
        if (should_skip_narrowphase_pair_slot(pair, bodies, shapes)) {

// --- deepen additive from deepen-b4-narrowphase-guards-37c2 ---
    preflight.pairPreflight = preflight_contact_pair(pair, bodies, shapes);
    preflight.skipped = !preflight.pairPreflight.can_dispatch();
    return !should_skip_narrowphase_pair_slot(pair, bodies, shapes);
        if (should_skip_narrowphase_pair_slot(pairs[pairIndex], bodies, shapes)) {

// --- deepen additive from deepen-b4-narrowphase-guards-e6d2 ---
    return should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);

// --- deepen additive from narrowphase-guard-pass-4c08 ---
        if (should_skip_contact_pair_dispatch(pairs[pairIndex], bodies, shapes)) {

// --- deepen additive from deepen-b4-narrowphase-guards-754b ---
        if (should_skip_contact_pair_deepen_dispatch(pairs[pairIndex], bodies, shapes)) {

// --- deepen additive from deepen-b4-narrowphase-guards-7d67 ---
void runNarrowphaseIntoBufferWithDeepenPreflight(
    runNarrowphaseIntoBufferWithDeepenPreflight(pairs, bodies, shapes, buffer);
