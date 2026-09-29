// CUDA backend of the SDF ray march: the __global__ trampolines from cuda_launch.cuh run the same
// FUSE_HOST_DEVICE bodies (fuse/compute/ray_march_kernel.hpp) the CPU backends run — the full-scene
// march ("sdf_ray_march") and the per-tile culled march ("sdf_ray_march_tiled"). This TU only stages
// the scene and surfaces in device memory — the pattern for every ported kernel's CUDA host wrapper —
// and hosts the resident-buffer CUDA-event benchmark.

#include <fuse/compute/ray_march.hpp>
#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>

#include <cuda_runtime.h>

#include <algorithm>

namespace fuse::compute {

namespace {

namespace rm = ray_march_kernel;

constexpr u32 kThreads = rm::kWorkgroup.x * rm::kWorkgroup.y;
// Tiled march: 128-thread tiles, at least 8 resident per SM -> the compiler keeps the culling and march
// phases within 64 registers (32 of 48 warps on sm_86); unbounded it took 95 (20 warps).
constexpr u32 kTiledMinBlocksPerSm = 8;

/// The workgroup trampoline of cuda_launch.cuh for the tiled march, with launch bounds.
__global__ void __launch_bounds__(kThreads, kTiledMinBlocksPerSm)
    ray_march_tiled_kernel(rm::Params params, kernel::Dim3 grid, kernel::Dim3 workgroup) {
    using Scratch = rm::TiledKernel::Scratch;
    __shared__ __align__(16) unsigned char raw[sizeof(Scratch) * rm::TiledKernel::kScratchCount];
    kernel::WorkgroupContext<Scratch> ctx{};
    ctx.scratch = reinterpret_cast<Scratch*>(raw);
    ctx.scratch_count = rm::TiledKernel::kScratchCount;
    ctx.phase_count = rm::TiledKernel::kPhases;
    const kernel::LaunchIndex idx = kernel::cuda::device_index(grid, workgroup);
    for (u32 phase = 0; phase < rm::TiledKernel::kPhases; ++phase) {
        ctx.phase = phase;
        rm::TiledKernel{}(idx, ctx, params);
        __syncthreads();
    }
}

bool tiledEntry(const kernel::KernelLaunch& launch, const void* /*body*/, const void* params, void* stream,
                bool synchronize) {
    const kernel::Dim3 groups = kernel::group_count(launch.grid, launch.workgroup);
    if (groups.count() == 0u) {
        return true;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    ray_march_tiled_kernel<<<::dim3(groups.x, groups.y, groups.z),
                             ::dim3(launch.workgroup.x, launch.workgroup.y, launch.workgroup.z), 0, cudaStream>>>(
        *static_cast<const rm::Params*>(params), launch.grid, launch.workgroup);
    if (cudaGetLastError() != cudaSuccess) {
        return false;
    }
    return !synchronize || cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

/// Scene objects + output surfaces in device memory.
struct DeviceFrame {
    kernel::cuda::DeviceBuffer<SdfObject> objects;
    kernel::cuda::DeviceBuffer<f32> depth;
    kernel::cuda::DeviceBuffer<math::Vec4> normals;
    usize pixels = 0;

    bool stage(const RayMarchParams& params, cudaStream_t stream, RayMarchParams& device) {
        pixels = static_cast<usize>(params.width) * params.height;
        bool ok = objects.allocate(params.object_count) && objects.upload(params.objects, params.object_count, stream);
        if (ok && params.depth_surface != nullptr) {
            ok = depth.allocate(pixels);
        }
        if (ok && params.output_surface != nullptr) {
            ok = normals.allocate(pixels);
        }
        if (!ok) {
            return false;
        }
        device = params;
        device.objects = objects.data();
        device.depth_surface = params.depth_surface != nullptr ? depth.data() : nullptr;
        device.output_surface = params.output_surface != nullptr ? normals.data() : nullptr;
        return true;
    }

    bool download(const RayMarchParams& params, cudaStream_t stream) const {
        bool ok = true;
        if (params.depth_surface != nullptr) {
            ok = depth.download(static_cast<f32*>(params.depth_surface), pixels, stream);
        }
        if (ok && params.output_surface != nullptr) {
            ok = normals.download(static_cast<math::Vec4*>(params.output_surface), pixels, stream);
        }
        return ok;
    }
};

bool launchOnDevice(const RayMarchParams& params, bool tiled, void* stream) {
    if (!rm::params_valid(params)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    DeviceFrame frame{};
    RayMarchParams deviceScene{};
    if (!frame.stage(params, cudaStream, deviceScene)) {
        return false;
    }
    kernel::LaunchOptions options{};
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = true;
    bool ok = false;
    if (tiled) {
        options.cuda = &tiledEntry;
        ok = kernel::launch(kernel::Backend::Cuda, rm::make_tiled_launch(params), rm::TiledKernel{},
                            rm::make_tiled_params(deviceScene, params.objects), options)
                 .ok;
    } else {
        options.cuda = &kernel::cuda::entry<rm::Kernel, rm::Params>;
        ok = kernel::launch(kernel::Backend::Cuda, rm::make_launch(params), rm::Kernel{}, rm::make_params(deviceScene),
                            options)
                 .ok;
    }
    ok = ok && frame.download(params, cudaStream);
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

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

template <typename KernelFn>
void describe(KernelFn kernelFn, u32 threads, RayMarchDeviceTiming& out) {
    cudaFuncAttributes attributes{};
    if (cudaFuncGetAttributes(&attributes, kernelFn) == cudaSuccess) {
        out.registers_per_thread = attributes.numRegs;
        out.shared_bytes_per_block = static_cast<s32>(attributes.sharedSizeBytes);
    }
    int device = 0;
    int perSm = 0;
    cudaDeviceProp props{};
    if (cudaOccupancyMaxActiveBlocksPerMultiprocessor(&perSm, kernelFn, static_cast<int>(threads), 0) == cudaSuccess &&
        cudaGetDevice(&device) == cudaSuccess && cudaGetDeviceProperties(&props, device) == cudaSuccess &&
        props.maxThreadsPerMultiProcessor > 0) {
        out.blocks_per_sm = perSm;
        out.theoretical_occupancy =
            static_cast<f32>(perSm * static_cast<int>(threads)) / static_cast<f32>(props.maxThreadsPerMultiProcessor);
    }
}

} // namespace

bool launchRayMarchCuda(const RayMarchParams& params, void* stream) {
    return launchOnDevice(params, false, stream);
}

bool launchRayMarchTiledCuda(const RayMarchParams& params, void* stream) {
    return launchOnDevice(params, true, stream);
}

kernel::DeviceEntryFn ray_march_tiled_cuda_entry() {
    return &tiledEntry;
}

bool benchmark_ray_march_cuda(const RayMarchParams& params, bool tiled, u32 iterations, RayMarchDeviceTiming& timing) {
    if (!kernel::backend_available(kernel::Backend::Cuda) || !rm::params_valid(params)) {
        return false;
    }
    cudaStream_t stream = nullptr;
    if (cudaStreamCreate(&stream) != cudaSuccess) {
        return false;
    }
    RayMarchDeviceTiming result{};
    result.iterations = std::max(iterations, 1u);
    bool ok = true;
    {
        DeviceFrame frame{};
        RayMarchParams deviceScene{};
        Event up0;
        Event up1;
        ok = cudaEventRecord(up0.event, stream) == cudaSuccess && frame.stage(params, stream, deviceScene) &&
             cudaEventRecord(up1.event, stream) == cudaSuccess && cudaStreamSynchronize(stream) == cudaSuccess;
        result.upload_ms = ok ? elapsedMs(up0, up1) : 0.f;

        const kernel::KernelLaunch launch = tiled ? rm::make_tiled_launch(params) : rm::make_launch(params);
        const rm::Params deviceParams =
            tiled ? rm::make_tiled_params(deviceScene, params.objects) : rm::make_params(deviceScene);
        const kernel::DeviceEntryFn entry = tiled ? &tiledEntry : &kernel::cuda::entry<rm::Kernel, rm::Params>;
        const rm::TiledKernel tiledBody{};
        const rm::Kernel body{};
        const void* bodyPtr = tiled ? static_cast<const void*>(&tiledBody) : static_cast<const void*>(&body);
        Event k0;
        Event k1;
        f32 total = 0.f;
        result.kernel_ms_min = 3.4e38f;
        for (u32 i = 0; ok && i < result.iterations; ++i) {
            ok = cudaEventRecord(k0.event, stream) == cudaSuccess && entry(launch, bodyPtr, &deviceParams, stream, false) &&
                 cudaEventRecord(k1.event, stream) == cudaSuccess && cudaEventSynchronize(k1.event) == cudaSuccess;
            const f32 ms = ok ? elapsedMs(k0, k1) : 0.f;
            total += ms;
            result.kernel_ms_min = std::min(result.kernel_ms_min, ms);
        }
        result.kernel_ms_avg = total / static_cast<f32>(result.iterations);

        Event d0;
        Event d1;
        ok = ok && cudaEventRecord(d0.event, stream) == cudaSuccess && frame.download(params, stream) &&
             cudaEventRecord(d1.event, stream) == cudaSuccess && cudaStreamSynchronize(stream) == cudaSuccess;
        result.download_ms = ok ? elapsedMs(d0, d1) : 0.f;
    }
    if (tiled) {
        describe(ray_march_tiled_kernel, kThreads, result);
    } else {
        describe(kernel::cuda::item_trampoline<rm::Kernel, rm::Params>, kThreads, result);
    }
    (void)cudaStreamDestroy(stream);
    if (ok) {
        timing = result;
    }
    return ok;
}

} // namespace fuse::compute
