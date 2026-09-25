// Narrowphase kernel launches (see narrowphase_kernels.hpp).

#include <fuse/physics/narrowphase/narrowphase_kernels.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/physics/broadphase/broadphase_kernel.hpp>

#include <algorithm>

namespace fuse::physics::narrowphase {
namespace {

struct NarrowphaseParams {
    const broadphase::CandidatePair* pairs = nullptr;
    const RigidBodySoA* bodies = nullptr;
    const CollisionShapeSoA* shapes = nullptr;
    ContactBufferSoA* staging = nullptr; ///< slot per pair (disjoint writes)
    u32* counts = nullptr;               ///< 1 when the pair produced a contact
};

/// One candidate pair: the same per-pair body the legacy loop runs, into the pair's own slot.
struct NarrowphaseKernel {
    void operator()(const kernel::LaunchIndex& idx, const NarrowphaseParams& p) const {
        const u32 i = idx.linear;
        ContactManifold manifold;
        if (detail::narrowphasePairContact(p.pairs[i], *p.bodies, *p.shapes, manifold)) {
            p.staging->writeSlot(i, manifold);
        }
        p.counts[i] = p.staging->validFlags[i] != 0u ? 1u : 0u;
    }
};

struct NarrowphaseWriteParams {
    const ContactBufferSoA* staging = nullptr;
    const u32* offsets = nullptr; ///< scanned counts: contact slot of each pair
    ContactBufferSoA* out = nullptr;
};

/// Copies a produced contact from its pair slot to its scanned contact slot (stable compaction).
struct NarrowphaseWriteKernel {
    void operator()(const kernel::LaunchIndex& idx, const NarrowphaseWriteParams& p) const {
        const ContactBufferSoA& src = *p.staging;
        const u32 s = idx.linear;
        if (src.validFlags[s] == 0u) {
            return;
        }
        ContactBufferSoA& dst = *p.out;
        const u32 d = p.offsets[s];
        dst.contactPoints[d] = src.contactPoints[s];
        dst.contactNormals[d] = src.contactNormals[s];
        dst.penetrationDepths[d] = src.penetrationDepths[s];
        dst.minSeparations[d] = src.minSeparations[s];
        dst.bodyA[d] = src.bodyA[s];
        dst.bodyB[d] = src.bodyB[s];
        dst.validFlags[d] = 1u;
        dst.pointCounts[d] = src.pointCounts[s];
        dst.warmNormalImpulses[d] = src.warmNormalImpulses[s];
        dst.warmTangentImpulses[d] = src.warmTangentImpulses[s];
        dst.tangent1[d] = src.tangent1[s];
        dst.tangent2[d] = src.tangent2[s];
        const usize sBase = static_cast<usize>(s) * kMaxContactPointsPerManifold;
        const usize dBase = static_cast<usize>(d) * kMaxContactPointsPerManifold;
        for (u32 k = 0; k < kMaxContactPointsPerManifold; ++k) {
            dst.pointSlots[dBase + k] = src.pointSlots[sBase + k];
            dst.pointPenetrations[dBase + k] = src.pointPenetrations[sBase + k];
        }
    }
};

} // namespace

void runNarrowphaseKernels(const std::vector<broadphase::CandidatePair>& pairs, const RigidBodySoA& bodies,
                           const CollisionShapeSoA& shapes, ContactBufferSoA& buffer,
                           NarrowphaseKernelContext& context) {
    const u32 pairCount = static_cast<u32>(pairs.size());
    buffer.preparePairSlots(pairCount);
    if (pairCount == 0u) {
        buffer.compactAndClamp(); // same empty-buffer bookkeeping as the legacy path
        return;
    }
    // Staging mirrors the output buffer's capacity (plus headroom) so a growing pair count does not
    // reallocate every few frames: preparePairSlots only allocates past the reserved capacity.
    if (context.staging.bodyA.capacity() < pairCount) {
        const usize grown = static_cast<usize>(pairCount) + pairCount / 2u;
        context.staging.reserve(static_cast<u32>(std::max<usize>(grown, buffer.bodyA.capacity())));
    }
    context.staging.preparePairSlots(pairCount);
    if (context.offsets.capacity() < pairCount) {
        context.offsets.reserve(static_cast<usize>(pairCount) + pairCount / 2u);
    }
    context.offsets.resize(pairCount);

    const kernel::KernelLaunch detect{kNarrowphaseKernelName, kernel::extent1(pairCount),
                                      broadphase_kernel::kItemWorkgroup};
    NarrowphaseParams params{};
    params.pairs = pairs.data();
    params.bodies = &bodies;
    params.shapes = &shapes;
    params.staging = &context.staging;
    params.counts = context.offsets.data();
    kernel::launch(context.backend, detect, NarrowphaseKernel{}, params);

    const u32 contacts =
        broadphase::exclusiveScanKernel(context.backend, context.offsets.data(), pairCount, context.scan);

    const kernel::KernelLaunch write{kNarrowphaseWriteKernelName, kernel::extent1(pairCount),
                                     broadphase_kernel::kItemWorkgroup};
    kernel::launch(context.backend, write, NarrowphaseWriteKernel{},
                   NarrowphaseWriteParams{&context.staging, context.offsets.data(), &buffer});
    buffer.activeCount = contacts;
    buffer.applyMaxCapacityClamp();
}

} // namespace fuse::physics::narrowphase
