#include <fuse/core/init.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/fence_wait.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/renderer/vk/present_path.hpp>
#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (got %u, expected %u)\n", message, actual, expected);
        ++g_failures;
    }
}

void testHeadlessPresentStateMachine() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated");

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan device — headless present path state machine only\n");
        return;
    }
#else
    std::printf("SKIP: Vulkan backend disabled — present path requires stub bootstrap\n");
    return;
#endif

    fuse::renderer::PresentPathDesc presentDesc{};
    presentDesc.vsyncMode = fuse::renderer::VsyncMode::Fifo;
    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap, presentDesc);
    expectTrue(presentPath != nullptr, "present path allocated");
    expectTrue(presentPath->status().headless, "CI path is headless");

    expectTrue(presentPath->waitInFlightFence(), "fence wait succeeds on headless path");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::FenceWaited,
               "state advances to FenceWaited");

    const fuse::u32 imageIndex = presentPath->acquireImage();
    expectEq(imageIndex, UINT32_MAX, "headless acquire returns UINT32_MAX");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::ImageAcquired,
               "state advances to ImageAcquired");

    expectTrue(presentPath->presentImage(), "headless present succeeds");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::Presented,
               "state advances to Presented");
    expectEq(presentPath->status().presentedFrames, 1u, "present frame counter increments");
}

void testBeginEndFrameCycle() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    expectTrue(presentPath->beginFrame(0u), "beginFrame runs wait+acquire");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::ReadyToPresent,
               "beginFrame leaves path ready to present");
    expectTrue(presentPath->endFrame(), "endFrame presents and advances frame ring");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::Idle,
               "endFrame returns to Idle");
}

void testVsyncModeEnum() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    presentPath->setVsyncMode(fuse::renderer::VsyncMode::Immediate);
    expectTrue(presentPath->vsyncMode() == fuse::renderer::VsyncMode::Immediate,
               "vsync mode stored on present path");
    expectTrue(!fuse::renderer::vsyncEnabled(presentPath->vsyncMode()),
               "Immediate mode disables vsync gate");
}

void testResizeRecreateStub() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    presentPath->requestResize(1024, 768);
    expectTrue(presentPath->hasPendingResize(), "resize queued");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::ResizePending,
               "state marks resize pending");

    expectTrue(presentPath->waitInFlightFence(), "fence wait processes pending resize");
    expectTrue(!presentPath->hasPendingResize(), "resize cleared after recreate");
    expectEq(presentPath->status().width, 1024u, "resize width recorded");
    expectEq(presentPath->status().height, 768u, "resize height recorded");
    expectEq(presentPath->status().swapchainRecreateCount, 1u, "resize increments recreate counter");
}

void testInvalidStateTransitions() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    expectEq(presentPath->acquireImage(), UINT32_MAX, "acquire without fence wait rejected");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::Idle,
               "invalid acquire keeps Idle state");

    expectTrue(presentPath->waitInFlightFence(), "fence wait from Idle");
    (void)presentPath->acquireImage();
    expectTrue(presentPath->markReadyToPresent(), "mark ready after acquire");
    expectTrue(presentPath->presentImage(), "first present succeeds");
    expectTrue(!presentPath->presentImage(), "second present without re-acquire fails");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::Presented,
               "state stays Presented after failed second present");
}

void testVsyncModeSwitch() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    presentPath->setVsyncMode(fuse::renderer::VsyncMode::Mailbox);
    expectTrue(presentPath->vsyncMode() == fuse::renderer::VsyncMode::Mailbox,
               "Mailbox vsync mode applied");
    expectTrue(std::strcmp(fuse::renderer::vsyncModeName(presentPath->vsyncMode()), "Mailbox") == 0,
               "vsync mode name helper");

    presentPath->setVsyncMode(fuse::renderer::VsyncMode::Fifo);
    expectTrue(fuse::renderer::vsyncEnabled(presentPath->vsyncMode()), "Fifo enables vsync gate");
}

void testFenceWaitHelpers() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    fuse::renderer::FrameManager* frameManager = bootstrap->frameManager();
    expectTrue(frameManager != nullptr && frameManager->isReady(), "frame manager ready");

    expectTrue(fuse::renderer::waitInFlightFenceForSlot(*frameManager, frameManager->currentIndex()),
               "per-slot fence wait helper");
    expectTrue(fuse::renderer::waitAllInFlightFences(*frameManager),
               "wait-all helper succeeds on fresh ring");
}

void testMultipleFrameCycles() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    for (fuse::u32 frame = 0; frame < 3u; ++frame) {
        expectTrue(presentPath->beginFrame(frame), "beginFrame succeeds for multi-frame loop");
        expectTrue(presentPath->endFrame(), "endFrame succeeds for multi-frame loop");
        expectTrue(presentPath->state() == fuse::renderer::PresentPathState::Idle,
                   "endFrame returns to Idle each iteration");
    }
    expectEq(presentPath->status().presentedFrames, 3u, "presented frame counter tracks three cycles");
    expectEq(presentPath->status().fenceWaitCount, 3u, "fence wait counter tracks three cycles");
}

void testResizeCoalescing() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    presentPath->requestResize(800, 600);
    presentPath->requestResize(1920, 1080);
    expectTrue(presentPath->hasPendingResize(), "resize still pending after coalesce");

    expectTrue(presentPath->recreateSwapchain(), "explicit recreate applies coalesced dimensions");
    expectEq(presentPath->status().width, 1920u, "coalesced resize width applied");
    expectEq(presentPath->status().height, 1080u, "coalesced resize height applied");
    expectEq(presentPath->status().swapchainRecreateCount, 1u, "coalesced resize counts as one recreate");
}

void testRecreateWithoutPendingResize() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    expectTrue(presentPath->recreateSwapchain(), "recreate without pending resize is a no-op success");
    expectTrue(!presentPath->hasPendingResize(), "no pending resize after no-op recreate");
    expectEq(presentPath->status().swapchainRecreateCount, 0u, "no-op recreate does not bump counter");
}

void testFenceWaitOutOfBounds() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    fuse::renderer::FrameManager* frameManager = bootstrap->frameManager();
    expectTrue(frameManager != nullptr && frameManager->isReady(), "frame manager ready");

    expectTrue(!fuse::renderer::isValidFrameSlotIndex(fuse::renderer::kFramesInFlight),
               "slot index at ring size is invalid");
    expectTrue(!fuse::renderer::waitInFlightFenceForSlot(*frameManager, fuse::renderer::kFramesInFlight),
               "OOB slot fence wait rejected");
    expectTrue(!fuse::renderer::waitInFlightFenceForSlot(*frameManager, 99u),
               "far OOB slot fence wait rejected");
}

void testFenceWaitAfterEndFrame() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    fuse::renderer::FrameManager* frameManager = bootstrap->frameManager();
    expectTrue(frameManager != nullptr && frameManager->isReady(), "frame manager ready");

    expectTrue(fuse::renderer::waitAllInFlightFences(*frameManager),
               "clear initial signaled fences on fresh ring");
    expectEq(fuse::renderer::countPendingInFlightFences(*frameManager), 0u,
             "ring has no pending in-flight fences after wait-all");

    const fuse::u32 slotBeforeEnd = frameManager->currentIndex();
    frameManager->signalTickComplete();
    frameManager->beginFrame(0u);
    frameManager->endFrame();

    expectEq(fuse::renderer::countPendingInFlightFences(*frameManager), 1u,
             "endFrame marks submitted slot in-flight");

    expectTrue(fuse::renderer::waitInFlightFenceForSlot(*frameManager, slotBeforeEnd),
               "wait submitted slot after endFrame succeeds");
    expectEq(fuse::renderer::countPendingInFlightFences(*frameManager), 0u,
             "fence wait clears pending count");
}

void testMarkReadyWithoutAcquire() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    expectTrue(!presentPath->markReadyToPresent(), "mark ready without acquire rejected");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::Idle,
               "invalid mark ready keeps Idle");
}

void testPresentFromIdle() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    expectTrue(!presentPath->presentImage(), "present from Idle rejected");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::Idle,
               "failed present from Idle stays Idle");
}

void testResizePreservesVsyncMode() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    presentPath->setVsyncMode(fuse::renderer::VsyncMode::Mailbox);
    presentPath->requestResize(640, 480);
    expectTrue(presentPath->recreateSwapchain(), "resize recreate succeeds");
    expectTrue(presentPath->vsyncMode() == fuse::renderer::VsyncMode::Mailbox,
               "vsync mode preserved across resize recreate");
}

void testFenceWaitOutOfOrder() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    expectTrue(presentPath->waitInFlightFence(), "fence wait from Idle");
    (void)presentPath->acquireImage();
    expectTrue(!presentPath->waitInFlightFence(), "fence wait rejected mid-frame");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::ImageAcquired,
               "out-of-order fence wait keeps current state");
}

void testResizeDuringPresentCycle() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    expectTrue(presentPath->beginFrame(0u), "beginFrame starts cycle");
    presentPath->requestResize(1600, 900);
    expectTrue(presentPath->hasPendingResize(), "resize queued during active frame");
    expectTrue(presentPath->endFrame(), "endFrame completes despite pending resize");
    expectTrue(presentPath->hasPendingResize(), "resize still pending after present");

    expectTrue(presentPath->waitInFlightFence(), "next fence wait applies deferred resize");
    expectEq(presentPath->status().width, 1600u, "deferred resize width applied");
    expectEq(presentPath->status().height, 900u, "deferred resize height applied");
}

void testZeroExtentResizeRejected() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    presentPath->requestResize(0, 0);
    expectTrue(!presentPath->hasPendingResize(), "zero extent resize not queued");
    expectEq(presentPath->status().resizeRejectedCount, 1u, "zero extent increments reject counter");

    presentPath->requestResize(1280, 0);
    expectTrue(!presentPath->hasPendingResize(), "partial zero extent rejected");
    expectEq(presentPath->status().resizeRejectedCount, 2u, "partial zero extent increments reject counter");
}

void testResizeCoalesceCounter() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    presentPath->requestResize(800, 600);
    presentPath->requestResize(1920, 1080);
    expectEq(presentPath->status().resizeCoalesceCount, 1u, "second resize coalesces into pending");
    expectTrue(presentPath->recreateSwapchain(), "coalesced recreate succeeds");
    expectEq(presentPath->status().swapchainRecreateCount, 1u, "single recreate for coalesced resize");
}

void testCanAcquireCanPresent() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    expectTrue(!presentPath->canAcquire(), "cannot acquire from Idle");
    expectTrue(!presentPath->canPresent(), "cannot present from Idle");

    expectTrue(presentPath->waitInFlightFence(), "fence wait");
    expectTrue(presentPath->canAcquire(), "can acquire after fence wait on headless path");
    (void)presentPath->acquireImage();
    expectTrue(presentPath->canPresent(), "can present after headless acquire");
}

void testEmptyAcquirePresentCounters() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    expectTrue(presentPath->beginFrame(0u), "beginFrame on headless path");
    expectEq(presentPath->status().emptyAcquireCount, 1u, "headless acquire counted as empty");
    expectTrue(presentPath->endFrame(), "endFrame on headless path");
    expectEq(presentPath->status().emptyPresentCount, 1u, "headless present counted as empty stub");
}

void testFenceWaitIfSignaledHelper() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    fuse::renderer::FrameManager* frameManager = bootstrap->frameManager();
    expectTrue(frameManager != nullptr && frameManager->isReady(), "frame manager ready");

    expectTrue(fuse::renderer::waitAllInFlightFences(*frameManager),
               "clear initial signaled fences on fresh ring");
    expectTrue(!fuse::renderer::hasPendingInFlightFences(*frameManager),
               "ring has no pending fences after wait-all");
    expectTrue(fuse::renderer::waitInFlightFenceIfSignaled(*frameManager, frameManager->currentIndex()),
               "if-signaled wait is no-op on clear slot");

    const fuse::u32 slotBeforeEnd = frameManager->currentIndex();
    frameManager->signalTickComplete();
    frameManager->beginFrame(0u);
    frameManager->endFrame();
    expectTrue(fuse::renderer::hasPendingInFlightFences(*frameManager),
               "endFrame leaves slot in-flight");

    expectTrue(fuse::renderer::waitInFlightFenceIfSignaled(*frameManager, slotBeforeEnd),
               "if-signaled wait clears submitted slot");
    expectTrue(!fuse::renderer::hasPendingInFlightFences(*frameManager),
               "if-signaled wait drains pending count");
}

void testReadyToPresentTransition() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    expectTrue(presentPath->waitInFlightFence(), "fence wait");
    (void)presentPath->acquireImage();
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::ImageAcquired,
               "acquire reaches ImageAcquired");
    expectTrue(presentPath->markReadyToPresent(), "mark ready to present");
    expectTrue(presentPath->state() == fuse::renderer::PresentPathState::ReadyToPresent,
               "ReadyToPresent state wired");
    expectTrue(presentPath->presentImage(), "present from ReadyToPresent");
}

void testDuplicateResizeIgnored() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    presentPath->requestResize(1280, 720);
    expectTrue(presentPath->hasPendingResize(), "initial resize queued");
    expectEq(presentPath->pendingResizeWidth(), 1280u, "pending width stored");
    expectEq(presentPath->pendingResizeHeight(), 720u, "pending height stored");

    presentPath->requestResize(1280, 720);
    expectEq(presentPath->status().resizeDuplicateCount, 1u, "duplicate resize increments counter");
    expectEq(presentPath->status().resizeCoalesceCount, 0u, "duplicate does not count as coalesce");
    expectEq(presentPath->pendingResizeWidth(), 1280u, "pending width unchanged after duplicate");
}

void testIsPresentCycleActiveHelper() {
    expectTrue(!fuse::renderer::isPresentCycleActive(fuse::renderer::PresentPathState::Idle),
               "Idle is not an active present cycle");
    expectTrue(!fuse::renderer::isPresentCycleActive(fuse::renderer::PresentPathState::Presented),
               "Presented is not an active present cycle");
    expectTrue(fuse::renderer::isPresentCycleActive(fuse::renderer::PresentPathState::FenceWaited),
               "FenceWaited is active present cycle");
    expectTrue(fuse::renderer::isPresentCycleActive(fuse::renderer::PresentPathState::ImageAcquired),
               "ImageAcquired is active present cycle");
    expectTrue(fuse::renderer::isPresentCycleActive(fuse::renderer::PresentPathState::ReadyToPresent),
               "ReadyToPresent is active present cycle");
}

void testCanWaitInFlightFenceForSlot() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    fuse::renderer::FrameManager* frameManager = bootstrap->frameManager();
    expectTrue(frameManager != nullptr && frameManager->isReady(), "frame manager ready");

    expectTrue(fuse::renderer::canWaitInFlightFenceForSlot(*frameManager, frameManager->currentIndex()),
               "current slot passes preflight");
    expectTrue(!fuse::renderer::canWaitInFlightFenceForSlot(*frameManager, fuse::renderer::kFramesInFlight),
               "OOB slot fails preflight");
}

void testWaitFencesBeforeRecreateHelper() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    fuse::renderer::FrameManager* frameManager = bootstrap->frameManager();
    expectTrue(frameManager != nullptr && frameManager->isReady(), "frame manager ready");

    expectTrue(fuse::renderer::waitInFlightFencesBeforeRecreate(*frameManager, false),
               "headless recreate wait uses current slot");
    expectTrue(fuse::renderer::waitInFlightFencesBeforeRecreate(*frameManager, true),
               "GPU recreate wait drains all slots");
}

void testLastPendingFenceCountOnWait() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        return;
    }
#else
    return;
#endif

    auto presentPath = fuse::renderer::PresentPath::create(*bootstrap);
    fuse::renderer::FrameManager* frameManager = bootstrap->frameManager();
    expectTrue(frameManager != nullptr && frameManager->isReady(), "frame manager ready");

    const fuse::u32 pendingBefore = fuse::renderer::countPendingInFlightFences(*frameManager);
    expectTrue(presentPath->waitInFlightFence(), "fence wait succeeds");
    expectEq(presentPath->status().lastPendingFenceCount, pendingBefore,
             "lastPendingFenceCount mirrors ring before wait");
}

} // namespace

int main() {
    fuse::core::initialize();

    testHeadlessPresentStateMachine();
    testBeginEndFrameCycle();
    testVsyncModeEnum();
    testResizeRecreateStub();
    testInvalidStateTransitions();
    testVsyncModeSwitch();
    testFenceWaitHelpers();
    testMultipleFrameCycles();
    testResizeCoalescing();
    testRecreateWithoutPendingResize();
    testFenceWaitOutOfBounds();
    testFenceWaitAfterEndFrame();
    testFenceWaitOutOfOrder();
    testResizeDuringPresentCycle();
    testZeroExtentResizeRejected();
    testResizeCoalesceCounter();
    testCanAcquireCanPresent();
    testEmptyAcquirePresentCounters();
    testFenceWaitIfSignaledHelper();
    testMarkReadyWithoutAcquire();
    testPresentFromIdle();
    testResizePreservesVsyncMode();
    testReadyToPresentTransition();
    testDuplicateResizeIgnored();
    testIsPresentCycleActiveHelper();
    testCanWaitInFlightFenceForSlot();
    testWaitFencesBeforeRecreateHelper();
    testLastPendingFenceCountOnWait();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_rhi_present_path_stub: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_rhi_present_path_stub: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
