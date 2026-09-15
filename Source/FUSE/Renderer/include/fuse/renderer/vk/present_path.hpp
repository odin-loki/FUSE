#pragma once

#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

class VulkanBootstrap;

/// CPU-side present lifecycle for B2.2 headless CI and future WSI wiring.
enum class PresentPathState : u8 {
    Idle = 0,
    FenceWaited = 1,
    ImageAcquired = 2,
    ReadyToPresent = 3,
    Presented = 4,
    ResizePending = 5,
};

struct PresentPathDesc {
    VsyncMode vsyncMode = VsyncMode::Fifo;
};

struct PresentPathStatus {
    PresentPathState state = PresentPathState::Idle;
    VsyncMode vsyncMode = VsyncMode::Fifo;
    bool headless = true;
    bool fenceWaited = false;
    bool acquireAttempted = false;
    bool presentAttempted = false;
    u32 acquiredImageIndex = UINT32_MAX;
    u32 width = 0;
    u32 height = 0;
    u32 pendingResizeWidth = 0;
    u32 pendingResizeHeight = 0;
    bool resizePending = false;
    u64 presentedFrames = 0;
    u32 fenceWaitCount = 0;
    u32 swapchainRecreateCount = 0;
    u32 resizeCoalesceCount = 0;
    u32 resizeNoOpCount = 0;
    u32 resizeRejectedCount = 0;
    u32 emptyAcquireCount = 0;
    u32 emptyPresentCount = 0;
    u32 fenceWaitSkippedCount = 0;
    u32 lastPendingFenceCount = 0;
    std::string message;
};

/// Acquire / present / fence-wait stubs over `VulkanBootstrap` swapchain + frame ring.
class PresentPath {
public:
    static std::unique_ptr<PresentPath> create(VulkanBootstrap& bootstrap, const PresentPathDesc& desc = {});

    const PresentPathStatus& status() const { return m_status; }
    PresentPathState state() const { return m_status.state; }

    /// Pre-flight: fence waited and swapchain can acquire (or headless stub path).
    bool canAcquire() const;

    /// Pre-flight: image acquired (or headless stub) and ready to present.
    bool canPresent() const;

    /// Wait on the current slot in-flight fence before acquire (stub-safe when headless).
    bool waitInFlightFence();

    /// Acquire swapchain image; headless returns UINT32_MAX but advances the state machine.
    u32 acquireImage();

    /// Mark render record complete — transitions ImageAcquired → ReadyToPresent.
    bool markReadyToPresent();

    /// Present the acquired image; headless succeeds without queue submit.
    bool presentImage();

    /// Convenience: wait → acquire for frame N (render record happens between acquire and present).
    bool beginFrame(u32 frameIndex);
    bool endFrame();

    void setVsyncMode(VsyncMode mode);
    VsyncMode vsyncMode() const { return m_status.vsyncMode; }

    void requestResize(u32 width, u32 height);
    bool hasPendingResize() const { return m_status.resizePending; }

    /// Drop a queued resize without recreating (e.g. surface lost).
    void cancelPendingResize();

    /// Apply a queued resize immediately (fence-waits first). No-op when nothing is pending.
    bool recreateSwapchain();

private:
    explicit PresentPath(VulkanBootstrap& bootstrap, PresentPathDesc desc);

    void refreshDimensions();
    bool processPendingResize();

    VulkanBootstrap& m_bootstrap;
    PresentPathDesc m_desc;
    PresentPathStatus m_status;
};

} // namespace fuse::renderer
