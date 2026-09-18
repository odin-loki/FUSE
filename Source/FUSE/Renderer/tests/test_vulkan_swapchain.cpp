#include <fuse/core/init.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/renderer/vk/surface.hpp>
#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

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

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (got %u, expected %u)\n", message, actual, expected);
        ++g_failures;
    }
}

void testHeadlessSwapchainRecordsDesc() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;

    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    expectTrue(instance != nullptr, "instance allocated");

#if defined(FUSE_VULKAN_BACKEND)
    if (!instance->isValid()) {
        std::printf("SKIP: no Vulkan ICD — headless swapchain desc test only\n");
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    expectTrue(device != nullptr && device->isValid(), "device ready");

    fuse::renderer::SwapchainDesc swapDesc{};
    swapDesc.surface.kind = fuse::renderer::SurfaceKind::Headless;
    swapDesc.width = 1920;
    swapDesc.height = 1080;
    swapDesc.imageCount = 3;

    auto swapchain = fuse::renderer::VulkanSwapchain::create(*device, swapDesc);
    expectTrue(swapchain != nullptr, "swapchain object allocated");
    expectTrue(swapchain->isHeadless(), "headless path selected without surface");
    expectTrue(!swapchain->isReady(), "no VkSwapchainKHR without surface");
    expectTrue(swapchain->info().width == 1920u, "width recorded");
    expectTrue(swapchain->info().height == 1080u, "height recorded");
    expectTrue(swapchain->info().imageCount == 3u, "triple-buffer count recorded");

    expectTrue(swapchain->rebuild(*device, 1280, 720), "headless resize succeeds");
    expectTrue(swapchain->info().width == 1280u, "resize width recorded");
#else
    (void)instance;
#endif
}

void testFrameRingAdvances() {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated");

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan device — frame ring test only\n");
        return;
    }

    fuse::renderer::FrameManager* frameManager = bootstrap->frameManager();
    expectTrue(frameManager != nullptr && frameManager->isReady(), "frame manager ready");
    expectTrue(frameManager->info().framesInFlight == fuse::renderer::kFramesInFlight,
               "triple-buffered ring");

    frameManager->signalTickComplete();
    frameManager->beginFrame(0u);
    frameManager->endFrame();
    expectTrue(frameManager->totalFrames() == 1u, "first frame advanced");

    frameManager->signalTickComplete();
    frameManager->beginFrame(1u);
    frameManager->endFrame();
    expectTrue(frameManager->totalFrames() == 2u, "second frame advanced");
    expectTrue(frameManager->currentIndex() == 2u, "ring index wraps toward slot 2");
#else
    expectTrue(!bootstrap->status().frameManagerReady, "stub has no frame ring");
#endif
}

void testExternalSurfaceWithoutHandleFailsGracefully() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;

    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!instance->isValid()) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    if (!device->isValid()) {
        return;
    }

    fuse::renderer::SwapchainDesc swapDesc{};
    swapDesc.surface.kind = fuse::renderer::SurfaceKind::External;
    swapDesc.surface.nativeSurface = nullptr;
    swapDesc.width = 800;
    swapDesc.height = 600;

    auto swapchain = fuse::renderer::VulkanSwapchain::create(*device, swapDesc);
    expectTrue(swapchain != nullptr, "swapchain allocated on invalid external surface");
    expectTrue(!swapchain->isReady(), "invalid external surface does not create swapchain");
    expectTrue(swapchain->isHeadless(), "falls back to headless semantics");
#else
    (void)instance;
#endif
}

void testAcquireOnEmptySwapchain() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;

    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!instance->isValid()) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    if (!device->isValid()) {
        return;
    }

    fuse::renderer::SwapchainDesc swapDesc{};
    swapDesc.surface.kind = fuse::renderer::SurfaceKind::Headless;
    swapDesc.width = 800;
    swapDesc.height = 600;

    auto swapchain = fuse::renderer::VulkanSwapchain::create(*device, swapDesc);
    expectTrue(swapchain != nullptr, "headless swapchain allocated");
    expectTrue(swapchain->isEmpty(), "headless swapchain is empty");
    expectTrue(!swapchain->hasImages(), "headless swapchain has no images");
    expectTrue(!fuse::renderer::isSwapchainPresentable(*swapchain),
               "empty swapchain is not presentable");
    expectTrue(fuse::renderer::isSwapchainEmpty(*swapchain), "util agrees swapchain is empty");

    expectEq(swapchain->acquireNextImage(nullptr), UINT32_MAX, "acquire on empty swapchain returns sentinel");
    expectTrue(!swapchain->present(nullptr, UINT32_MAX), "present with empty index rejected");
#else
    (void)instance;
#endif
}

void testPresentEmptyImageIndex() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;

    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!instance->isValid()) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    if (!device->isValid()) {
        return;
    }

    fuse::renderer::SwapchainDesc swapDesc{};
    swapDesc.surface.kind = fuse::renderer::SurfaceKind::External;
    swapDesc.surface.nativeSurface = nullptr;
    swapDesc.width = 800;
    swapDesc.height = 600;

    auto swapchain = fuse::renderer::VulkanSwapchain::create(*device, swapDesc);
    expectTrue(swapchain != nullptr, "invalid-surface swapchain allocated");
    expectTrue(swapchain->isEmpty(), "invalid surface yields empty swapchain");
    expectTrue(fuse::renderer::isEmptyAcquireResult(UINT32_MAX), "UINT32_MAX is empty acquire sentinel");
    expectTrue(!swapchain->present(nullptr, UINT32_MAX), "present with UINT32_MAX rejected on empty swapchain");
#else
    (void)instance;
#endif
}

void testZeroExtentRebuildRejected() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;

    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!instance->isValid()) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    if (!device->isValid()) {
        return;
    }

    fuse::renderer::SwapchainDesc swapDesc{};
    swapDesc.surface.kind = fuse::renderer::SurfaceKind::Headless;
    swapDesc.width = 640;
    swapDesc.height = 480;

    auto swapchain = fuse::renderer::VulkanSwapchain::create(*device, swapDesc);
    expectTrue(swapchain != nullptr, "headless swapchain allocated");
    expectTrue(!swapchain->rebuild(*device, 0, 0), "zero extent rebuild rejected");
    expectTrue(swapchain->info().width == 640u, "width unchanged after rejected rebuild");
    expectTrue(!fuse::renderer::isValidSwapchainExtent(0, 720), "zero width extent invalid");
    expectTrue(!fuse::renderer::isValidSwapchainExtent(1280, 0), "zero height extent invalid");
#else
    (void)instance;
#endif
}

void testEmptySwapchainSkipGuards() {
    expectTrue(fuse::renderer::shouldSkipAcquireForEmptySwapchain(nullptr),
               "null swapchain skips acquire");
    expectTrue(fuse::renderer::shouldEarlyOutEmptyPresent(nullptr, UINT32_MAX, nullptr),
               "null swapchain early-outs present");

    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;

    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!instance->isValid()) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    if (!device->isValid()) {
        return;
    }

    fuse::renderer::SwapchainDesc swapDesc{};
    swapDesc.surface.kind = fuse::renderer::SurfaceKind::Headless;
    swapDesc.width = 800;
    swapDesc.height = 600;

    auto swapchain = fuse::renderer::VulkanSwapchain::create(*device, swapDesc);
    expectTrue(swapchain != nullptr, "headless swapchain allocated");
    expectTrue(fuse::renderer::shouldSkipAcquireForEmptySwapchain(swapchain.get()),
               "empty swapchain skips acquire helper");
    expectTrue(fuse::renderer::shouldEarlyOutEmptyPresent(swapchain.get(), UINT32_MAX, nullptr),
               "empty acquire index early-outs present");
    expectTrue(fuse::renderer::isDuplicatePendingResizeExtent(1920, 1080, 1920, 1080),
               "matching pending extent is duplicate");
    expectTrue(!fuse::renderer::isDuplicatePendingResizeExtent(1920, 1080, 1280, 720),
               "different extent is not duplicate");
    expectTrue(fuse::renderer::pendingResizeMatchesCurrentExtent(1920, 1080, 1920, 1080),
               "pending extent matching current is no-op candidate");
    expectTrue(!fuse::renderer::pendingResizeMatchesCurrentExtent(1920, 1080, 1280, 720),
               "different pending extent is not no-op");
    expectTrue(fuse::renderer::isResizeCoalesceRequest(true, 800, 600, 1920, 1080),
               "pending resize with new extent is coalesce candidate");
    expectTrue(!fuse::renderer::isResizeCoalesceRequest(false, 0, 0, 1920, 1080),
               "first resize is not coalesce");
    expectTrue(!fuse::renderer::isResizeCoalesceRequest(true, 1920, 1080, 1920, 1080),
               "duplicate pending resize is not coalesce");
#else
    (void)instance;
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testHeadlessSwapchainRecordsDesc();
    testFrameRingAdvances();
    testExternalSurfaceWithoutHandleFailsGracefully();
    testAcquireOnEmptySwapchain();
    testPresentEmptyImageIndex();
    testZeroExtentRebuildRejected();
    testEmptySwapchainSkipGuards();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_vulkan_swapchain: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_vulkan_swapchain: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
