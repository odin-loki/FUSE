#pragma once

#include <fuse/renderer/rg/graph.hpp>
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
    u64 acquiredImageCount = 0; ///< vkAcquireNextImageKHR calls that returned a swapchain image
    u32 fenceWaitCount = 0;
    u32 swapchainRecreateCount = 0;
    u32 resizeCoalesceCount = 0;
    u32 resizeDuplicateCount = 0;
    u32 resizeRejectedCount = 0;
    u32 resizeDeferredCount = 0;
    u32 resizeNoOpCount = 0;
    u32 emptyAcquireCount = 0;
    u32 emptyPresentCount = 0;
    u32 queueSubmitCount = 0;
    u32 presentSkippedNoWsiCount = 0;
    u32 realPresentCallCount = 0;
    u32 qtRealPresentCallCount = 0;
    bool desktopPresentEnabled = false;
    bool qtPresentEnabled = false;
    bool desktopPresentRuntimeReady = false;
    bool qtPresentRuntimeReady = false;
    u32 fenceWaitSkippedCount = 0;
    u32 lastPendingFenceCount = 0;
    bool lastQueueSubmitOk = false;
    std::string message;
};

/// True when acquire/present is in progress and resize must be deferred.
inline bool isPresentCycleActive(PresentPathState state) {
    return state == PresentPathState::FenceWaited || state == PresentPathState::ImageAcquired ||
           state == PresentPathState::ReadyToPresent;
}

/// True when a resize request must wait until the present cycle completes.
inline bool shouldDeferResizeDuringPresentCycle(PresentPathState state) {
    return isPresentCycleActive(state);
}

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

    /// Present the acquired image; headless succeeds without `vkQueuePresentKHR`.
    bool presentImage();

    /// Mirror queue-submit diagnostics from the paired RHI context (optional).
    void noteQueueSubmit(bool ok, bool submitted, bool headless);

    /// Convenience: wait → acquire for frame N (render record happens between acquire and present).
    bool beginFrame(u32 frameIndex);
    bool endFrame();

    void setVsyncMode(VsyncMode mode);
    VsyncMode vsyncMode() const { return m_status.vsyncMode; }

    void requestResize(u32 width, u32 height);
    bool hasPendingResize() const { return m_status.resizePending; }
    u32 pendingResizeWidth() const { return m_status.pendingResizeWidth; }
    u32 pendingResizeHeight() const { return m_status.pendingResizeHeight; }

    /// Apply a queued resize immediately (fence-waits first). No-op when nothing is pending.
    bool recreateSwapchain();

private:
    explicit PresentPath(VulkanBootstrap& bootstrap, PresentPathDesc desc);

    void refreshDimensions();
    bool processPendingResize();

    VulkanBootstrap& m_bootstrap;
    PresentPathDesc m_desc;
    PresentPathStatus m_status;
    /// Frame-ring slot whose `imageAvailable` the acquire signalled. The render submit waits on it
    /// and signals the same slot's `renderFinished`; present must wait on that one even though the
    /// RHI has already advanced the ring (`FrameManager::endFrame`) by the time it presents.
    u32 m_acquireSlot = 0;
    /// `noteQueueSubmit` saw a submit that signalled `renderFinished` for the acquired image; a
    /// real present without it would wait on a semaphore nobody signals.
    bool m_renderFinishedSignalled = false;
};

// --- render-graph present hand-off (E02 SceneRenderer, RE-FI-1) -----------------------------------------------------
// The frame's final image (FrameGraphOutputs::output, RGBA16F) reaches a swapchain image or a headless target through
// one "present.blit" graph pass (vkCmdBlitImage: format conversion + optional scaling; the graph declares
// TransferSrc / TransferDst, no hand barrier). A swapchain target then gets a "present.handoff" pass (Access::Present:
// the graph leaves it in PRESENT_SRC_KHR for vkQueuePresentKHR). With frame generation, the caller blits
// presentInterpolated to one acquired image and presentReal to the next (see SceneRenderer::addPresent).

/// A swapchain image or a persistent headless target to import into a frame's graph.
struct PresentTargetDesc {
    void* image = nullptr; ///< VkImage
    void* view = nullptr;  ///< optional VkImageView
    u32 format = 0;        ///< VkFormat (needs BLIT_DST)
    u32 width = 0;
    u32 height = 0;
    /// Swapchain image: imported with UNDEFINED (contents discarded) and handed off in PRESENT_SRC_KHR. Headless
    /// target: layout / queue tracked through the trackers below (null: UNDEFINED every frame).
    bool swapchain = false;
    u32* layoutTracker = nullptr;
    u8* queueTracker = nullptr;
    const char* name = "present.target";
};

/// Imports `target` into `graph` (invalid ref when the description is incomplete).
rg::TextureRef importPresentTarget(rg::Graph& graph, const PresentTargetDesc& target);

/// One blit of a frame image into a present target. The record must outlive the graph's execution (the pass callback
/// reads it).
struct PresentBlit {
    rg::TextureRef source;
    u32 sourceWidth = 0;  ///< region [0, w) x [0, h) of the source
    u32 sourceHeight = 0;
    rg::TextureRef target;
    u32 targetWidth = 0;
    u32 targetHeight = 0;
    /// Linear filtering when the extents differ (same extent: nearest, an exact per-texel conversion).
    bool linearFilter = true;
    /// Adds the "present.handoff" pass (Access::Present) after the blit: set for a swapchain image.
    bool present = false;
};

/// Records "present.blit" (+ "present.handoff"). False for invalid refs / extents.
bool addPresentBlit(rg::Graph& graph, PresentBlit& blit);

} // namespace fuse::renderer
