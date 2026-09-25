// Host orchestration of the single-source broadphase kernels (see broadphase_kernels.hpp).

#include <fuse/physics/broadphase/broadphase_kernel.hpp>
#include <fuse/physics/broadphase/broadphase_kernels.hpp>
#include <fuse/physics/broadphase/radix_sort_launch.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>

#include <algorithm>
#include <utility>

namespace fuse::physics::broadphase {
namespace {

namespace bk = broadphase_kernel;

/// Grow-only resize: steady-state frames never reallocate.
template <typename T>
void grow(std::vector<T>& v, usize size) {
    if (size > v.capacity()) {
        v.reserve(size + size / 2u);
    }
    v.resize(size);
}

template <typename Body, typename Params>
void launchItems(kernel::Backend backend, const char* name, u32 count, const Body& body, const Params& params) {
    if (count == 0u) {
        return;
    }
    kernel::launch(backend, kernel::KernelLaunch{name, kernel::extent1(count), bk::kItemWorkgroup}, body, params);
}

} // namespace

u32 exclusiveScanKernel(kernel::Backend backend, u32* data, u32 count, KernelScanScratch& scratch) {
    if (count == 0u) {
        return 0u;
    }
    const bk::ScanPlan plan = bk::planScan(count);
    grow(scratch.levels, plan.storage);
    bk::launchScan(backend, data, scratch.levels.data(), plan, bk::SortEntries{});
    return scratch.levels[plan.offsets[plan.levels]];
}

#if defined(FUSE_HAS_CUDA)
/// kernels/physics_broadphase.cu: stages keys / values on the device and runs the same kernels.
bool radixSortCuda(u32* keys, u32* values, u32 count, u32 keyBits, void* stream);
#endif

void radixSortKernel(kernel::Backend backend, u32* keys, u32* values, u32 count, u32 keyBits,
                     KernelRadixSortScratch& scratch) {
    if (count < 2u) {
        return;
    }
#if defined(FUSE_HAS_CUDA)
    if ((backend == kernel::Backend::Cuda || backend == kernel::Backend::Auto) &&
        kernel::backend_available(kernel::Backend::Cuda) && radixSortCuda(keys, values, count, keyBits, nullptr)) {
        return;
    }
#endif
    const u32 numTiles = kernel::div_up(count, bk::kSortTile);
    grow(scratch.keys, count);
    if (values != nullptr) {
        grow(scratch.values, count);
    }
    grow(scratch.hist, static_cast<usize>(numTiles) * bk::kBuckets);
    grow(scratch.scan.levels, bk::planScan(numTiles * bk::kBuckets).storage);
    bool inTemp = false;
    bk::launchRadixSort(backend, keys, values, scratch.keys.data(), values != nullptr ? scratch.values.data() : nullptr,
                        scratch.hist.data(), scratch.scan.levels.data(), count, keyBits, bk::SortEntries{}, &inTemp);
    if (inTemp) {
        std::copy(scratch.keys.begin(), scratch.keys.begin() + count, keys);
        if (values != nullptr) {
            std::copy(scratch.values.begin(), scratch.values.begin() + count, values);
        }
    }
}

namespace {

bool kernelSorterThunk(void* user, u32* keys, u32* values, u32 count, u32 keyBits) {
    KernelRadixSorter& self = *static_cast<KernelRadixSorter*>(user);
    radixSortKernel(self.backend, keys, values, count, keyBits, self.scratch);
    return true;
}

} // namespace

BroadphaseKeyValueSorter KernelRadixSorter::sorter() {
    BroadphaseKeyValueSorter hook{};
    hook.sort = &kernelSorterThunk;
    hook.user = this;
    hook.minCount = minCount;
    return hook;
}

void runBroadphaseKernels(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes, const SpatialHashParams& params,
                          bool use2D, PairBufferSoA& buffer, BroadphaseKernelContext& context) {
    BroadphaseKernelStats& stats = context.stats;
    stats = {};
    const u32 bits = bk::bodyIndexBits(bodies.count());
    if (bits * 2u > 32u) {
        // Packed pair keys need 2 * bits <= 32: huge scenes stay on the legacy (u64 key) path.
        if (use2D) {
            runBroadphase2DIntoBuffer(bodies, shapes, params, buffer, context.scratch.planes);
        } else {
            runBroadphaseIntoBuffer(bodies, shapes, params, buffer, context.scratch.planes);
        }
        return;
    }
    stats.usedKernels = true;

    buffer.clear();
    if (!preflightBroadphase(bodies, shapes).canRun()) {
        return;
    }

    const kernel::Backend backend = context.backend;
    BroadphaseKernelScratch& s = context.scratch;
    const SpatialHashParams normalized = normalizeSpatialHashParams(params);
    const u32 shapeCount = shapes.count();
    const bk::ShapeView view = bk::makeShapeView(bodies, shapes, normalized, use2D);

    // 1. Shape -> cells: count, scan, write (key, body) entries.
    grow(s.shapeOffsets, shapeCount);
    launchItems(backend, bk::kCountName, shapeCount, bk::CellCountKernel{},
                bk::CellCountParams{view, s.shapeOffsets.data()});
    const u32 entries = exclusiveScanKernel(backend, s.shapeOffsets.data(), shapeCount, s.scan);
    grow(s.entryKeys, entries);
    grow(s.entryBodies, entries);
    launchItems(backend, bk::kKeysName, shapeCount, bk::CellKeysKernel{},
                bk::CellKeysParams{view, s.shapeOffsets.data(), s.entryKeys.data(), s.entryBodies.data()});
    stats.entries = entries;

    // 2. Group entries by cell: stable radix sort of (key, body) — the counting sort's order.
    radixSortKernel(backend, s.entryKeys.data(), s.entryBodies.data(), entries,
                    bk::bodyIndexBits(normalized.tableSize), s.sort);

    // 3. Cells = runs of equal keys: flags, scan, starts.
    grow(s.runIndex, entries);
    launchItems(backend, bk::kRunsName, entries, bk::BoundaryFlagKernel{},
                bk::BoundaryFlagParams{s.entryKeys.data(), s.runIndex.data()});
    const u32 runs = exclusiveScanKernel(backend, s.runIndex.data(), entries, s.scan);
    grow(s.runStarts, static_cast<usize>(runs) + 1u);
    launchItems(backend, bk::kRunStartsName, entries, bk::BoundaryWriteKernel{},
                bk::BoundaryWriteParams{s.entryKeys.data(), s.runIndex.data(), s.runStarts.data()});
    s.runStarts[runs] = entries;
    stats.cells = runs;

    // 4. Per cell: sort + unique occupants, pair count; scan; write packed canonical pairs.
    grow(s.uniqueCounts, runs);
    grow(s.pairOffsets, runs);
    launchItems(backend, bk::kCellsName, runs, bk::CellKernel{},
                bk::CellParams{s.runStarts.data(), s.entryBodies.data(), s.uniqueCounts.data(), s.pairOffsets.data()});
    const u32 cellPairs = exclusiveScanKernel(backend, s.pairOffsets.data(), runs, s.scan);
    stats.cellPairs = cellPairs;

    if (shouldRunBroadphaseCellPairGen(cellPairs)) {
        grow(s.pairKeys, cellPairs);
        bk::PairParams pairParams{};
        pairParams.runStarts = s.runStarts.data();
        pairParams.bodies = s.entryBodies.data();
        pairParams.uniqueCounts = s.uniqueCounts.data();
        pairParams.pairOffsets = s.pairOffsets.data();
        pairParams.bits = bits;
        pairParams.pairKeys = s.pairKeys.data();
        launchItems(backend, bk::kPairsName, runs, bk::PairKernel{}, pairParams);

        // 5. Dedupe (a pair sharing several cells is emitted once per cell): sort keys, unique.
        radixSortKernel(backend, s.pairKeys.data(), nullptr, cellPairs, bits * 2u, s.sort);
        grow(s.pairIndex, cellPairs);
        launchItems(backend, bk::kPairFlagsName, cellPairs, bk::BoundaryFlagKernel{},
                    bk::BoundaryFlagParams{s.pairKeys.data(), s.pairIndex.data()});
        const u32 unique = exclusiveScanKernel(backend, s.pairIndex.data(), cellPairs, s.scan);
        grow(s.pairStarts, unique);
        launchItems(backend, bk::kPairUniqueName, cellPairs, bk::BoundaryWriteKernel{},
                    bk::BoundaryWriteParams{s.pairKeys.data(), s.pairIndex.data(), s.pairStarts.data()});
        stats.uniquePairs = unique;

        // Same buffer state the legacy dedupe leaves: clear, then push in sorted order (the push
        // applies the max-capacity drop accounting).
        if (unique > buffer.bodyA.capacity()) {
            buffer.reserve(unique + unique / 2u);
        }
        buffer.clear();
        const u32 lowMask = (1u << bits) - 1u;
        for (u32 i = 0; i < unique; ++i) {
            const u32 key = s.pairKeys[s.pairStarts[i]];
            buffer.push(key >> bits, key & lowMask);
        }
    }

    detail::mergePlanePairsAndClamp(bodies, shapes, buffer, s.planes);
}

} // namespace fuse::physics::broadphase
