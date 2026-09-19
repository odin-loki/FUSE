#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer::cuda {

/// Shared timeline semaphore pair — Vulkan signals, CUDA waits (and vice versa).
struct SharedTimeline {
    void* vkSemaphore = nullptr;
    void* cudaSemaphore = nullptr;
    u64 value = 0;
    bool valid = false;
    const char* message = nullptr;

    static SharedTimeline create(void* vkDevice);
    void destroy(void* vkDevice);

    /// Vulkan timeline signal stub — returns false until full B2.6 driver wiring.
    bool signalVulkan(void* vkDevice, u64 newValue) const;
    /// CUDA timeline wait stub — returns false until full B2.6 driver wiring.
    bool waitCuda(void* cudaStream, u64 waitValue) const;
};

struct FrameSyncPair {
    SharedTimeline vkToCuda{};
    SharedTimeline cudaToVk{};
};

} // namespace fuse::renderer::cuda
