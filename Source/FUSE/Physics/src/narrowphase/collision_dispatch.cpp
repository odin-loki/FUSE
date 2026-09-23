#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>
#include <fuse/physics/narrowphase/narrowphase_kernels.hpp>

namespace fuse::physics::narrowphase {

bool detail::narrowphasePairContact(const broadphase::CandidatePair& pair, const RigidBodySoA& bodies,
                                    const CollisionShapeSoA& shapes, ContactManifold& out) {
    if (!should_run_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
        return false;
    }
    out = detect_contacts_pair(pair, bodies, shapes);
    return finalize_contact_manifold_with_preflight(out);
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
    // runNarrowphaseKernels runs the same per-pair body as the physics_narrowphase kernel.
    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        ContactManifold manifold;
        if (detail::narrowphasePairContact(pairs[pairIndex], bodies, shapes, manifold)) {
            buffer.writeSlot(pairIndex, manifold);
        }
    }

    buffer.compactAndClamp();
}

void collidePairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    std::vector<ContactManifold>& out,
    f32 margin) {
    out.clear();
    // Only validity checks here: trigger and sleeping pairs are still reported (the solver skips
    // resolving them) so overlap events and resting contacts stay continuous.
    for (const broadphase::CandidatePair& pair : pairs) {
        ContactManifold manifold = detect_contacts_pair(pair, bodies, shapes, margin);
        if (margin > 0.f) {
            // Finalize prunes separated points: shift speculative points into range and back.
            for (u32 i = 0; i < manifold.pointCount; ++i) {
                manifold.points[i].penetration += margin;
            }
        }
        if (!finalize_contact_manifold_with_preflight(manifold)) {
            continue;
        }
        if (margin > 0.f) {
            for (u32 i = 0; i < manifold.pointCount; ++i) {
                manifold.points[i].penetration -= margin;
            }
            manifold.syncLegacyFields();
        }
        out.push_back(manifold);
    }
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
