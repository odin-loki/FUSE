#include <fuse/renderer/vk/frame.hpp>
#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

namespace fuse::renderer {

bool isSwapchainPresentable(const VulkanSwapchain& swapchain) {
    return swapchain.isReady() && swapchain.nativeHandle() != nullptr && swapchain.hasImages();
}

bool isSwapchainEmpty(const VulkanSwapchain& swapchain) {
    return !swapchain.isReady() || swapchain.nativeHandle() == nullptr || !swapchain.hasImages();
}

bool shouldEarlyOutEmptySwapchainAcquire(const VulkanSwapchain* swapchain, const FrameManager* frameManager) {
    if (swapchain == nullptr) {
        return true;
    }
    if (frameManager == nullptr || !frameManager->isReady()) {
        return true;
    }
    return isSwapchainEmpty(*swapchain);
}

} // namespace fuse::renderer
