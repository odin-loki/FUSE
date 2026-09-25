#pragma once

// Host orchestration of the single-source broadphase kernels (broadphase_kernel.hpp):
//
//   exclusiveScanKernel     multi-level reduce-then-scan of a u32 array (count -> scan -> write)
//   radixSortKernel         stable LSD radix sort of u32 keys (+ values), 4-bit digit passes
//   KernelRadixSorter       radixSortKernel as a BroadphaseKeyValueSorter (SpatialHashParams::entrySorter)
//   runBroadphaseKernels    the whole spatial-hash broadphase as kernel launches; the pair buffer is
//                           bit-identical to runBroadphaseIntoBuffer (pairs, order, counts)
//
// Scratch structs grow to the working size once and are reused: steady-state frames make no heap
// allocations on the CPU backends. `backend` is any kernel::Backend: CPU backends run the bodies
// directly (host memory); GPU requests fall back to CpuParallel (recorded in the kernel stats)
// unless the CUDA staging wrapper (kernels/physics_broadphase.cu) is used.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::broadphase {

struct KernelScanScratch {
    std::vector<u32> levels; ///< chunk-total levels of the multi-level scan
};

/// Exclusive scan of data[0..count) in place through kernel launches; returns the total.
u32 exclusiveScanKernel(kernel::Backend backend, u32* data, u32 count, KernelScanScratch& scratch);

struct KernelRadixSortScratch {
    std::vector<u32> keys;   ///< ping-pong keys
    std::vector<u32> values; ///< ping-pong values
    std::vector<u32> hist;   ///< digit-major tile histograms
    KernelScanScratch scan;
};

/// Stable ascending sort of keys[0..count) (every key < 2^keyBits), carrying values[i] with keys[i]
/// (`values` may be null). ceil(keyBits / 4) passes of histogram -> scan -> scatter launches.
void radixSortKernel(kernel::Backend backend, u32* keys, u32* values, u32 count, u32 keyBits,
                     KernelRadixSortScratch& scratch);

/// The kernel radix sort behind the BroadphaseKeyValueSorter hook (legacy broadphase path).
struct KernelRadixSorter {
    kernel::Backend backend = kernel::Backend::CpuParallel;
    u32 minCount = 0u;
    KernelRadixSortScratch scratch;

    /// Hook for SpatialHashParams::entrySorter; `this` must outlive its use.
    BroadphaseKeyValueSorter sorter();
};

struct BroadphaseKernelScratch {
    std::vector<u32> shapeOffsets; ///< per shape: cell count, then exclusive offsets
    std::vector<u32> entryKeys;
    std::vector<u32> entryBodies;
    std::vector<u32> runIndex;     ///< boundary flags, then scanned run index
    std::vector<u32> runStarts;    ///< runCount + 1
    std::vector<u32> uniqueCounts; ///< per run
    std::vector<u32> pairOffsets;  ///< per run: pair count, then exclusive offsets
    std::vector<u32> pairKeys;
    std::vector<u32> pairIndex;    ///< boundary flags of the sorted pair keys, then scanned
    std::vector<u32> pairStarts;
    KernelRadixSortScratch sort;
    KernelScanScratch scan;
    BroadphaseScratch planes; ///< plane merge (shared host code with the legacy path)
};

/// Per-launch statistics of the last runBroadphaseKernels call.
struct BroadphaseKernelStats {
    u32 entries = 0;     ///< (cell, body) occupancy entries
    u32 cells = 0;       ///< occupied cells (runs)
    u32 cellPairs = 0;   ///< pairs emitted before dedupe
    u32 uniquePairs = 0; ///< after dedupe (before the plane merge / clamp)
    bool usedKernels = false; ///< false: fell back to the legacy path (> 65536 bodies)
};

struct BroadphaseKernelContext {
    kernel::Backend backend = kernel::Backend::CpuParallel;
    BroadphaseKernelScratch scratch;
    BroadphaseKernelStats stats;
};

/// Kernel pipeline equivalent of runBroadphaseIntoBuffer / runBroadphase2DIntoBuffer: identical
/// pair buffer. `params.entrySorter` is ignored (the kernel radix sort always runs). Scenes with
/// more than 65536 bodies (pair keys no longer fit 32 bits) run the legacy path instead.
void runBroadphaseKernels(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes, const SpatialHashParams& params,
                          bool use2D, PairBufferSoA& buffer, BroadphaseKernelContext& context);

namespace detail {

/// Legacy-path tail shared with the kernel path (spatial_hash.cpp): plane pairs for every dynamic
/// body, dedupe, max-capacity clamp.
void mergePlanePairsAndClamp(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes, PairBufferSoA& buffer,
                             BroadphaseScratch& scratch);

} // namespace detail

} // namespace fuse::physics::broadphase
