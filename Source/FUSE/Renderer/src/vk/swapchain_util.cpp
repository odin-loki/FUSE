#include <fuse/platform/window_wsi.hpp>
#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

namespace fuse::renderer {

bool desktopGlfwPresentEnabled() {
#if defined(FUSE_ENABLE_GLFW_PRESENT)
    return true;
#else
    return false;
#endif
}

bool desktopGlfwPresentRuntimeReady() {
#if defined(FUSE_ENABLE_GLFW_PRESENT) && defined(FUSE_PLATFORM_WINDOW_GLFW)
    return fuse::platform::windowWsiAvailable();
#else
    return false;
#endif
}

bool isSwapchainPresentable(const VulkanSwapchain& swapchain) {
    return swapchain.isReady() && swapchain.nativeHandle() != nullptr && swapchain.hasImages();
}

bool isSwapchainEmpty(const VulkanSwapchain& swapchain) {
    return !swapchain.isReady() || swapchain.nativeHandle() == nullptr || !swapchain.hasImages();
}

} // namespace fuse::renderer
