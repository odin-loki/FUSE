#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/renderer_bootstrap.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/fence_wait.hpp>
#include <fuse/renderer/vk/queue_submit.hpp>
#include <fuse/renderer/vk/present_path.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testHeadlessQueueSubmit() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated");

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan device — queue submit state machine only\n");
        return;
    }
#else
    std::printf("SKIP: Vulkan backend disabled\n");
    return;
#endif

    fuse::renderer::FrameManager* frameManager = bootstrap->frameManager();
    expectTrue(frameManager != nullptr && frameManager->isReady(), "frame manager ready");

    fuse::renderer::GraphicsQueueSubmitDesc submitDesc{};
    submitDesc.device = bootstrap->device();
    submitDesc.frameManager = frameManager;
    submitDesc.swapchain = bootstrap->swapchain();
    submitDesc.acquiredImageIndex = UINT32_MAX;

    const fuse::renderer::GraphicsQueueSubmitResult result = fuse::renderer::submitGraphicsQueue(submitDesc);
    expectTrue(result.ok, "headless vkQueueSubmit succeeds");
    expectTrue(result.submitted, "headless path records a real submit");
    expectTrue(result.headless, "headless path omits WSI semaphores");
    expectTrue(!result.semaphoresUsed, "headless path does not wire acquire semaphores");

    expectTrue(fuse::renderer::countPendingInFlightFences(*frameManager) >= 1u,
               "submit signals in-flight fence");
    expectTrue(fuse::renderer::waitInFlightFenceForSlot(*frameManager, frameManager->currentIndex()),
               "fence wait after headless submit succeeds");
}

void testQueueSubmitThroughRhiContext() {
    fuse::core::initialize();

    fuse::renderer::RendererBootstrapDesc desc{};
    desc.rhi.bootstrap.instance.enableValidation = false;
    desc.rhi.bootstrap.createSwapchain = false;

    auto bootstrap = fuse::renderer::RendererBootstrap::create(desc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        fuse::core::shutdown();
        return;
    }
#else
    fuse::core::shutdown();
    return;
#endif

    fuse::renderer::RhiContext* rhi = bootstrap->rhiContext();
    expectTrue(rhi != nullptr, "RhiContext available");

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.f, 0.f, 0.f);

    expectTrue(rhi->beginFrame(0u), "beginFrame on render thread");
    rhi->setAcquiredSwapchainImage(UINT32_MAX);
    expectTrue(rhi->submitFrame(commands, 0u), "submitFrame issues vkQueueSubmit");
    expectTrue(rhi->queueSubmitCount() >= 1u, "queue submit counter advanced");
    expectTrue(rhi->lastQueueSubmitOk(), "last queue submit ok");

    bootstrap->shutdown();
    fuse::core::shutdown();
}

void testPresentPathWithQueueSubmitMirror() {
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
    presentPath->noteQueueSubmit(true, true, true);
    expectTrue(presentPath->status().lastQueueSubmitOk, "present path mirrors submit ok");
    expectTrue(presentPath->status().queueSubmitCount == 1u, "present path mirrors submit count");

    expectTrue(presentPath->waitInFlightFence(), "fence wait before acquire");
    (void)presentPath->acquireImage();
    presentPath->noteQueueSubmit(true, true, true);
    expectTrue(presentPath->markReadyToPresent(), "ready after render record");
    expectTrue(presentPath->presentImage(), "headless present sink after submit");
    expectTrue(presentPath->status().presentSkippedNoWsiCount >= 1u,
               "headless present counted as no-WSI sink");
}

void testShouldUseSwapchainSemaphoresHelper() {
    expectTrue(!fuse::renderer::shouldUseSwapchainSemaphores(nullptr, 0u),
               "null swapchain never uses WSI semaphores");
    expectTrue(!fuse::renderer::shouldUseSwapchainSemaphores(nullptr, UINT32_MAX),
               "UINT32_MAX acquire never uses WSI semaphores");
}

} // namespace

int main() {
    fuse::core::initialize();

    testShouldUseSwapchainSemaphoresHelper();
    testHeadlessQueueSubmit();
    testQueueSubmitThroughRhiContext();
    testPresentPathWithQueueSubmitMirror();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_rhi_queue_submit: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_rhi_queue_submit: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
