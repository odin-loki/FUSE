#pragma once

// CUDA backend of the single-source kernel model: __global__ trampolines that call the same
// FUSE_HOST_DEVICE body the CPU backends run. Include ONLY from .cu translation units (nvcc).
//
// Per kernel, one .cu TU instantiates the entry and hands it to host code:
//
//     #include <fuse/compute_kernel/cuda_launch.cuh>
//     fuse::kernel::DeviceEntryFn my_kernel_cuda_entry() {
//         return &fuse::kernel::cuda::entry<MyKernel, MyParams>;
//     }
//
// and host code launches with `kernel::launch(Backend::Cuda, desc, MyKernel{}, deviceParams,
// {.cuda = my_kernel_cuda_entry()})`. Params must point at device-visible memory (cudaMalloc /
// managed); staging host data is the kernel host wrapper's job (see ray_march.cu for the pattern).
//
// Mapping: CUDA block = workgroup, grid of blocks = group_count(grid, workgroup). Item kernels mask
// out-of-grid threads. Workgroup kernels get static __shared__ scratch and run
// `for phase: body(...); __syncthreads();` on every thread of the block (padding threads included,
// with idx.active == false) — exactly the CPU emulation's phase-major order.

#if !defined(__CUDACC__)
#error "fuse/compute_kernel/cuda_launch.cuh must only be included from CUDA (.cu) translation units"
#endif

#include <fuse/compute_kernel/kernel.hpp>

#include <cuda_runtime.h>

namespace fuse::kernel::cuda {

__device__ inline LaunchIndex device_index(const Dim3& grid, const Dim3& workgroup) {
    return make_index(grid, workgroup, Dim3{blockIdx.x, blockIdx.y, blockIdx.z},
                      Dim3{threadIdx.x, threadIdx.y, threadIdx.z});
}

template <typename Body, typename Params>
__global__ void item_trampoline(Body body, Params params, Dim3 grid, Dim3 workgroup) {
    const LaunchIndex idx = device_index(grid, workgroup);
    if (!idx.active) {
        return;
    }
    body(idx, params);
}

template <typename Body, typename Params>
__global__ void workgroup_trampoline(Body body, Params params, Dim3 grid, Dim3 workgroup) {
    using Scratch = typename Body::Scratch;
    // Raw bytes: __shared__ variables cannot have constructors (Scratch may have default member
    // initialisers). Contents are undefined at entry on every backend — phase 0 must initialise.
    __shared__ __align__(16) unsigned char raw[sizeof(Scratch) * Body::kScratchCount];
    WorkgroupContext<Scratch> ctx{};
    ctx.scratch = reinterpret_cast<Scratch*>(raw);
    ctx.scratch_count = Body::kScratchCount;
    ctx.phase_count = Body::kPhases;
    const LaunchIndex idx = device_index(grid, workgroup);
    for (u32 phase = 0; phase < Body::kPhases; ++phase) {
        ctx.phase = phase;
        body(idx, ctx, params);
        __syncthreads();
    }
}

/// Type-erased DeviceEntryFn for <Body, Params>. Launches on `stream` (cudaStream_t, may be null);
/// returns false when the launch (or, with `synchronize`, the kernel) fails.
template <typename Body, typename Params>
bool entry(const KernelLaunch& launch, const void* body, const void* params, void* stream, bool synchronize) {
    static_assert(check_kernel_types<Body, Params>());
    const Dim3 groups = group_count(launch.grid, launch.workgroup);
    if (groups.count() == 0u) {
        return true;
    }
    const ::dim3 blocks(groups.x, groups.y, groups.z);
    const ::dim3 threads(launch.workgroup.x, launch.workgroup.y, launch.workgroup.z);
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const Body& typedBody = *static_cast<const Body*>(body);
    const Params& typedParams = *static_cast<const Params*>(params);
    if constexpr (is_workgroup_kernel_v<Body>) {
        workgroup_trampoline<Body, Params>
            <<<blocks, threads, 0, cudaStream>>>(typedBody, typedParams, launch.grid, launch.workgroup);
    } else {
        item_trampoline<Body, Params>
            <<<blocks, threads, 0, cudaStream>>>(typedBody, typedParams, launch.grid, launch.workgroup);
    }
    if (cudaGetLastError() != cudaSuccess) {
        return false;
    }
    return !synchronize || cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

/// True when the CUDA runtime reports at least one device (no stream / context side effects).
inline bool device_present() {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

/// Minimal RAII device buffer for kernel host wrappers that stage host data.
template <typename T>
class DeviceBuffer {
public:
    DeviceBuffer() = default;
    explicit DeviceBuffer(usize count) { allocate(count); }
    ~DeviceBuffer() { release(); }
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    bool allocate(usize count) {
        release();
        if (count == 0u) {
            return true;
        }
        void* ptr = nullptr;
        if (cudaMalloc(&ptr, count * sizeof(T)) != cudaSuccess) {
            return false;
        }
        m_data = static_cast<T*>(ptr);
        m_count = count;
        return true;
    }
    bool upload(const T* host, usize count, cudaStream_t stream) {
        return count <= m_count &&
               (count == 0u ||
                cudaMemcpyAsync(m_data, host, count * sizeof(T), cudaMemcpyHostToDevice, stream) == cudaSuccess);
    }
    bool download(T* host, usize count, cudaStream_t stream) const {
        return count <= m_count &&
               (count == 0u ||
                cudaMemcpyAsync(host, m_data, count * sizeof(T), cudaMemcpyDeviceToHost, stream) == cudaSuccess);
    }
    void release() {
        if (m_data != nullptr) {
            cudaFree(m_data);
        }
        m_data = nullptr;
        m_count = 0;
    }
    T* data() const { return m_data; }
    usize size() const { return m_count; }

private:
    T* m_data = nullptr;
    usize m_count = 0;
};

} // namespace fuse::kernel::cuda
