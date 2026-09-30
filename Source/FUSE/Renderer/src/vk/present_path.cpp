#include <fuse/renderer/vk/present_path.hpp>

#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/fence_wait.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

PresentPath::PresentPath(VulkanBootstrap& bootstrap, PresentPathDesc desc)
    : m_bootstrap(bootstrap), m_desc(desc) {
    m_status.vsyncMode = desc.vsyncMode;
    m_status.desktopPresentEnabled = desktopGlfwPresentEnabled();
    m_status.qtPresentEnabled = desktopQtPresentEnabled();
    m_status.desktopPresentRuntimeReady = desktopPresentRuntimeReady();
    m_status.qtPresentRuntimeReady = desktopQtPresentRuntimeReady();
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

    if (pendingResizeMatchesCurrentExtent(m_status.width, m_status.height, m_status.pendingResizeWidth,
                                          m_status.pendingResizeHeight)) {
        m_status.resizePending = false;
        ++m_status.resizeNoOpCount;
        m_status.message = "Resize no-op — pending extent matches current swapchain";
        return true;
    }

    FrameManager* frameManager = m_bootstrap.frameManager();
    if (frameManager != nullptr && frameManager->isReady()) {
        // A real swapchain may have been wired after this path was created (m_status.headless is
        // then stale): every slot's commands can reference its render pass / framebuffers.
        const VulkanSwapchain* current = m_bootstrap.swapchain();
        const bool waitAllSlots = !m_status.headless || (current != nullptr && !current->isHeadless());
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
    m_status.message = m_status.headless ? "Headless fence wait (slot bookkeeping before submit)"
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
    m_renderFinishedSignalled = false;
    if (!shouldSkipAcquireForEmptySwapchain(swapchain) && frameManager != nullptr && frameManager->isReady()) {
        m_acquireSlot = frameManager->currentIndex();
        const FrameSyncData& slot = frameManager->slot(m_acquireSlot);
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
        ++m_status.acquiredImageCount;
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

    const bool realPresentEligibleNow =
        realPresentEligible(swapchain, m_status.acquiredImageIndex, frameManager);
    const bool realQtPresentEligibleNow =
        realQtPresentEligible(swapchain, m_status.acquiredImageIndex, frameManager);

    bool presented = false;
    // FUSE Track B: skip vkQueuePresentKHR unless VulkanProduction is unlocked. Never present an
    // image no submit rendered (its renderFinished semaphore would never signal).
    if (!productionPresentAllowed() || !m_renderFinishedSignalled ||
        shouldEarlyOutEmptyPresent(swapchain, m_status.acquiredImageIndex, frameManager)) {
        presented = true;
        ++m_status.emptyPresentCount;
    } else {
        const FrameSyncData& slot = frameManager->slot(m_acquireSlot);
        presented = swapchain->present(slot.renderFinished, m_status.acquiredImageIndex);
        if (presented && realPresentEligibleNow) {
            ++m_status.realPresentCallCount;
        }
        if (presented && realQtPresentEligibleNow) {
            ++m_status.qtRealPresentCallCount;
        }
    }

    m_status.presentAttempted = true;
    if (!presented) {
        m_status.message = "Swapchain present failed";
        return false;
    }

    ++m_status.presentedFrames;
    const bool headlessSink = !realPresentEligibleNow || !m_renderFinishedSignalled;
    m_renderFinishedSignalled = false;
    m_status.acquiredImageIndex = UINT32_MAX;
    m_status.fenceWaited = false;
    m_status.acquireAttempted = false;
    m_status.presentAttempted = false;
    m_status.state = PresentPathState::Presented;
    if (headlessSink) {
        ++m_status.presentSkippedNoWsiCount;
        m_status.message = m_status.lastQueueSubmitOk
                               ? "Headless present sink (vkQueueSubmit done, no WSI)"
                               : "Headless present stub (no queue submit)";
    } else {
        m_status.message = "Swapchain image presented via vkQueuePresentKHR";
    }
    return true;
}

void PresentPath::noteQueueSubmit(bool ok, bool submitted, bool headless) {
    m_status.lastQueueSubmitOk = ok;
    // A non-headless submit waited on imageAvailable and signalled renderFinished (queue_submit.cpp).
    m_renderFinishedSignalled = ok && submitted && !headless &&
                                (m_status.state == PresentPathState::ImageAcquired ||
                                 m_status.state == PresentPathState::ReadyToPresent);
    if (submitted) {
        ++m_status.queueSubmitCount;
    }
    if (headless) {
        m_status.headless = true;
    }
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

    if (m_status.resizePending &&
        isDuplicatePendingResizeExtent(m_status.pendingResizeWidth, m_status.pendingResizeHeight, width, height)) {
        ++m_status.resizeDuplicateCount;
        m_status.message = "Resize duplicate ignored — extent already pending";
        return;
    }

    if (isResizeCoalesceRequest(m_status.resizePending, m_status.pendingResizeWidth, m_status.pendingResizeHeight,
                                width, height)) {
        ++m_status.resizeCoalesceCount;
    }

    const bool midPresentCycle = shouldDeferResizeDuringPresentCycle(m_status.state);
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

// --- render-graph present hand-off (E02) -----------------------------------------------------------------------------

namespace {
constexpr u32 kLayoutPresentSrc = 1000001002u; // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR

void recordPresentBlit(const rg::PassContext& context, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const PresentBlit& b = *static_cast<const PresentBlit*>(user);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<s32>(b.sourceWidth), static_cast<s32>(b.sourceHeight), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<s32>(b.targetWidth), static_cast<s32>(b.targetHeight), 1};
    const bool scaled = b.sourceWidth != b.targetWidth || b.sourceHeight != b.targetHeight;
    vkCmdBlitImage(static_cast<VkCommandBuffer>(context.commandBuffer), static_cast<VkImage>(context.image(b.source)),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkImage>(context.image(b.target)),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   scaled && b.linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
#else
    (void)context;
    (void)user;
#endif
}
} // namespace

rg::TextureRef importPresentTarget(rg::Graph& graph, const PresentTargetDesc& target) {
    if (target.image == nullptr || target.width == 0u || target.height == 0u || target.format == 0u) {
        return rg::TextureRef{};
    }
    rg::ImportedImage i{};
    i.image = target.image;
    i.view = target.view;
    i.format = target.format;
    i.width = target.width;
    i.height = target.height;
    i.name = target.name;
    if (target.swapchain) {
        i.initialLayout = 0u; // acquired image: previous contents are discarded
        i.finalLayout = kLayoutPresentSrc;
    } else {
        i.initialLayout = target.layoutTracker != nullptr ? *target.layoutTracker : 0u;
        i.initialQueue = target.queueTracker != nullptr ? *target.queueTracker : rg::kNoQueue;
        i.layoutTracker = target.layoutTracker;
        i.queueTracker = target.queueTracker;
    }
    return graph.importImage(i);
}

bool addPresentBlit(rg::Graph& graph, PresentBlit& blit) {
    if (!blit.source.valid() || !blit.target.valid() || blit.sourceWidth == 0u || blit.sourceHeight == 0u ||
        blit.targetWidth == 0u || blit.targetHeight == 0u) {
        return false;
    }
    graph.addPass("present.blit", &recordPresentBlit, &blit)
        .use(blit.source, rg::Access::TransferSrc)
        .use(blit.target, rg::Access::TransferDst)
        .neverCull();
    if (blit.present) {
        graph.addPass("present.handoff", nullptr, nullptr).use(blit.target, rg::Access::Present).neverCull();
    }
    return true;
}

} // namespace fuse::renderer
