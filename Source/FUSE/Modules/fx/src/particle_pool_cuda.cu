// CUDA backend of the AFX particle pool: the packed-SSBO item kernel is the single-source
// particle_pool_kernel::PackedKernel (fuse/fx/particle_pool_kernel.hpp) — the same integrate step
// ParticlePool::tick runs on the CPU — launched through the cuda_launch.cuh trampoline. This TU only
// keeps the packed mirror resident on the device and copies it back.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/fx/particle_pool_kernel.hpp>
#include <fuse/types.hpp>

#include <cuda_runtime.h>

using fuse::u32;
using fuse::u8;
using fuse::usize;

namespace {

struct DeviceSsboState {
    u8* devicePacked = nullptr;
    u32 deviceCapacityBytes = 0;
    u32 allocCount = 0;
    u32 reuseCount = 0;
    u32 integrateDispatchCount = 0;
    u32 residentFrameCount = 0;
    u32 hostToDeviceSkipCount = 0;
    bool deviceResident = false;
};

DeviceSsboState& deviceSsboState() {
    static DeviceSsboState state;
    return state;
}

bool ensureDeviceSsbo(u32 bytes, u8** outDevicePacked) {
    DeviceSsboState& state = deviceSsboState();
    if (state.devicePacked != nullptr && state.deviceCapacityBytes >= bytes) {
        ++state.reuseCount;
        *outDevicePacked = state.devicePacked;
        return true;
    }

    if (state.devicePacked != nullptr) {
        cudaFree(state.devicePacked);
        state.devicePacked = nullptr;
        state.deviceCapacityBytes = 0;
    }

    if (cudaMalloc(reinterpret_cast<void**>(&state.devicePacked), bytes) != cudaSuccess) {
        return false;
    }

    state.deviceCapacityBytes = bytes;
    ++state.allocCount;
    *outDevicePacked = state.devicePacked;
    return true;
}

} // namespace

extern "C" u32 fuse_fx_particle_pool_device_ssbo_alloc_count() {
    return deviceSsboState().allocCount;
}

extern "C" u32 fuse_fx_particle_pool_device_ssbo_reuse_count() {
    return deviceSsboState().reuseCount;
}

extern "C" u32 fuse_fx_particle_pool_device_ssbo_capacity_bytes() {
    return deviceSsboState().deviceCapacityBytes;
}

extern "C" u32 fuse_fx_particle_pool_cuda_integrate_dispatch_count() {
    return deviceSsboState().integrateDispatchCount;
}

extern "C" u32 fuse_fx_particle_pool_device_resident_frames() {
    return deviceSsboState().residentFrameCount;
}

extern "C" u32 fuse_fx_particle_pool_host_to_device_skip_count() {
    return deviceSsboState().hostToDeviceSkipCount;
}

/// Integrates `slotCount` packed slots in place (device-resident between frames unless `hostDirty`).
/// Returns 1 when the kernel ran and the packed mirror was read back, else 0.
extern "C" int fuse_fx_particle_pool_cuda_integrate(u8* packed, u32 slotCount, float dt, int hostDirty) {
    namespace ppk = fuse::fx::particle_pool_kernel;
    if (packed == nullptr || slotCount == 0u) {
        return 0;
    }

    u8* devicePacked = nullptr;
    const usize bytes = static_cast<usize>(slotCount) * ppk::kBytesPerSlot;
    if (!ensureDeviceSsbo(static_cast<u32>(bytes), &devicePacked)) {
        return 0;
    }

    DeviceSsboState& state = deviceSsboState();
    const bool needsHostUpload = hostDirty != 0 || !state.deviceResident;
    if (needsHostUpload) {
        if (cudaMemcpy(devicePacked, packed, bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
            return 0;
        }
        state.deviceResident = true;
    } else {
        ++state.hostToDeviceSkipCount;
    }

    ppk::PackedParams params{};
    params.packed = {devicePacked, static_cast<u32>(bytes)};
    params.dt = ppk::resolve_dt(dt);
    fuse::kernel::LaunchOptions options{};
    options.cuda = &fuse::kernel::cuda::entry<ppk::PackedKernel, ppk::PackedParams>;
    options.allow_fallback = false;
    options.synchronize = true;
    if (!fuse::kernel::launch(fuse::kernel::Backend::Cuda, ppk::packed_launch(slotCount), ppk::PackedKernel{}, params,
                              options)
             .ok ||
        cudaMemcpy(packed, devicePacked, bytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
        state.deviceResident = false;
        return 0;
    }
    ++state.integrateDispatchCount;
    ++state.residentFrameCount;
    return 1;
}
