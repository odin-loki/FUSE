#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer::cuda {

/// Shared timeline semaphore pair — Vulkan signals, CUDA waits (and vice versa).
struct SharedTimeline {
    void* vkSemaphore = nullptr;
    void* cudaSemaphore = nullptr;
    u64 value = 0;
    bool valid = false;

    static SharedTimeline create(void* vkDevice);
    void destroy(void* vkDevice);
};

struct FrameSyncPair {
    SharedTimeline vkToCuda{};
    SharedTimeline cudaToVk{};
};

} // namespace fuse::renderer::cuda
