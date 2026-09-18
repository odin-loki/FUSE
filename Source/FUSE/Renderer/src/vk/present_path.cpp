#include <fuse/renderer/vk/present_path.hpp>

#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/fence_wait.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

namespace fuse::renderer {

ResizeRequestOutcome classifyResizeRequest(PresentPathState state,
                                           bool resizePending,
                                           u32 pendingWidth,
                                           u32 pendingHeight,
                                           u32 requestedWidth,
                                           u32 requestedHeight) {
    if (!isValidSwapchainExtent(requestedWidth, requestedHeight)) {
        return ResizeRequestOutcome::RejectedInvalidExtent;
    }
    if (resizePending &&
        isDuplicatePendingResizeExtent(pendingWidth, pendingHeight, requestedWidth, requestedHeight)) {
        return ResizeRequestOutcome::DuplicateIgnored;
    }
    if (isResizeCoalesceRequest(resizePending, pendingWidth, pendingHeight, requestedWidth, requestedHeight)) {
        return ResizeRequestOutcome::Coalesced;
    }
    if (shouldDeferResizeDuringPresentCycle(state)) {
        return ResizeRequestOutcome::DeferredDuringPresent;
    }
    return ResizeRequestOutcome::Queued;
}

PresentPath::PresentPath(VulkanBootstrap& bootstrap, PresentPathDesc desc)
    : m_bootstrap(bootstrap), m_desc(desc) {
    m_status.vsyncMode = desc.vsyncMode;
    refreshDimensions();
}

std::unique_ptr<PresentPath> PresentPath::create(VulkanBootstrap& bootstrap, const PresentPathDesc& desc) {
    return std::unique_ptr<PresentPath>(new PresentPath(bootstrap, desc));
}

void PresentPath::refreshDimensions() {
    const VulkanSwapchain* swapchain = m_bootstrap.swapchain();
    if (swapchain != nullptr) {
        m_status.width = swapchain->info().width;
        m_status.height = swapchain->info().height;
        m_status.headless = swapchain->isHeadless();
        return;
    }

    m_status.headless = true;
    m_status.width = 0;
    m_status.height = 0;
}

bool PresentPath::canAcquire() const {
    if (m_status.state != PresentPathState::FenceWaited) {
        return false;
    }

    const VulkanSwapchain* swapchain = m_bootstrap.swapchain();
    if (swapchain == nullptr) {
        return m_status.headless;
    }
    return m_status.headless || isSwapchainPresentable(*swapchain);
}

bool PresentPath::canPresent() const {
    if (m_status.state != PresentPathState::ImageAcquired &&
        m_status.state != PresentPathState::ReadyToPresent) {
        return false;
    }

    const VulkanSwapchain* swapchain = m_bootstrap.swapchain();
    if (swapchain == nullptr) {
        return m_status.headless;
    }
    if (isEmptyAcquireResult(m_status.acquiredImageIndex)) {
        return m_status.headless || isSwapchainEmpty(*swapchain);
    }
    return isSwapchainPresentable(*swapchain);
}

bool PresentPath::processPendingResize() {
    if (!m_status.resizePending) {
        return true;
    }

    m_status.lastResizeOutcome =
        classifyResizeApplyOutcome(m_status.width, m_status.height, m_status.pendingResizeWidth,
                                   m_status.pendingResizeHeight);
    if (m_status.lastResizeOutcome == ResizeRequestOutcome::NoOpMatchesCurrent) {
        m_status.resizePending = false;
        ++m_status.resizeNoOpCount;
        m_status.message = "Resize no-op — pending extent matches current swapchain";
        return true;
    }

    FrameManager* frameManager = m_bootstrap.frameManager();
    if (frameManager != nullptr && frameManager->isReady()) {
        const bool waitAllSlots = !m_status.headless;
        if (!waitInFlightFencesBeforeRecreate(*frameManager, waitAllSlots)) {
            m_status.message = "In-flight fence wait failed before swapchain recreate";
            return false;
        }
    }

    VulkanDevice* device = m_bootstrap.device();
    VulkanSwapchain* swapchain = m_bootstrap.swapchain();
    if (device == nullptr || !device->isValid() || swapchain == nullptr) {
        m_status.width = m_status.pendingResizeWidth;
        m_status.height = m_status.pendingResizeHeight;
        m_status.resizePending = false;
        m_status.state = PresentPathState::Idle;
        ++m_status.swapchainRecreateCount;
        m_status.message = "Headless resize recorded (no VkSwapchainKHR)";
        return true;
    }

    if (!swapchain->rebuild(*device, m_status.pendingResizeWidth, m_status.pendingResizeHeight)) {
        m_status.message = "Swapchain rebuild failed during resize recreate";
        return false;
    }

    m_status.width = swapchain->info().width;
    m_status.height = swapchain->info().height;
    m_status.headless = swapchain->isHeadless();
    m_status.resizePending = false;
    m_status.state = PresentPathState::Idle;
    ++m_status.swapchainRecreateCount;
    m_status.message = swapchain->info().message;
    return true;
}

bool PresentPath::waitInFlightFence() {
    if (m_status.state != PresentPathState::Idle && m_status.state != PresentPathState::Presented &&
        m_status.state != PresentPathState::ResizePending) {
        m_status.message = "waitInFlightFence called out of order";
        return false;
    }

    if (!processPendingResize()) {
        return false;
    }

    FrameManager* frameManager = m_bootstrap.frameManager();
    if (frameManager != nullptr && frameManager->isReady()) {
        m_status.lastPendingFenceCount = countPendingInFlightFences(*frameManager);
        if (m_status.lastPendingFenceCount == 0) {
            ++m_status.fenceWaitSkippedCount;
        }
        if (!waitInFlightFencesBeforeAcquire(*frameManager)) {
            m_status.message = "In-flight fence wait failed";
            return false;
        }
    } else {
        m_status.lastPendingFenceCount = 0;
    }

    ++m_status.fenceWaitCount;
    m_status.fenceWaited = true;
    m_status.state = PresentPathState::FenceWaited;
    m_status.message = m_status.headless ? "Headless fence wait stub (slot bookkeeping)"
                                         : "In-flight fence waited before acquire";
    return true;
}

u32 PresentPath::acquireImage() {
    if (m_status.state != PresentPathState::FenceWaited) {
        m_status.message = "acquireImage requires prior waitInFlightFence";
        return UINT32_MAX;
    }

    FrameManager* frameManager = m_bootstrap.frameManager();
    VulkanSwapchain* swapchain = m_bootstrap.swapchain();

    m_status.lastEmptyAcquireReason = classifyEmptyAcquireSkip(swapchain);

    u32 imageIndex = UINT32_MAX;
    if (m_status.lastEmptyAcquireReason == EmptyAcquireSkipReason::None && frameManager != nullptr &&
        frameManager->isReady()) {
        const FrameSyncData& slot = frameManager->current();
        imageIndex = swapchain->acquireNextImage(slot.imageAvailable);
    }

    m_status.acquireAttempted = true;
    m_status.acquiredImageIndex = imageIndex;
    m_status.state = PresentPathState::ImageAcquired;
    if (isEmptyAcquireResult(imageIndex)) {
        ++m_status.emptyAcquireCount;
        m_status.message = m_status.headless ? "Headless acquire stub (no swapchain image)"
                                             : "Swapchain acquire returned no image";
    } else {
        m_status.message = "Swapchain image acquired";
    }
    return imageIndex;
}

bool PresentPath::markReadyToPresent() {
    if (m_status.state != PresentPathState::ImageAcquired) {
        m_status.message = "markReadyToPresent requires acquired image";
        return false;
    }

    m_status.state = PresentPathState::ReadyToPresent;
    m_status.message = "Render record complete — ready to present";
    return true;
}

bool PresentPath::presentImage() {
    if (m_status.state != PresentPathState::ImageAcquired &&
        m_status.state != PresentPathState::ReadyToPresent) {
        m_status.message = "presentImage requires acquired image";
        return false;
    }

    FrameManager* frameManager = m_bootstrap.frameManager();
    VulkanSwapchain* swapchain = m_bootstrap.swapchain();

    m_status.lastEmptyPresentReason =
        classifyEmptyPresentSkip(swapchain, m_status.acquiredImageIndex, frameManager);

    bool presented = false;
    if (m_status.lastEmptyPresentReason != EmptyPresentSkipReason::None) {
        presented = true;
        ++m_status.emptyPresentCount;
    } else {
        const FrameSyncData& slot = frameManager->current();
        presented = swapchain->present(slot.renderFinished, m_status.acquiredImageIndex);
    }

    m_status.presentAttempted = true;
    if (!presented) {
        m_status.message = "Swapchain present failed";
        return false;
    }

    ++m_status.presentedFrames;
    m_status.acquiredImageIndex = UINT32_MAX;
    m_status.fenceWaited = false;
    m_status.acquireAttempted = false;
    m_status.presentAttempted = false;
    m_status.state = PresentPathState::Presented;
    m_status.message = m_status.headless ? "Headless present stub (no queue submit)"
                                         : "Swapchain image presented";
    return true;
}

bool PresentPath::beginFrame(u32 frameIndex) {
    FrameManager* frameManager = m_bootstrap.frameManager();
    if (frameManager != nullptr && frameManager->isReady()) {
        frameManager->signalTickComplete();
        frameManager->beginFrame(frameIndex);
    }

    if (!waitInFlightFence()) {
        return false;
    }

    acquireImage();
    markReadyToPresent();
    return true;
}

bool PresentPath::endFrame() {
    if (!presentImage()) {
        return false;
    }

    FrameManager* frameManager = m_bootstrap.frameManager();
    if (frameManager != nullptr && frameManager->isReady()) {
        frameManager->endFrame();
    }

    m_status.state = PresentPathState::Idle;
    return true;
}

void PresentPath::setVsyncMode(VsyncMode mode) {
    m_desc.vsyncMode = mode;
    m_status.vsyncMode = mode;

    VulkanSwapchain* swapchain = m_bootstrap.swapchain();
    if (swapchain == nullptr) {
        return;
    }

    SwapchainDesc swapDesc{};
    swapDesc.width = m_status.width;
    swapDesc.height = m_status.height;
    swapDesc.vsyncMode = mode;
    (void)m_bootstrap.ensureSwapchain(swapDesc);
    refreshDimensions();
}

void PresentPath::requestResize(u32 width, u32 height) {
    m_status.lastResizeOutcome =
        classifyResizeRequest(m_status.state, m_status.resizePending, m_status.pendingResizeWidth,
                              m_status.pendingResizeHeight, width, height);

    if (m_status.lastResizeOutcome == ResizeRequestOutcome::RejectedInvalidExtent) {
        ++m_status.resizeRejectedCount;
        m_status.message = "Resize rejected — extent must be non-zero";
        return;
    }

    if (m_status.lastResizeOutcome == ResizeRequestOutcome::DuplicateIgnored) {
        ++m_status.resizeDuplicateCount;
        m_status.message = "Resize duplicate ignored — extent already pending";
        return;
    }

    if (m_status.lastResizeOutcome == ResizeRequestOutcome::Coalesced) {
        ++m_status.resizeCoalesceCount;
    }

    const bool midPresentCycle = m_status.lastResizeOutcome == ResizeRequestOutcome::DeferredDuringPresent;
    if (midPresentCycle) {
        ++m_status.resizeDeferredCount;
    }

    m_status.pendingResizeWidth = width;
    m_status.pendingResizeHeight = height;
    m_status.resizePending = true;
    if (!midPresentCycle && m_status.state != PresentPathState::ResizePending) {
        m_status.state = PresentPathState::ResizePending;
    }
    m_status.message = midPresentCycle ? "Resize deferred until present cycle completes"
                                       : "Resize queued — recreate on next fence wait or recreateSwapchain()";
}

bool PresentPath::recreateSwapchain() {
    if (!m_status.resizePending) {
        m_status.message = "recreateSwapchain: no pending resize";
        return true;
    }
    return processPendingResize();
}

} // namespace fuse::renderer
