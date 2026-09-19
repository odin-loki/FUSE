#include <fuse/editor/viewport_swapchain_recreate.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/fence_wait.hpp>
#include <fuse/renderer/vk/present_path.hpp>

#include <vulkan/vulkan.h>
#endif

namespace fuse::editor {

void drainViewportGpuContext(fuse::renderer::RhiContext& context) {
#if defined(FUSE_VULKAN_BACKEND)
    fuse::renderer::FrameManager* frameManager = context.bootstrap().frameManager();
    if (frameManager != nullptr && frameManager->isReady()) {
        waitAllInFlightFences(*frameManager);
    }

    fuse::renderer::VulkanDevice* device = context.bootstrap().device();
    if (device != nullptr && device->isValid()) {
        vkDeviceWaitIdle(static_cast<VkDevice>(device->nativeHandle()));
    }
#else
    (void)context;
#endif
}

ViewportSwapchainRecreateResult requestViewportSwapchainRecreate(
    fuse::renderer::RhiContext& context, std::unique_ptr<fuse::renderer::PresentPath>& presentPath,
    u32 width, u32 height) {
    ViewportSwapchainRecreateResult result{};
    if (width == 0u || height == 0u) {
        result.note = "invalid viewport resize extent";
        return result;
    }

#if defined(FUSE_VULKAN_BACKEND)
    result.attempted = true;
    if (presentPath == nullptr) {
        presentPath = fuse::renderer::PresentPath::create(context.bootstrap());
    }

    presentPath->requestResize(width, height);
    result.deferred = presentPath->hasPendingResize();
    result.width = width;
    result.height = height;
    result.note = result.deferred ? "viewport resize queued for swapchain recreate"
                                  : "viewport resize rejected by present path";
#else
    (void)context;
    (void)presentPath;
    result.attempted = true;
    result.headlessHonest = true;
    result.width = width;
    result.height = height;
    result.note = "vulkan backend disabled — resize recorded only";
#endif

    return result;
}

ViewportSwapchainRecreateResult applyViewportPendingSwapchainRecreate(
    fuse::renderer::RhiContext& context, std::unique_ptr<fuse::renderer::PresentPath>& presentPath) {
    ViewportSwapchainRecreateResult result{};

#if defined(FUSE_VULKAN_BACKEND)
    if (presentPath == nullptr || !presentPath->hasPendingResize()) {
        result.note = "no pending viewport swapchain recreate";
        return result;
    }

    result.attempted = true;
    result.recreated = presentPath->recreateSwapchain();
    if (result.recreated) {
        const fuse::renderer::PresentPathStatus& status = presentPath->status();
        result.width = status.width;
        result.height = status.height;
        result.headlessHonest = status.headless;
        result.swapchainRecreateCount = status.swapchainRecreateCount;
        result.note = status.headless ? "headless viewport swapchain recreate applied"
                                      : "viewport swapchain recreate applied";
    } else {
        result.note = "viewport swapchain recreate failed";
    }
#else
    (void)context;
    (void)presentPath;
    result.note = "vulkan backend disabled";
#endif

    return result;
}

} // namespace fuse::editor
