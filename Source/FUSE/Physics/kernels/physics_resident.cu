// CUDA backend of the resident physics pipeline (fuse/physics/resident/resident_physics.hpp): the
// __global__ trampolines of cuda_launch.cuh around the same FUSE_HOST_DEVICE bodies the CPU backends
// run (resident_kernels.hpp, broadphase_kernel.hpp), plus the memory / stream / event helpers the host
// orchestration (src/resident/resident_physics.cpp) calls through resident_cuda_backend.hpp.
//
// Built with --fmad=false (Source/FUSE/Physics/CMakeLists.txt): nvcc would otherwise contract
// a * b + c into FMAs the x86-64 host build never emits, and the device results would drift from the
// CpuReference by an ulp here and there. Without contraction every +, -, *, / and sqrtf is IEEE
// round-to-nearest on both sides, so the device gate can compare against the CPU bit for bit.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/physics/broadphase/broadphase_kernel.hpp>
#include <fuse/physics/resident/resident_cuda_backend.hpp>
#include <fuse/physics/resident/resident_kernels.hpp>

#include <cuda_runtime.h>

namespace fuse::physics::resident::cuda_backend {

namespace bk = broadphase_kernel;

bool deviceUsable() { return kernel::cuda::device_present(); }

Entries entries() {
    Entries e{};
    e.cellCount = &kernel::cuda::entry<bk::CellCountKernel, bk::CellCountParams>;
    e.keys = &kernel::cuda::entry<KeysKernel, KeysParams>;
    e.pad = &kernel::cuda::entry<PadKernel, PadParams>;
    e.runFlags = &kernel::cuda::entry<RunFlagKernel, RunFlagParams>;
    e.runStarts = &kernel::cuda::entry<RunStartKernel, RunStartParams>;
    e.cells = &kernel::cuda::entry<CellsKernel, CellsParams>;
    e.pairs = &kernel::cuda::entry<PairsKernel, PairsParams>;
    e.planePairs = &kernel::cuda::entry<PlanePairsKernel, PlanePairsParams>;
    e.uniqueFlags = &kernel::cuda::entry<UniqueFlagKernel, UniqueFlagParams>;
    e.uniqueWrite = &kernel::cuda::entry<UniqueWriteKernel, UniqueWriteParams>;
    e.status = &kernel::cuda::entry<StatusKernel, StatusParams>;
    e.detect = &kernel::cuda::entry<DetectKernel, DetectParams>;
    e.compact = &kernel::cuda::entry<CompactKernel, CompactParams>;
    e.solveColor = &kernel::cuda::entry<SolveColorKernel, SolveParams>;
    e.solveSerial = &kernel::cuda::entry<SolveSerialKernel, SolveParams>;
    e.sort.scanBlocks = &kernel::cuda::entry<bk::ScanBlocksKernel, bk::ScanParams>;
    e.sort.scanAdd = &kernel::cuda::entry<bk::ScanAddKernel, bk::ScanAddParams>;
    e.sort.histogram = &kernel::cuda::entry<bk::RadixHistogramKernel, bk::RadixParams>;
    e.sort.scatter = &kernel::cuda::entry<bk::RadixScatterKernel, bk::RadixParams>;
    return e;
}

bool createStream(void** stream) {
    cudaStream_t s = nullptr;
    if (cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking) != cudaSuccess) {
        return false;
    }
    *stream = s;
    return true;
}

void destroyStream(void* stream) {
    if (stream != nullptr) {
        cudaStreamSynchronize(static_cast<cudaStream_t>(stream));
        cudaStreamDestroy(static_cast<cudaStream_t>(stream));
    }
}

bool allocate(void** pointer, usize bytes) { return cudaMalloc(pointer, bytes) == cudaSuccess; }

void release(void* pointer) {
    if (pointer != nullptr) {
        cudaFree(pointer);
    }
}

bool copyAsync(void* dst, const void* src, usize bytes, CopyKind kind, void* stream) {
    cudaMemcpyKind k = cudaMemcpyHostToDevice;
    if (kind == CopyKind::DeviceToHost) {
        k = cudaMemcpyDeviceToHost;
    } else if (kind == CopyKind::DeviceToDevice) {
        k = cudaMemcpyDeviceToDevice;
    }
    return cudaMemcpyAsync(dst, src, bytes, k, static_cast<cudaStream_t>(stream)) == cudaSuccess;
}

bool fillAsync(void* dst, int value, usize bytes, void* stream) {
    return cudaMemsetAsync(dst, value, bytes, static_cast<cudaStream_t>(stream)) == cudaSuccess;
}

bool synchronize(void* stream) {
    return cudaStreamSynchronize(static_cast<cudaStream_t>(stream)) == cudaSuccess &&
           cudaGetLastError() == cudaSuccess;
}

bool createEvent(void** event) {
    cudaEvent_t e = nullptr;
    if (cudaEventCreate(&e) != cudaSuccess) {
        return false;
    }
    *event = e;
    return true;
}

void destroyEvent(void* event) {
    if (event != nullptr) {
        cudaEventDestroy(static_cast<cudaEvent_t>(event));
    }
}

bool recordEvent(void* event, void* stream) {
    return cudaEventRecord(static_cast<cudaEvent_t>(event), static_cast<cudaStream_t>(stream)) == cudaSuccess;
}

bool elapsedMs(void* start, void* stop, float* ms) {
    return cudaEventSynchronize(static_cast<cudaEvent_t>(stop)) == cudaSuccess &&
           cudaEventElapsedTime(ms, static_cast<cudaEvent_t>(start), static_cast<cudaEvent_t>(stop)) == cudaSuccess;
}

} // namespace fuse::physics::resident::cuda_backend
