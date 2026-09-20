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

    /// Vulkan timeline when backend + device + timeline feature are present. CUDA import is
    /// optional: `driverWired` is set only when `cudaImportExternalSemaphore` succeeds.
    static SharedTimeline create(void* vkDevice, void* vkPhysicalDevice = nullptr);
    void destroy(void* vkDevice);

    /// Vulkan timeline signal — real `vkSignalSemaphore` when `valid` and `vkSemaphore` are set.
    bool signalVulkan(void* vkDevice, u64 newValue) const;
    /// CUDA timeline wait — real `cudaWaitExternalSemaphoresAsync` when driver-wired.
    bool waitCuda(void* cudaStream, u64 waitValue) const;
    /// CUDA timeline signal — real `cudaSignalExternalSemaphoresAsync` when driver-wired.
    bool signalCuda(void* cudaStream, u64 newValue) const;
    /// Vulkan timeline wait — real `vkWaitSemaphores` when `valid` and `vkSemaphore` are set.
    bool waitVulkan(void* vkDevice, u64 waitValue) const;
};

/// Cross-lane progress for render thread ↔ CUDA job lane (WP-06h).
struct FrameSyncProgress {
    u64 frameIndex = 0;
    u64 vkToCudaValue = 0;
    u64 cudaToVkValue = 0;
    u32 renderLaneSignals = 0;
    u32 jobLaneWaits = 0;
    u32 jobLaneSignals = 0;
    u32 renderLaneWaits = 0;
};

struct FrameSyncPair {
    SharedTimeline vkToCuda{};
    SharedTimeline cudaToVk{};
    FrameSyncProgress progress{};

    static FrameSyncPair create(void* vkDevice, void* vkPhysicalDevice = nullptr);
    void destroy(void* vkDevice);

    bool valid() const { return vkToCuda.valid || cudaToVk.valid; }
    bool driverWired() const { return vkToCuda.driverWired || cudaToVk.driverWired; }
    const FrameSyncProgress& lastProgress() const { return progress; }

    /// Render thread: Vulkan-signal frame `frameIndex` when `vkToCuda` is valid (no CUDA import).
    bool signalRenderLane(void* vkDevice, u64 frameIndex);
    /// Job lane: wait for render signal, then run CUDA work for `frameIndex`.
    bool waitJobLaneOnRenderSignal(void* cudaStream, u64 frameIndex);
    /// Job lane: signal CUDA completion for `frameIndex`.
    bool signalJobLaneComplete(void* cudaStream, u64 frameIndex);
    /// Render thread: wait for CUDA before composite sampling. `waitVulkan` only when `driverWired()`.
    bool waitRenderLane(void* vkDevice, u64 frameIndex);
    /// Convenience — job lane wait + signal in one call.
    bool advanceJobLane(void* cudaStream, u64 frameIndex);
};

/// Multi-frame bookkeeping stress (WP-06i) — always updates `progress` counters. CUDA lane
/// ops succeed only when `driverWired()`; Vulkan render-lane signal does not require CUDA.
struct FrameSyncLoadStressResult {
    u32 framesAttempted = 0;
    u32 framesCompleted = 0;
    FrameSyncProgress finalProgress{};
};

[[nodiscard]] FrameSyncLoadStressResult stressFrameSyncUnderLoad(FrameSyncPair& pair, void* vkDevice,
                                                                 void* cudaStream, u32 frameCount);

/// Multi-cycle create → stress → destroy for timeline bookkeeping (WP-06j).
struct FrameSyncTeardownStressResult {
    u32 teardownCycles = 0;
    u32 framesPerCycle = 0;
    u32 totalFramesCompleted = 0;
    FrameSyncProgress finalProgress{};
};

[[nodiscard]] FrameSyncTeardownStressResult stressFrameSyncTeardownCycle(void* vkDevice,
                                                                         void* vkPhysicalDevice,
                                                                         void* cudaStream,
                                                                         u32 cycles,
                                                                         u32 framesPerCycle);

} // namespace fuse::renderer::cuda
