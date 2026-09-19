#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
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

bool should_skip_narrowphase_pair_slot(
    return !preflight_narrowphase_pair_slot(pair, bodies, shapes).can_process();
    return !preflight_narrowphase_pair_slot(pair, bodies, shapes).can_dispatch();

bool should_run_narrowphase_pair_slot(
    return !should_skip_narrowphase_pair_slot(pair, bodies, shapes);
NarrowphaseRunPreflight preflightNarrowphaseRun(
    const std::vector<broadphase::CandidatePair>& pairs,
    return preflight_run_narrowphase(pairs, bodies, shapes);

bool canSkipNarrowphaseRun(
    return can_skip_narrowphase_run(pairs, bodies, shapes);
namespace {

u32 countBaseDispatchableContactPairs(
    u32 dispatchable = 0u;
    for (const broadphase::CandidatePair& pair : pairs) {
        if (!should_skip_narrowphase_pair_slot(pair, bodies, shapes)) {
            ++dispatchable;
    return dispatchable;

} // namespace

NarrowphaseIntoBufferPreflight preflight_narrowphase_into_buffer(
    const CollisionShapeSoA& shapes,
    const ContactBufferSoA& buffer) {
    NarrowphaseIntoBufferPreflight preflight{};
    preflight.pairCount = static_cast<u32>(pairs.size());
    preflight.dispatchableCount = countBaseDispatchableContactPairs(pairs, bodies, shapes);
    preflight.canSkipPass = preflight.pairCount == 0u;
    preflight.canSkipBufferCompaction = buffer.canSkipCompaction();
    preflight.canSkipBufferClamp = buffer.canSkipMaxCapacityClamp();

bool can_skip_narrowphase_into_buffer(
    if (pairs.empty()) {
        return true;
    return countBaseDispatchableContactPairs(pairs, bodies, shapes) == 0u;
NarrowphaseIntoBufferPreflight preflightNarrowphaseIntoBuffer(
    preflight.emptyPairs = pairs.empty();
    if (!preflight.emptyPairs) {
        const NarrowphaseBatchPreflight batchPreflight = preflight_narrowphase_batch(pairs, bodies, shapes);
        preflight.dispatchableCount = batchPreflight.dispatchableCount;

bool should_skip_narrowphase_into_buffer(
    return !preflightNarrowphaseIntoBuffer(pairs, bodies, shapes).can_run();
NarrowphaseBufferFinalizePreflight preflight_narrowphase_buffer_finalize(const ContactBufferSoA& buffer) {
    NarrowphaseBufferFinalizePreflight preflight{};
    preflight.pairSlotCount = buffer.pairSlotCount;
    preflight.validSlotCount = buffer.countValidSlots();
    if (preflight.pairSlotCount == 0u) {

    preflight.needsCompaction = should_run_contact_buffer_compaction(buffer);
    preflight.needsClamp = should_run_contact_buffer_clamp(buffer);

bool can_skip_narrowphase_buffer_finalize(const ContactBufferSoA& buffer) {
    const NarrowphaseBufferFinalizePreflight preflight = preflight_narrowphase_buffer_finalize(buffer);
    return preflight.skipped || (!preflight.needsCompaction && !preflight.needsClamp);
    }
    return preflight;

    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
}

void runNarrowphaseIntoBuffer(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    if (pairs.empty()) {
    const NarrowphaseRunPreflight runPreflight = preflight_run_narrowphase(pairs, bodies, shapes);
    if (runPreflight.canSkip) {
    const NarrowphaseIntoBufferPreflight preflight = preflightNarrowphaseIntoBuffer(pairs, bodies, shapes);
    if (!preflight.can_run()) {
        buffer.preparePairSlots(0u);
        return;
    }

    const u32 pairCount = static_cast<u32>(pairs.size());
    const u32 pairCount = preflight.pairCount;
    buffer.preparePairSlots(pairCount);
    if (pairCount == 0u) {
        return;
    }

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
        if (should_skip_narrowphase_pair_slot(pairs[pairIndex], bodies, shapes)) {
        if (should_skip_contact_pair_dispatch(pairs[pairIndex], bodies, shapes)) {
        if (should_skip_narrowphase_pair_slot(pairIndex, pairs[pairIndex], bodies, shapes)) {
        }
        ContactManifold manifold = detect_contacts_pair(pairs[pairIndex], bodies, shapes);
        if (generate_contact_manifold(manifold)) {
            buffer.writeSlot(pairIndex, manifold);

        if (should_skip_narrowphase_pair_slot(pairIndex, pairs[pairIndex], bodies, shapes)) {
        }
        ContactManifold manifold = detect_contacts_pair(pairs[pairIndex], bodies, shapes);
        if (finalize_contact_manifold_with_preflight(manifold)) {
        const broadphase::CandidatePair& pair = pairs[pairIndex];
        if (should_skip_contact_pair_dispatch(pair, bodies, shapes)) {
        if (should_skip_narrowphase_pair_slot(pair, bodies, shapes)) {

        ContactManifold manifold = detect_contacts_pair(pair, bodies, shapes);
        if (generate_contact_manifold(manifold)) {
        if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {


        if (generate_contact_manifold_if_needed(manifold)) {

        if (generate_contact_manifold_with_preflight(manifold)) {
            buffer.writeSlot(pairIndex, manifold);


void runNarrowphaseIntoBufferWithDeepenPreflight(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactBufferSoA& buffer) {
    const u32 pairCount = runPreflight.pairCount;
    buffer.preparePairSlots(pairCount);

    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        ContactManifold manifold = detect_contacts_pair_deepen(pairs[pairIndex], bodies, shapes);
            write_contact_buffer_slot_with_preflight(buffer, pairIndex, manifold);

    compact_and_clamp_contact_buffer_with_preflight(buffer);

bool runNarrowphaseIntoBufferIfNeeded(
    if (can_skip_narrowphase(pairs, bodies, shapes)) {
        buffer.clear();
        return false;
    runNarrowphaseIntoBufferWithDeepenPreflight(pairs, bodies, shapes, buffer);
    return buffer.hasValidContacts();

void runNarrowphaseIntoBufferWithPreflight(
    if (narrowphase_batch_rejects_all(pairs, bodies, shapes)) {
        return;

    const u32 pairCount = static_cast<u32>(pairs.size());

        ContactManifold manifold =
            detect_contacts_pair_with_deepen_preflight(pairs[pairIndex], bodies, shapes);
            buffer.writeSlotWithPreflight(pairIndex, manifold);


    buffer.compactAndClampWithPreflight();

void runNarrowphaseIntoBufferIfDispatchable(
    runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);
            writeContactBufferSlotWithPreflight(buffer, pairIndex, manifold);

    compactAndClampContactBufferWithPreflight(buffer);

bool should_skip_narrowphase_buffer_pass(const ContactBufferSoA& buffer) {
    return canSkipContactBufferCompactAndClamp(buffer);
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
void runNarrowphaseIntoBufferWithDeepenPreflight(
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
    return should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
    if (can_skip_narrowphase(pairs, bodies, shapes)) {
        return;

        if (generate_contact_manifold(manifold)) {

        if (should_skip_contact_pair_deepen_dispatch(pairs[pairIndex], bodies, shapes)) {
            continue;

        ContactManifold manifold = detect_contacts_pair(pairs[pairIndex], bodies, shapes);


std::vector<ContactManifold> runNarrowphaseDeepen(
        if (finalize_contact_manifold_with_preflight(manifold)) {
            buffer.writeSlotIfValid(pairIndex, manifold);


    ContactBufferSoA buffer;
    buffer.reserve(static_cast<u32>(pairs.size()));
    runNarrowphaseIntoBufferDeepen(pairs, bodies, shapes, buffer);
        ContactManifold manifold = detect_contacts_pair_with_preflight(pairs[pairIndex], bodies, shapes);


std::vector<ContactManifold> runNarrowphaseWithDeepenPreflight(
    for (u32 pairIndex = 0u; pairIndex < pairCount; ++pairIndex) {

            write_contact_manifold_to_buffer_with_preflight(buffer, pairIndex, manifold);


    runNarrowphaseIntoBufferWithDeepenPreflight(pairs, bodies, shapes, buffer);
    return buffer.toVector();
NarrowphaseDispatchPreflight preflight_run_narrowphase_into_buffer(
    preflight.skipped = preflight.batch.can_skip();

bool should_skip_narrowphase_dispatch(
    return preflight_run_narrowphase_into_buffer(pairs, bodies, shapes).can_skip();
    if (narrowphase_batch_rejects_all(pairs, bodies, shapes)) {
        compact_and_clamp_contact_buffer_with_preflight(buffer);
    }

        ContactManifold manifold = detect_contacts_pair_with_deepen_preflight(pairs[pairIndex], bodies, shapes);
        if (generate_contact_manifold_with_deepen_preflight(manifold)) {
            write_contact_buffer_slot_with_preflight(buffer, pairIndex, manifold);

}

} // namespace fuse::physics::narrowphase
