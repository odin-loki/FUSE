#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

namespace fuse::renderer {

bool isSwapchainPresentable(const VulkanSwapchain& swapchain) {
    return swapchain.isReady() && swapchain.nativeHandle() != nullptr && swapchain.hasImages();
}

bool isSwapchainEmpty(const VulkanSwapchain& swapchain) {
    return !swapchain.isReady() || swapchain.nativeHandle() == nullptr || !swapchain.hasImages();
}

} // namespace fuse::renderer
