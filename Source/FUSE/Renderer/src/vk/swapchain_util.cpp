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

bool desktopWin32PresentEnabled() {
#if defined(FUSE_ENABLE_WIN32_PRESENT)
    return true;
#else
    return false;
#endif
}

bool desktopQtPresentEnabled() {
#if defined(FUSE_ENABLE_QT_PRESENT)
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

bool desktopWin32PresentRuntimeReady() {
#if defined(FUSE_ENABLE_WIN32_PRESENT) && defined(FUSE_PLATFORM_WINDOW_WIN32)
    return fuse::platform::windowWsiAvailable() &&
           fuse::platform::activeWindowWsiKind() == fuse::platform::WindowWsiKind::Win32;
#else
    return false;
#endif
}

bool desktopQtPresentRuntimeReady() {
#if defined(FUSE_ENABLE_QT_PRESENT)
    return fuse::platform::displayServerAvailable();
#else
    return false;
#endif
}

bool desktopPresentRuntimeReady() {
    return desktopGlfwPresentRuntimeReady() || desktopWin32PresentRuntimeReady() ||
           desktopQtPresentRuntimeReady();
}

bool realPresentEligible(const VulkanSwapchain* swapchain, u32 imageIndex,
                         const FrameManager* frameManager) {
    if (!productionPresentAllowed()) {
        return false;
    }
    if (swapchain == nullptr || isSwapchainEmpty(*swapchain) || isEmptyAcquireResult(imageIndex) ||
        frameManager == nullptr || !frameManager->isReady()) {
        return false;
    }
    if (!desktopPresentRuntimeReady() || !isSwapchainPresentable(*swapchain)) {
        return false;
    }
    return true;
}

bool realQtPresentEligible(const VulkanSwapchain* swapchain, u32 imageIndex,
                           const FrameManager* frameManager) {
    if (!productionPresentAllowed()) {
        return false;
    }
    if (swapchain == nullptr || isSwapchainEmpty(*swapchain) || isEmptyAcquireResult(imageIndex) ||
        frameManager == nullptr || !frameManager->isReady()) {
        return false;
    }
    if (!desktopQtPresentRuntimeReady() || !isSwapchainPresentable(*swapchain)) {
        return false;
    }
    return true;
}

bool isSwapchainPresentable(const VulkanSwapchain& swapchain) {
    return swapchain.isReady() && swapchain.nativeHandle() != nullptr && swapchain.hasImages();
}

bool isSwapchainEmpty(const VulkanSwapchain& swapchain) {
    return !swapchain.isReady() || swapchain.nativeHandle() == nullptr || !swapchain.hasImages();
}

} // namespace fuse::renderer
