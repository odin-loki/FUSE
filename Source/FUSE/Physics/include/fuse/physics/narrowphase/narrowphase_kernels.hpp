#pragma once

// Narrowphase as single-source kernel launches (docs/compute-kernels.md):
//
//   physics_narrowphase        item / candidate pair: detect + finalize the manifold into the pair's
//                              staging slot, count = 1 when a contact was produced
//   physics_scan (+ _add)      exclusive scan of the counts -> contact slot per pair
//   physics_narrowphase_write  item / candidate pair: copy the staged manifold into its scanned slot
//
// The output buffer equals runNarrowphaseIntoBuffer's (same contacts in pair order, same
// activeCount / droppedCount; the max-capacity clamp runs on the host afterwards, as before).
//
// Backends: CpuReference / CpuParallel. The body calls the host narrowphase (collision_dispatch /
// contact_pair / gjk), which is not device-compiled yet, so it is not FUSE_HOST_DEVICE and there is
// no CUDA entry: a Cuda / Auto / VulkanCompute request falls back to CpuParallel (recorded in the
// kernel stats). Moving the shape-pair math into FUSE_HOST_DEVICE headers is what enables a .cu.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/physics/broadphase/broadphase_kernels.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::narrowphase {

inline constexpr const char* kNarrowphaseKernelName = "physics_narrowphase";
inline constexpr const char* kNarrowphaseWriteKernelName = "physics_narrowphase_write";

struct NarrowphaseKernelContext {
    kernel::Backend backend = kernel::Backend::CpuParallel;
    ContactBufferSoA staging;   ///< one slot per candidate pair
    std::vector<u32> offsets;   ///< per pair: contact count, then scanned contact slot
    broadphase::KernelScanScratch scan;
};

/// Kernel pipeline equivalent of runNarrowphaseIntoBuffer (identical contacts and counters).
void runNarrowphaseKernels(const std::vector<broadphase::CandidatePair>& pairs, const RigidBodySoA& bodies,
                           const CollisionShapeSoA& shapes, ContactBufferSoA& buffer,
                           NarrowphaseKernelContext& context);

namespace detail {

/// One candidate pair through dispatch + finalize (the per-pair body shared by the legacy loop and
/// the physics_narrowphase kernel). Returns false when the pair yields no contact.
bool narrowphasePairContact(const broadphase::CandidatePair& pair, const RigidBodySoA& bodies,
                            const CollisionShapeSoA& shapes, ContactManifold& out);

} // namespace detail

} // namespace fuse::physics::narrowphase
