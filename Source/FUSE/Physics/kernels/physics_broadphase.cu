// CUDA backend of the physics broadphase kernels: the __global__ trampolines from cuda_launch.cuh run
// the same FUSE_HOST_DEVICE bodies (fuse/physics/broadphase/broadphase_kernel.hpp) the CPU backends
// run. This TU instantiates the device entries of every broadphase kernel (broadphaseDeviceEntries)
// and stages the (key, value) arrays of the broadphase radix sort in device memory to run the shared
// launch sequence (radix_sort_launch.hpp) — the path behind KernelRadixSorter / radixSortKernel for
// Backend::Cuda / Auto when a device is present.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/physics/broadphase/broadphase_kernel.hpp>
#include <fuse/physics/broadphase/radix_sort_launch.hpp>

#include <cuda_runtime.h>

namespace fuse::physics::broadphase_kernel {

BroadphaseDeviceEntries broadphaseDeviceEntries() {
    BroadphaseDeviceEntries e{};
    e.count = &kernel::cuda::entry<CellCountKernel, CellCountParams>;
    e.keys = &kernel::cuda::entry<CellKeysKernel, CellKeysParams>;
    e.boundaryFlags = &kernel::cuda::entry<BoundaryFlagKernel, BoundaryFlagParams>;
    e.boundaryWrite = &kernel::cuda::entry<BoundaryWriteKernel, BoundaryWriteParams>;
    e.cells = &kernel::cuda::entry<CellKernel, CellParams>;
    e.pairs = &kernel::cuda::entry<PairKernel, PairParams>;
    e.sort.scanBlocks = &kernel::cuda::entry<ScanBlocksKernel, ScanParams>;
    e.sort.scanAdd = &kernel::cuda::entry<ScanAddKernel, ScanAddParams>;
    e.sort.histogram = &kernel::cuda::entry<RadixHistogramKernel, RadixParams>;
    e.sort.scatter = &kernel::cuda::entry<RadixScatterKernel, RadixParams>;
    return e;
}

} // namespace fuse::physics::broadphase_kernel

namespace fuse::physics::broadphase {

bool radixSortCuda(u32* keys, u32* values, u32 count, u32 keyBits, void* stream) {
    namespace bk = broadphase_kernel;
    if (count < 2u) {
        return true;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const u32 numTiles = kernel::div_up(count, bk::kSortTile);
    const usize histCount = static_cast<usize>(numTiles) * bk::kBuckets;
    const bool withValues = values != nullptr;

    kernel::cuda::DeviceBuffer<u32> dKeys;
    kernel::cuda::DeviceBuffer<u32> dValues;
    kernel::cuda::DeviceBuffer<u32> dTmpKeys;
    kernel::cuda::DeviceBuffer<u32> dTmpValues;
    kernel::cuda::DeviceBuffer<u32> dHist;
    kernel::cuda::DeviceBuffer<u32> dLevels;
    bool ok = dKeys.allocate(count) && dTmpKeys.allocate(count) && dHist.allocate(histCount) &&
              dLevels.allocate(bk::planScan(static_cast<u32>(histCount)).storage) &&
              dKeys.upload(keys, count, cudaStream);
    if (ok && withValues) {
        ok = dValues.allocate(count) && dTmpValues.allocate(count) && dValues.upload(values, count, cudaStream);
    }
    if (!ok) {
        return false;
    }

    bk::SortEntries entries = bk::broadphaseDeviceEntries().sort;
    entries.stream = stream;
    bool inTemp = false;
    ok = bk::launchRadixSort(kernel::Backend::Cuda, dKeys.data(), withValues ? dValues.data() : nullptr,
                             dTmpKeys.data(), withValues ? dTmpValues.data() : nullptr, dHist.data(), dLevels.data(),
                             count, keyBits, entries, &inTemp);
    if (ok) {
        ok = (inTemp ? dTmpKeys : dKeys).download(keys, count, cudaStream);
    }
    if (ok && withValues) {
        ok = (inTemp ? dTmpValues : dValues).download(values, count, cudaStream);
    }
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

} // namespace fuse::physics::broadphase
