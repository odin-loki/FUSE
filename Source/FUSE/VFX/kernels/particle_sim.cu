// CUDA backend of the particle simulation: __global__ entries run the same FUSE_HOST_DEVICE update +
// compaction bodies (fuse/vfx/particle_sim_kernel.hpp) the CPU backends run. This TU stages the SoA
// columns, colliders and counters in device memory and reads them back.
//
// Occupancy (B7 row "CUDA particle kernel achieves > 70% occupancy"): the generic trampolines left the
// update at 42 registers -> 5 x 256-thread blocks per SM (83% theoretical) and, at 262144 slots, 1024
// short-lived blocks = 2.5 waves on an RTX 3090, so the half-empty last wave and block turnover held
// achieved occupancy to 69%. The entries below
//   - cap registers with __launch_bounds__(256, 6): <= 40 registers -> 6 blocks = 48 warps (100%),
//   - launch a persistent grid (SMs x resident blocks, from the occupancy calculator) whose blocks loop
//     over the 256-slot workgroups (grid stride), so every SM stays full until the work runs out.
// Each workgroup still sees exactly the LaunchIndex the CPU backends give it (same group id, same local
// ids), so the results are bit-identical to the generic trampolines.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/vfx/particle_sim_kernel.hpp>
#include <fuse/vfx/particle_system.hpp>

#include <cuda_runtime.h>

#include <algorithm>

namespace fuse::vfx {

namespace {

namespace psk = particle_sim_kernel;

constexpr u32 kThreads = psk::kGroupSize;
constexpr u32 kMinBlocksPerSm = 6; // 6 x 256 threads = 48 warps: all of an sm_86 SM

/// Item body over grid-strided 256-slot workgroups.
__global__ void __launch_bounds__(kThreads, kMinBlocksPerSm)
    particle_update_kernel(psk::Params params, kernel::Dim3 grid, kernel::Dim3 workgroup, u32 groups) {
    for (u32 group = blockIdx.x; group < groups; group += gridDim.x) {
        const kernel::LaunchIndex idx =
            kernel::make_index(grid, workgroup, kernel::Dim3{group, 0u, 0u}, kernel::Dim3{threadIdx.x, 0u, 0u});
        if (idx.active) {
            psk::UpdateKernel{}(idx, params);
        }
    }
}

/// Workgroup body (count -> scan -> write phases) over grid-strided workgroups. The trailing barrier of
/// each workgroup's last phase keeps the next workgroup's phase 0 from overwriting live scratch.
__global__ void __launch_bounds__(kThreads, kMinBlocksPerSm)
    particle_compact_kernel(psk::Params params, kernel::Dim3 grid, kernel::Dim3 workgroup, u32 groups) {
    using Scratch = psk::CompactKernel::Scratch;
    __shared__ __align__(16) unsigned char raw[sizeof(Scratch) * psk::CompactKernel::kScratchCount];
    kernel::WorkgroupContext<Scratch> ctx{};
    ctx.scratch = reinterpret_cast<Scratch*>(raw);
    ctx.scratch_count = psk::CompactKernel::kScratchCount;
    ctx.phase_count = psk::CompactKernel::kPhases;
    for (u32 group = blockIdx.x; group < groups; group += gridDim.x) {
        const kernel::LaunchIndex idx =
            kernel::make_index(grid, workgroup, kernel::Dim3{group, 0u, 0u}, kernel::Dim3{threadIdx.x, 0u, 0u});
        for (u32 phase = 0; phase < psk::CompactKernel::kPhases; ++phase) {
            ctx.phase = phase;
            psk::CompactKernel{}(idx, ctx, params);
            __syncthreads();
        }
    }
}

/// Blocks of `kernelFn` resident per SM x SM count (0 when the runtime cannot say).
template <typename KernelFn>
u32 residentBlocks(KernelFn kernelFn) {
    int device = 0;
    int sms = 0;
    int perSm = 0;
    if (cudaGetDevice(&device) != cudaSuccess ||
        cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, device) != cudaSuccess ||
        cudaOccupancyMaxActiveBlocksPerMultiprocessor(&perSm, kernelFn, static_cast<int>(kThreads), 0) != cudaSuccess) {
        return 0u;
    }
    return static_cast<u32>(std::max(sms, 0) * std::max(perSm, 0));
}

template <typename KernelFn>
bool launchPersistent(KernelFn kernelFn, u32 resident, const kernel::KernelLaunch& launch, const psk::Params& params,
                      void* stream, bool synchronize) {
    const u32 groups = kernel::group_count(launch.grid, launch.workgroup).x;
    if (groups == 0u) {
        return true;
    }
    const u32 blocks = resident == 0u ? groups : std::min(groups, resident);
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    kernelFn<<<blocks, kThreads, 0, cudaStream>>>(params, launch.grid, launch.workgroup, groups);
    if (cudaGetLastError() != cudaSuccess) {
        return false;
    }
    return !synchronize || cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

bool updateEntry(const kernel::KernelLaunch& launch, const void* /*body*/, const void* params, void* stream,
                 bool synchronize) {
    static const u32 resident = residentBlocks(particle_update_kernel); // once per process
    return launchPersistent(particle_update_kernel, resident, launch, *static_cast<const psk::Params*>(params), stream,
                            synchronize);
}

bool compactEntry(const kernel::KernelLaunch& launch, const void* /*body*/, const void* params, void* stream,
                  bool synchronize) {
    static const u32 resident = residentBlocks(particle_compact_kernel);
    return launchPersistent(particle_compact_kernel, resident, launch, *static_cast<const psk::Params*>(params),
                            stream, synchronize);
}

template <typename T>
bool stage(kernel::cuda::DeviceBuffer<T>& buffer, kernel::Span<T>& span, cudaStream_t stream) {
    if (!buffer.allocate(span.size) || !buffer.upload(span.data, span.size, stream)) {
        return false;
    }
    span.data = buffer.data();
    return true;
}

template <typename T>
bool stage(kernel::cuda::DeviceBuffer<T>& buffer, kernel::Span<const T>& span, cudaStream_t stream) {
    if (!buffer.allocate(span.size) || !buffer.upload(span.data, span.size, stream)) {
        return false;
    }
    span.data = buffer.data();
    return true;
}

template <typename KernelFn>
void describe(KernelFn kernelFn, ParticleKernelOccupancy::Entry& out) {
    cudaFuncAttributes attributes{};
    if (cudaFuncGetAttributes(&attributes, kernelFn) == cudaSuccess) {
        out.registers_per_thread = attributes.numRegs;
        out.local_bytes_per_thread = static_cast<s32>(attributes.localSizeBytes);
        out.shared_bytes_per_block = static_cast<s32>(attributes.sharedSizeBytes);
    }
    int device = 0;
    int perSm = 0;
    cudaDeviceProp props{};
    if (cudaOccupancyMaxActiveBlocksPerMultiprocessor(&perSm, kernelFn, static_cast<int>(kThreads), 0) == cudaSuccess &&
        cudaGetDevice(&device) == cudaSuccess && cudaGetDeviceProperties(&props, device) == cudaSuccess &&
        props.maxThreadsPerMultiProcessor > 0) {
        out.blocks_per_sm = perSm;
        out.persistent_blocks = perSm * props.multiProcessorCount;
        out.theoretical_occupancy =
            static_cast<f32>(perSm * static_cast<int>(kThreads)) / static_cast<f32>(props.maxThreadsPerMultiProcessor);
    }
}

} // namespace

bool particle_cuda_occupancy(ParticleKernelOccupancy& out) {
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        return false;
    }
    ParticleKernelOccupancy result{};
    result.threads_per_block = kThreads;
    describe(particle_update_kernel, result.update);
    describe(particle_compact_kernel, result.compact);
    out = result;
    return true;
}

bool launchParticleSimCuda(const particle_sim_kernel::Params& params, u32 capacity, void* stream) {
    if (capacity == 0u || params.alive_flags.size != capacity || params.dead_slots.size < capacity ||
        params.counters.size < psk::kCounterCount || params.group_dead.size < psk::group_count(capacity)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);

    psk::Params device = params;
    kernel::cuda::DeviceBuffer<math::Vec3> positions, velocities, colors;
    kernel::cuda::DeviceBuffer<f32> ages, lifetimes, sizes, alphas;
    kernel::cuda::DeviceBuffer<u32> aliveFlags, counters, groupDead, deadSlots;
    kernel::cuda::DeviceBuffer<ParticleCollider> colliders;
    bool ok = stage(positions, device.positions, cudaStream) && stage(velocities, device.velocities, cudaStream) &&
              stage(ages, device.ages, cudaStream) && stage(lifetimes, device.lifetimes, cudaStream) &&
              stage(sizes, device.sizes, cudaStream) && stage(colors, device.colors, cudaStream) &&
              stage(alphas, device.alphas, cudaStream) && stage(aliveFlags, device.alive_flags, cudaStream) &&
              stage(colliders, device.colliders, cudaStream) && stage(counters, device.counters, cudaStream) &&
              stage(groupDead, device.group_dead, cudaStream) && deadSlots.allocate(capacity);
    if (!ok) {
        return false;
    }
    device.dead_slots = {deadSlots.data(), capacity};

    kernel::LaunchOptions options{};
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = false; // same stream: the compaction is ordered after the update
    options.cuda = &updateEntry;
    ok = kernel::launch(kernel::Backend::Cuda, psk::update_launch(capacity), psk::UpdateKernel{}, device, options).ok;
    options.cuda = &compactEntry;
    options.synchronize = true;
    ok = ok &&
         kernel::launch(kernel::Backend::Cuda, psk::compact_launch(capacity), psk::CompactKernel{}, device, options).ok;

    ok = ok && counters.download(params.counters.data, params.counters.size, cudaStream) &&
         cudaStreamSynchronize(cudaStream) == cudaSuccess;
    const u32 culled = ok ? params.counters[psk::kCounterCulled] : 0u;
    ok = ok && positions.download(params.positions.data, capacity, cudaStream) &&
         velocities.download(params.velocities.data, capacity, cudaStream) &&
         ages.download(params.ages.data, capacity, cudaStream) &&
         sizes.download(params.sizes.data, capacity, cudaStream) &&
         colors.download(params.colors.data, capacity, cudaStream) &&
         alphas.download(params.alphas.data, capacity, cudaStream) &&
         aliveFlags.download(params.alive_flags.data, capacity, cudaStream) &&
         deadSlots.download(params.dead_slots.data, culled, cudaStream);
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

} // namespace fuse::vfx
