// CUDA backend of the batched SVO ray cast: a __global__ entry runs the same FUSE_HOST_DEVICE body
// (fuse/scene/svo_ray_kernel.hpp) the CPU backends run. This TU stages the flat node / brick / payload
// arrays and the rays in device memory, and hosts the resident-buffer CUDA-event benchmark.
//
// Parity: the TU is compiled with -fmad=false (Scene/CMakeLists.txt), so nvcc never contracts the DDA's
// mul + add into an fma; with IEEE division / sqrt (nvcc defaults) the walk takes the same decisions as
// the host build.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/scene/svo.hpp>
#include <fuse/scene/svo_ray_device.hpp>
#include <fuse/scene/svo_ray_kernel.hpp>

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>

namespace fuse::scene {

namespace {

namespace sk = svo_kernel;

constexpr u32 kThreads = sk::kWorkgroup.x;
// 128-thread blocks, at least 8 resident per SM: the compiler keeps the walk within 64 registers
// (32 of 48 warps on sm_86 before any other limit) without spilling the traversal state.
constexpr u32 kMinBlocksPerSm = 8;

__global__ void __launch_bounds__(kThreads, kMinBlocksPerSm)
    svo_ray_cast_kernel(sk::Params params, kernel::Dim3 grid, kernel::Dim3 workgroup) {
    const kernel::LaunchIndex idx = kernel::cuda::device_index(grid, workgroup);
    if (!idx.active) {
        return;
    }
    sk::Kernel{}(idx, params);
}

/// DeviceEntryFn for kernel::launch (stats / profiler scope) with the launch-bounded kernel above.
bool svo_entry(const kernel::KernelLaunch& launch, const void* /*body*/, const void* params, void* stream,
               bool synchronize) {
    const kernel::Dim3 groups = kernel::group_count(launch.grid, launch.workgroup);
    if (groups.count() == 0u) {
        return true;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    svo_ray_cast_kernel<<<::dim3(groups.x, groups.y, groups.z), ::dim3(launch.workgroup.x, 1u, 1u), 0, cudaStream>>>(
        *static_cast<const sk::Params*>(params), launch.grid, launch.workgroup);
    if (cudaGetLastError() != cudaSuccess) {
        return false;
    }
    return !synchronize || cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

/// Device copies of an SVO's ray-walk layout (+ rays / hits) for one or more launches: 8-byte packed
/// nodes in depth-first order, the brick records in the same order and their repacked payload words.
struct DeviceSvo {
    kernel::cuda::DeviceBuffer<sk::PackedNode> nodes;
    kernel::cuda::DeviceBuffer<SVOBrick> bricks;
    kernel::cuda::DeviceBuffer<u32> pool;
    kernel::cuda::DeviceBuffer<SvoRay> rays;
    kernel::cuda::DeviceBuffer<SvoRayHit> hits;

    bool stage(const SvoRayLayout& layout, const sk::Params& host, u32 count, cudaStream_t stream,
               sk::Params& device) {
        const usize nodeCount = layout.nodes.size();
        const usize brickCount = layout.bricks.size();
        const usize poolCount = layout.pool.size();
        bool ok = nodes.allocate(nodeCount) && bricks.allocate(brickCount) && pool.allocate(poolCount) &&
                  rays.allocate(count) && hits.allocate(count);
        ok = ok && nodes.upload(layout.nodes.data(), nodeCount, stream) &&
             bricks.upload(layout.bricks.data(), brickCount, stream) &&
             pool.upload(layout.pool.data(), poolCount, stream) && rays.upload(host.rays.data, count, stream);
        if (!ok) {
            return false;
        }
        device = host;
        device.svo = layout.view();
        device.svo.packed = kernel::make_span<const sk::PackedNode>(nodes.data(), static_cast<u32>(nodeCount));
        device.svo.bricks = kernel::make_span<const SVOBrick>(bricks.data(), static_cast<u32>(brickCount));
        device.svo.pool = kernel::make_span<const u32>(pool.data(), static_cast<u32>(poolCount));
        device.rays = kernel::make_span<const SvoRay>(rays.data(), count);
        device.hits = kernel::make_span(hits.data(), count);
        return true;
    }
};

/// RAII CUDA event.
struct Event {
    cudaEvent_t event = nullptr;
    Event() { (void)cudaEventCreate(&event); }
    ~Event() {
        if (event != nullptr) {
            (void)cudaEventDestroy(event);
        }
    }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
};

f32 elapsedMs(const Event& a, const Event& b) {
    float ms = 0.f;
    return cudaEventElapsedTime(&ms, a.event, b.event) == cudaSuccess ? ms : -1.f;
}

} // namespace

bool launchSvoRayCastCuda(const svo_kernel::Params& params, void* stream) {
    if (!sk::params_valid(params)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const u32 count = params.rays.size;
    const SvoRayLayout layout = buildSvoRayLayout(params.svo);
    DeviceSvo staged{};
    sk::Params device{};
    if (!staged.stage(layout, params, count, cudaStream, device)) {
        return false;
    }
    kernel::LaunchOptions options{};
    options.cuda = &svo_entry;
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = true;
    bool ok = kernel::launch(kernel::Backend::Cuda, sk::make_launch(count), sk::Kernel{}, device, options).ok;
    ok = ok && staged.hits.download(params.hits.data, count, cudaStream);
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

bool benchmarkSvoRayCastCuda(const SVO& svo, const SvoRay* rays, SvoRayHit* hits, u32 count, u32 iterations,
                             SvoRayDeviceTiming& timing) {
    if (!kernel::backend_available(kernel::Backend::Cuda) || count == 0u || rays == nullptr || hits == nullptr) {
        return false;
    }
    sk::Params host{};
    host.svo = svo.view();
    host.rays = kernel::make_span(rays, count);
    host.hits = kernel::make_span(hits, count);
    if (!sk::params_valid(host)) {
        return false;
    }
    cudaStream_t stream = nullptr;
    if (cudaStreamCreate(&stream) != cudaSuccess) {
        return false;
    }
    SvoRayDeviceTiming result{};
    result.iterations = std::max(iterations, 1u);
    result.threads_per_block = kThreads;
    const auto l0 = std::chrono::steady_clock::now();
    const SvoRayLayout layout = svo.rayLayout();
    result.layout_ms = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - l0).count();
    result.layout_bytes = layout.nodes.size() * sizeof(sk::PackedNode) + layout.bricks.size() * sizeof(SVOBrick) +
                          layout.pool.size() * sizeof(u32);
    bool ok = true;
    {
        DeviceSvo staged{};
        sk::Params device{};
        Event up0;
        Event up1;
        ok = cudaEventRecord(up0.event, stream) == cudaSuccess && staged.stage(layout, host, count, stream, device) &&
             cudaEventRecord(up1.event, stream) == cudaSuccess && cudaStreamSynchronize(stream) == cudaSuccess;
        result.upload_ms = ok ? elapsedMs(up0, up1) : 0.f;

        const kernel::KernelLaunch launch = sk::make_launch(count);
        Event k0;
        Event k1;
        f32 total = 0.f;
        result.kernel_ms_min = 3.4e38f;
        for (u32 i = 0; ok && i < result.iterations; ++i) {
            ok = cudaEventRecord(k0.event, stream) == cudaSuccess && svo_entry(launch, nullptr, &device, stream, false) &&
                 cudaEventRecord(k1.event, stream) == cudaSuccess && cudaEventSynchronize(k1.event) == cudaSuccess;
            const f32 ms = ok ? elapsedMs(k0, k1) : 0.f;
            total += ms;
            result.kernel_ms_min = std::min(result.kernel_ms_min, ms);
        }
        result.kernel_ms_avg = total / static_cast<f32>(result.iterations);

        Event d0;
        Event d1;
        ok = ok && cudaEventRecord(d0.event, stream) == cudaSuccess && staged.hits.download(hits, count, stream) &&
             cudaEventRecord(d1.event, stream) == cudaSuccess && cudaStreamSynchronize(stream) == cudaSuccess;
        result.download_ms = ok ? elapsedMs(d0, d1) : 0.f;
    }

    cudaFuncAttributes attributes{};
    if (cudaFuncGetAttributes(&attributes, svo_ray_cast_kernel) == cudaSuccess) {
        result.registers_per_thread = attributes.numRegs;
        result.local_bytes_per_thread = static_cast<s32>(attributes.localSizeBytes);
    }
    int blocks = 0;
    int deviceId = 0;
    cudaDeviceProp props{};
    if (cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks, svo_ray_cast_kernel, static_cast<int>(kThreads), 0) ==
            cudaSuccess &&
        cudaGetDevice(&deviceId) == cudaSuccess && cudaGetDeviceProperties(&props, deviceId) == cudaSuccess &&
        props.maxThreadsPerMultiProcessor > 0) {
        result.blocks_per_sm = blocks;
        result.theoretical_occupancy = static_cast<f32>(blocks * static_cast<int>(kThreads)) /
                                       static_cast<f32>(props.maxThreadsPerMultiProcessor);
    }
    (void)cudaStreamDestroy(stream);
    if (ok) {
        timing = result;
    }
    return ok;
}

} // namespace fuse::scene
