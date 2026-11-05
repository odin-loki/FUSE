// CUDA backend of the batched SVO ray cast: a persistent __global__ entry runs the same FUSE_HOST_DEVICE walk
// (fuse/scene/svo_ray_kernel.hpp: ray_begin + ray_step == ray_cast) the CPU backends run. This TU stages the
// linearised node / brick / payload arrays and the rays in device memory, and hosts the resident-buffer
// CUDA-event benchmark.
//
// Parity: the TU is compiled with -fmad=false (Scene/CMakeLists.txt), so nvcc never contracts the DDA's
// mul + add into an fma; with IEEE division / sqrt (nvcc defaults) the walk takes the same decisions as
// the host build.
//
// Speed (2026-11-05, RTX 3090, 1M incoherent rays over a depth-10 SVO, CUDA events on resident buffers):
// 40 ms -> ~7 ms. The walk is bound by L1 capacity, not by arithmetic: (1) the traversal stack's node path
// moved from per-thread local memory (144 B / thread interleaved in L1, evicting the tree) to a strided
// column of dynamic shared memory, brick_depth words per thread; (2) std::clamp / std::max returned
// references to temporaries that nvcc placed in local memory inside the loop (now by-value helpers: no
// local memory at all); (3) 128-thread blocks with at least 6 resident per SM (72 registers, no spills);
// (4) this TU is built without -rdc (fuse_svo.cmake: device-link register allocation cost 8 registers and
// 56 B of local memory); (5) the persistent loop below keeps every lane busy while ray lengths differ
// (SIMT efficiency of a one-ray-per-thread launch is ~34 % on that workload). Measured one at a time: (1)
// 41 -> 17 ms, (2) -> 12 ms, (3) -> 8-9 ms, (4) -> 7 ms; (5) alone was neutral, the walk being memory bound.

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
// 128-thread blocks, at least 6 resident per SM: the compiler keeps the walk within 72 registers without
// spilling (8 per SM forced 64 registers and ran ~35 % slower; fewer threads per SM also thrash L1 less).
constexpr u32 kMinBlocksPerSm = 6;

/// Traversal-stack node path in dynamic shared memory: entry `d` of thread `t` at word d * kThreads + t
/// (conflict-free when a warp reads the same depth). brick_depth words per thread.
struct SharedNodePath {
    u32* column = nullptr;
    __device__ u32& operator[](u32 depth) { return column[depth * kThreads]; }
};

/// Persistent walk (Aila & Laine style). Incoherent rays differ wildly in length (a ray that leaves the
/// side of the volume crosses hundreds of bricks, one that hits the ground a few), so a one-ray-per-thread
/// launch keeps a warp alive until its longest ray ends while the other lanes idle. Here a resident grid
/// loops over `ray_step` and every lane whose ray finished takes the next ray index from a global counter
/// (one warp-aggregated atomic per refill), so the lanes stay busy until the batch runs dry. Each ray
/// still runs ray_begin + ray_step == svo_kernel::ray_cast exactly: same hit records, bit for bit.
__global__ void __launch_bounds__(kThreads, kMinBlocksPerSm)
    svo_ray_cast_kernel(sk::Params params, u32* next) {
    extern __shared__ u32 sharedPaths[];
    const u32 count = params.rays.size;
    const u32 lane = threadIdx.x & 31u;
    sk::RayWalk walk;
    sk::TraversalStackT<SharedNodePath> stack;
    stack.node.column = sharedPaths + threadIdx.x;
    SvoRayHit out{};
    u32 ray = 0u;
    bool active = false;
    for (;;) {
        // Refill: every lane without a ray takes one; one atomic per warp.
        const unsigned live = __activemask();
        const unsigned need = __ballot_sync(live, !active);
        if (need != 0u) {
            const int leader = __ffs(static_cast<int>(need)) - 1;
            u32 base = 0u;
            if (static_cast<int>(lane) == leader) {
                base = atomicAdd(next, static_cast<u32>(__popc(need)));
            }
            base = __shfl_sync(live, base, leader);
            if (!active) {
                ray = base + static_cast<u32>(__popc(need & ((1u << lane) - 1u)));
                if (ray >= count) {
                    break; // batch exhausted: this lane is done
                }
                out = SvoRayHit{};
                active = sk::ray_begin(params.svo, params.rays[ray], walk, stack);
                if (!active) {
                    params.hits[ray] = out; // missed the grid: cleared record
                    continue;
                }
            }
        }
        const sk::RayStep r = sk::ray_step(params.svo, walk, stack, out);
        if (r != sk::RayStep::Continue) {
            params.hits[ray] = out; // a miss leaves `out` cleared
            active = false;
        }
    }
}

/// Dynamic shared memory of one block: the traversal-stack node paths (brick_depth words per thread).
usize path_shared_bytes(const sk::SvoView& svo) {
    return static_cast<usize>(std::max(svo.brick_depth, 1u)) * kThreads * sizeof(u32);
}

/// Resident blocks for the persistent grid (SMs x blocks per SM at `sharedBytes` per block).
u32 persistent_blocks(usize sharedBytes) {
    int device = 0;
    int sms = 0;
    int perSm = 0;
    if (cudaGetDevice(&device) != cudaSuccess ||
        cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, device) != cudaSuccess ||
        cudaOccupancyMaxActiveBlocksPerMultiprocessor(&perSm, svo_ray_cast_kernel, static_cast<int>(kThreads),
                                                      sharedBytes) != cudaSuccess ||
        sms <= 0 || perSm <= 0) {
        return 82u * kMinBlocksPerSm;
    }
    return static_cast<u32>(sms * perSm);
}

/// DeviceEntryFn for kernel::launch (stats / profiler scope) with the persistent kernel above. The ray
/// counter is stream-ordered scratch (cudaMallocAsync from the device pool), so concurrent launches on
/// different streams never share it.
bool svo_entry(const kernel::KernelLaunch& launch, const void* /*body*/, const void* params, void* stream,
               bool synchronize) {
    const sk::Params& p = *static_cast<const sk::Params*>(params);
    if (launch.grid.count() == 0u || p.rays.size == 0u) {
        return true;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    void* counter = nullptr;
    if (cudaMallocAsync(&counter, sizeof(u32), cudaStream) != cudaSuccess) {
        return false;
    }
    bool ok = cudaMemsetAsync(counter, 0, sizeof(u32), cudaStream) == cudaSuccess;
    if (ok) {
        const u32 needed = (p.rays.size + kThreads - 1u) / kThreads;
        const usize shared = path_shared_bytes(p.svo);
        const u32 blocks = std::min(persistent_blocks(shared), needed);
        svo_ray_cast_kernel<<<blocks, kThreads, shared, cudaStream>>>(p, static_cast<u32*>(counter));
        ok = cudaGetLastError() == cudaSuccess;
    }
    ok = cudaFreeAsync(counter, cudaStream) == cudaSuccess && ok;
    return ok && (!synchronize || cudaStreamSynchronize(cudaStream) == cudaSuccess);
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
    if (cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks, svo_ray_cast_kernel, static_cast<int>(kThreads),
                                                      path_shared_bytes(host.svo)) ==
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
