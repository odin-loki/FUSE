#pragma once

// Launch sequences of the physics scan / radix sort kernels (broadphase_kernel.hpp), shared by the
// CPU host path (broadphase_kernels.cpp, host memory) and the CUDA wrapper
// (kernels/physics_broadphase.cu, device memory + cuda::entry<> trampolines). Host-side only.

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/physics/broadphase/broadphase_kernel.hpp>
#include <fuse/types.hpp>

#include <utility>

namespace fuse::physics::broadphase_kernel {

/// Device entries for Backend::Cuda (null on the CPU path).
struct SortEntries {
    kernel::DeviceEntryFn scanBlocks = nullptr;
    kernel::DeviceEntryFn scanAdd = nullptr;
    kernel::DeviceEntryFn histogram = nullptr;
    kernel::DeviceEntryFn scatter = nullptr;
    void* stream = nullptr;
};

inline kernel::LaunchOptions entryOptions(kernel::DeviceEntryFn entry, const SortEntries& entries) {
    kernel::LaunchOptions options{};
    options.cuda = entry;
    options.stream = entries.stream;
    options.allow_fallback = entry == nullptr; // a CUDA launch on device memory must not fall back
    return options;
}

/// Multi-level scan layout: level i + 1 holds the 1024-element chunk totals of level i; the last
/// level has one entry, the grand total. `storage` u32s hold levels 1..levels.
struct ScanPlan {
    static constexpr u32 kMaxLevels = 8u;
    u32 levels = 0;
    u32 lengths[kMaxLevels + 1u]{};
    u32 offsets[kMaxLevels + 1u]{};
    usize storage = 0;
};

inline ScanPlan planScan(u32 count) {
    ScanPlan plan{};
    plan.lengths[0] = count;
    while (count > 0u) {
        const u32 chunks = kernel::div_up(plan.lengths[plan.levels], kScanChunk);
        ++plan.levels;
        plan.lengths[plan.levels] = chunks;
        plan.offsets[plan.levels] = static_cast<u32>(plan.storage);
        plan.storage += chunks;
        if (chunks <= 1u || plan.levels == ScanPlan::kMaxLevels) {
            break;
        }
    }
    return plan;
}

/// Exclusive scan of data[0..count) in place; the total lands in levelStorage[plan.offsets[plan.levels]].
inline bool launchScan(kernel::Backend backend, u32* data, u32* levelStorage, const ScanPlan& plan,
                       const SortEntries& entries) {
    const auto level = [&](u32 i) { return i == 0u ? data : levelStorage + plan.offsets[i]; };
    bool ok = true;
    for (u32 i = 0; i < plan.levels && ok; ++i) {
        const kernel::KernelLaunch launch{kScanName, kernel::extent1(plan.lengths[i + 1u] * kScanWorkgroup),
                                          kernel::Dim3{kScanWorkgroup, 1u, 1u}};
        ok = kernel::launch(backend, launch, ScanBlocksKernel{}, ScanParams{level(i), plan.lengths[i], level(i + 1u)},
                            entryOptions(entries.scanBlocks, entries))
                 .ok;
    }
    for (u32 i = plan.levels > 0u ? plan.levels - 1u : 0u; i-- > 0u && ok;) {
        if (plan.lengths[i + 1u] > 1u) {
            const kernel::KernelLaunch launch{kScanAddName, kernel::extent1(plan.lengths[i]), kItemWorkgroup};
            ok = kernel::launch(backend, launch, ScanAddKernel{}, ScanAddParams{level(i), level(i + 1u)},
                                entryOptions(entries.scanAdd, entries))
                     .ok;
        }
    }
    return ok;
}

/// Radix sort passes over (keys, values) ping-ponging with (tmpKeys, tmpValues); `hist` holds
/// kBuckets * tiles, `scanLevels` planScan(kBuckets * tiles).storage. Returns true in `*inTemp`
/// when the sorted data ended up in the temp arrays (odd pass count). values may be null.
inline bool launchRadixSort(kernel::Backend backend, u32* keys, u32* values, u32* tmpKeys, u32* tmpValues, u32* hist,
                            u32* scanLevels, u32 count, u32 keyBits, const SortEntries& entries, bool* inTemp) {
    *inTemp = false;
    if (count < 2u) {
        return true;
    }
    const u32 passes = kernel::div_up(keyBits < 1u ? 1u : (keyBits > 32u ? 32u : keyBits), kDigitBits);
    const u32 numTiles = kernel::div_up(count, kSortTile);
    const ScanPlan plan = planScan(numTiles * kBuckets);
    const kernel::Dim3 grid = kernel::extent1(numTiles * kSortThreads);
    const kernel::KernelLaunch histLaunch{kSortHistName, grid, kernel::Dim3{kSortThreads, 1u, 1u}};
    const kernel::KernelLaunch scatterLaunch{kSortName, grid, kernel::Dim3{kSortThreads, 1u, 1u}};
    u32* srcKeys = keys;
    u32* srcVals = values;
    u32* dstKeys = tmpKeys;
    u32* dstVals = values != nullptr ? tmpValues : nullptr;
    bool ok = true;
    for (u32 pass = 0; pass < passes && ok; ++pass) {
        RadixParams params{};
        params.keysIn = srcKeys;
        params.valsIn = srcVals;
        params.keysOut = dstKeys;
        params.valsOut = dstVals;
        params.count = count;
        params.shift = pass * kDigitBits;
        params.numTiles = numTiles;
        params.hist = hist;
        ok = kernel::launch(backend, histLaunch, RadixHistogramKernel{}, params, entryOptions(entries.histogram, entries))
                 .ok &&
             launchScan(backend, hist, scanLevels, plan, entries) &&
             kernel::launch(backend, scatterLaunch, RadixScatterKernel{}, params, entryOptions(entries.scatter, entries))
                 .ok;
        std::swap(srcKeys, dstKeys);
        std::swap(srcVals, dstVals);
    }
    *inTemp = (passes % 2u) == 1u;
    return ok;
}

/// CUDA entries of every broadphase kernel (kernels/physics_broadphase.cu), for a device-resident
/// broadphase that keeps the SoA on the GPU. Null members when not built with CUDA.
struct BroadphaseDeviceEntries {
    kernel::DeviceEntryFn count = nullptr;
    kernel::DeviceEntryFn keys = nullptr;
    kernel::DeviceEntryFn boundaryFlags = nullptr;
    kernel::DeviceEntryFn boundaryWrite = nullptr;
    kernel::DeviceEntryFn cells = nullptr;
    kernel::DeviceEntryFn pairs = nullptr;
    SortEntries sort{};
};

#if defined(FUSE_HAS_CUDA)
BroadphaseDeviceEntries broadphaseDeviceEntries();
#endif

} // namespace fuse::physics::broadphase_kernel
