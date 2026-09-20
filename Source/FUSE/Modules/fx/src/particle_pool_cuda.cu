#include <fuse/types.hpp>

#include <cuda_runtime.h>

namespace {

__global__ void fuse_fx_integrate_particles_kernel(u8* packed, u32 activeCount, float dt) {
    const u32 slotIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (slotIndex >= activeCount) {
        return;
    }

    constexpr u32 kBytesPerSlot = 40u;
    u8* slot = packed + slotIndex * kBytesPerSlot;
    float* position = reinterpret_cast<float*>(slot);
    const float* velocity = reinterpret_cast<const float*>(slot + 12);
    float* lifetime = reinterpret_cast<float*>(slot + 24);
    float* age = reinterpret_cast<float*>(slot + 28);
    u32* alive = reinterpret_cast<u32*>(slot + 36);

    if (*alive == 0u) {
        return;
    }

    position[0] += velocity[0] * dt;
    position[1] += velocity[1] * dt;
    position[2] += velocity[2] * dt;

    float* vel = reinterpret_cast<float*>(slot + 12);
    const float damping = 0.98f;
    vel[0] *= damping;
    vel[1] *= damping;
    vel[2] *= damping;

    *age += dt;
    if (*age >= *lifetime) {
        *alive = 0u;
    }
}

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

extern "C" void fuse_fx_particle_pool_cuda_stub(const u8* packed, u32 activeCount, float dt, int hostDirty) {
    if (packed == nullptr || activeCount == 0u) {
        return;
    }

    u8* devicePacked = nullptr;
    const usize bytes = static_cast<usize>(activeCount) * 40u;
    if (!ensureDeviceSsbo(static_cast<u32>(bytes), &devicePacked)) {
        return;
    }

    DeviceSsboState& state = deviceSsboState();
    const bool needsHostUpload = hostDirty != 0 || !state.deviceResident;
    if (needsHostUpload) {
        if (cudaMemcpy(devicePacked, packed, bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
            return;
        }
        state.deviceResident = true;
    } else {
        ++state.hostToDeviceSkipCount;
    }

    const int blockSize = 64;
    const int gridSize = static_cast<int>((activeCount + blockSize - 1u) / blockSize);
    fuse_fx_integrate_particles_kernel<<<gridSize, blockSize>>>(devicePacked, activeCount, dt);
    cudaDeviceSynchronize();
    cudaMemcpy(const_cast<u8*>(packed), devicePacked, bytes, cudaMemcpyDeviceToHost);
    ++state.integrateDispatchCount;
    ++state.residentFrameCount;
}
