#include <fuse/core/init.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/fence_wait.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/renderer/vk/present_path.hpp>
#include <fuse/renderer/vk/swapchain.hpp>

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
    testReadyToPresentTransition();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_rhi_present_path_stub: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_rhi_present_path_stub: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
