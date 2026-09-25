// CUDA backend of the particle simulation: the __global__ trampolines from cuda_launch.cuh run the same
// FUSE_HOST_DEVICE update + compaction bodies (fuse/vfx/particle_sim_kernel.hpp) the CPU backends run.
// This TU only stages the SoA columns, colliders and counters in device memory and reads them back.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/vfx/particle_sim_kernel.hpp>

#include <cuda_runtime.h>

namespace fuse::vfx {

namespace {

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

} // namespace

bool launchParticleSimCuda(const particle_sim_kernel::Params& params, u32 capacity, void* stream) {
    namespace psk = particle_sim_kernel;
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
    options.cuda = &kernel::cuda::entry<psk::UpdateKernel, psk::Params>;
    ok = kernel::launch(kernel::Backend::Cuda, psk::update_launch(capacity), psk::UpdateKernel{}, device, options).ok;
    options.cuda = &kernel::cuda::entry<psk::CompactKernel, psk::Params>;
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
