#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer::cuda {

/// Shared timeline semaphore pair — Vulkan signals, CUDA waits (and vice versa).
struct SharedTimeline {
    void* vkSemaphore = nullptr;
    void* cudaSemaphore = nullptr;
    u64 value = 0;
    bool valid = false;
    bool driverWired = false;
    const char* message = nullptr;

    static SharedTimeline create(void* vkDevice, void* vkPhysicalDevice = nullptr);
    void destroy(void* vkDevice);

    /// Vulkan timeline signal — real `vkSignalSemaphore` when driver-wired, otherwise false.
    bool signalVulkan(void* vkDevice, u64 newValue) const;
    /// CUDA timeline wait — real `cudaWaitExternalSemaphoresAsync` when driver-wired.
    bool waitCuda(void* cudaStream, u64 waitValue) const;
};

struct FrameSyncPair {
    SharedTimeline vkToCuda{};
    SharedTimeline cudaToVk{};

    static FrameSyncPair create(void* vkDevice, void* vkPhysicalDevice = nullptr);
    void destroy(void* vkDevice);

    bool valid() const { return vkToCuda.valid || cudaToVk.valid; }
    bool driverWired() const { return vkToCuda.driverWired || cudaToVk.driverWired; }
};

} // namespace fuse::renderer::cuda
