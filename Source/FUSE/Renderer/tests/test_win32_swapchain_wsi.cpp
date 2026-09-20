#include <fuse/core/init.hpp>
#include <fuse/core/track_b.hpp>
#include <fuse/platform/window.hpp>
#include <fuse/platform/window_wsi.hpp>
#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

#include <cstdio>
#include <cstdlib>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testProductionPresentMatchesTrackB() {
    expectTrue(fuse::renderer::productionPresentAllowed() == fuse::core::trackBUnlocked(),
               "productionPresentAllowed matches Track B unlock");
}

void testHeadlessSwapchainStillCreates() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;

    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    expectTrue(instance != nullptr, "instance allocated");

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    expectTrue(device != nullptr, "device allocated");

    fuse::renderer::SwapchainDesc swapDesc{};
    swapDesc.surface.kind = fuse::renderer::SurfaceKind::Headless;
    swapDesc.width = 1920;
    swapDesc.height = 1080;
    swapDesc.imageCount = 3;

    auto swapchain = fuse::renderer::VulkanSwapchain::create(*device, swapDesc);
    expectTrue(swapchain != nullptr, "headless SwapchainDesc still allocates a swapchain");
    expectTrue(swapchain->isHeadless(), "headless SwapchainDesc still creates a headless swapchain");
    expectTrue(!swapchain->isReady(), "headless swapchain is not presentable");
}

void testWin32NativeSwapchainSmoke() {
#if defined(_WIN32)
    fuse::platform::WindowDesc windowDesc{};
    windowDesc.title = "FUSE B2.2 Win32 Swapchain";
    windowDesc.width = 256;
    windowDesc.height = 256;
    windowDesc.createNative = true;

    fuse::platform::Window window(windowDesc);
    if (window.nativeHandle().value == nullptr) {
        std::printf("SKIP: WindowDesc.createNative did not produce an HWND\n");
        return;
    }

    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    if (instance == nullptr || !instance->isValid()) {
        std::printf("SKIP: no Vulkan ICD\n");
        return;
    }

    fuse::renderer::VulkanDeviceDesc deviceDesc{};
    deviceDesc.requirePresentation = true;
    auto device = fuse::renderer::VulkanDevice::create(*instance, deviceDesc);
    if (device == nullptr || !device->isValid()) {
        std::printf("SKIP: VulkanDevice with requirePresentation is not valid\n");
        return;
    }

    void* surface = nullptr;
    if (!fuse::platform::createVulkanSurface(instance->nativeHandle(), window, &surface) ||
        surface == nullptr) {
        std::printf("SKIP: createVulkanSurface failed\n");
        return;
    }

    fuse::renderer::SwapchainDesc swapDesc{};
    swapDesc.surface.kind = fuse::renderer::SurfaceKind::External;
    swapDesc.surface.nativeSurface = surface;
    swapDesc.width = window.width();
    swapDesc.height = window.height();

    auto swapchain = fuse::renderer::VulkanSwapchain::create(*device, swapDesc);
    expectTrue(swapchain != nullptr, "native-surface swapchain allocated");

    if (swapchain == nullptr || swapchain->isHeadless() || !swapchain->isReady()) {
        std::printf("SKIP: GPU refused present on hidden window (headless/not ready)\n");
    } else if (swapchain->nativeHandle() != nullptr) {
        expectTrue(swapchain->hasImages(), "ready native swapchain has images");
        expectTrue(fuse::renderer::isSwapchainPresentable(*swapchain),
                   "ready native swapchain is presentable");
    }

    swapchain.reset();

#if defined(FUSE_VULKAN_BACKEND)
    vkDestroySurfaceKHR(static_cast<VkInstance>(instance->nativeHandle()),
                        static_cast<VkSurfaceKHR>(surface), nullptr);
#endif
#else
    std::printf("SKIP: native Win32 swapchain path is Windows-only\n");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testProductionPresentMatchesTrackB();
    testHeadlessSwapchainStillCreates();
    testWin32NativeSwapchainSmoke();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_win32_swapchain_wsi: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_win32_swapchain_wsi: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
