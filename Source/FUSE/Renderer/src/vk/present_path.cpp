#include <fuse/renderer/vk/present_path.hpp>

#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/fence_wait.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

namespace fuse::renderer {

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

    FrameManager* frameManager = m_bootstrap.frameManager();
    if (frameManager != nullptr && frameManager->isReady()) {
        const bool waitAllSlots = !m_status.headless;
        if (waitAllSlots) {
            if (!waitAllInFlightFences(*frameManager)) {
                m_status.message = "In-flight fence wait failed before swapchain recreate";
                return false;
            }
        } else if (!waitCurrentInFlightFence(*frameManager)) {
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
        const u32 slotIndex = frameManager->currentIndex();
        if (!isInFlightFencePending(*frameManager, slotIndex)) {
            ++m_status.fenceWaitSkippedCount;
        } else if (!waitCurrentInFlightFenceIfSignaled(*frameManager)) {
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

    u32 imageIndex = UINT32_MAX;
    if (swapchain != nullptr && shouldSkipSwapchainAcquire(*swapchain)) {
        // Empty-swapchain early-out — skip vkAcquireNextImageKHR on headless / not-ready paths.
    } else if (swapchain != nullptr && swapchain->isReady() && frameManager != nullptr &&
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

    bool presented = false;
    const bool useEmptyPresentStub =
        swapchain == nullptr || isSwapchainEmpty(*swapchain) ||
        isEmptyAcquireResult(m_status.acquiredImageIndex) || frameManager == nullptr ||
        !frameManager->isReady();

    if (useEmptyPresentStub) {
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
    if (!isValidSwapchainExtent(width, height)) {
        ++m_status.resizeRejectedCount;
        m_status.message = "Resize rejected — extent must be non-zero";
        return;
    }

    if (!m_status.resizePending &&
        swapchainExtentsMatch(width, height, m_status.width, m_status.height)) {
        ++m_status.resizeNoOpCount;
        m_status.message = "Resize no-op — extent matches current dimensions";
        return;
    }

    if (m_status.resizePending) {
        if (swapchainExtentsMatch(width, height, m_status.pendingResizeWidth,
                                  m_status.pendingResizeHeight)) {
            ++m_status.resizeNoOpCount;
            m_status.message = "Resize no-op — extent matches pending dimensions";
            return;
        }
        ++m_status.resizeCoalesceCount;
    }

    const bool midPresentCycle =
        m_status.state == PresentPathState::FenceWaited ||
        m_status.state == PresentPathState::ImageAcquired ||
        m_status.state == PresentPathState::ReadyToPresent;

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

void PresentPath::cancelPendingResize() {
    if (!m_status.resizePending) {
        m_status.message = "cancelPendingResize: no pending resize";
        return;
    }

    m_status.resizePending = false;
    m_status.pendingResizeWidth = 0;
    m_status.pendingResizeHeight = 0;
    if (m_status.state == PresentPathState::ResizePending) {
        m_status.state = PresentPathState::Idle;
    }
    m_status.message = "Pending resize cancelled";
}

} // namespace fuse::renderer
