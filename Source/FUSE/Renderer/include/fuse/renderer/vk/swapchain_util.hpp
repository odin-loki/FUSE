#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer {

class FrameManager;
class VulkanSwapchain;

/// Returns true when width and height are both non-zero.
inline bool isValidSwapchainExtent(u32 width, u32 height) {
    return width > 0 && height > 0;
}

/// True when pending resize dimensions match the current swapchain extent.
inline bool resizeExtentMatches(u32 pendingWidth, u32 pendingHeight, u32 currentWidth, u32 currentHeight) {
    return pendingWidth == currentWidth && pendingHeight == currentHeight;
}

/// UINT32_MAX is the sentinel for headless, OUT_OF_DATE, or failed acquire.
inline bool isEmptyAcquireResult(u32 imageIndex) {
    return imageIndex == UINT32_MAX;
}

/// True when the swapchain is ready and has a native handle (can acquire/present).
bool isSwapchainPresentable(const VulkanSwapchain& swapchain);

/// True when the swapchain has no backing images (headless stub or not ready).
bool isSwapchainEmpty(const VulkanSwapchain& swapchain);

/// True when acquire should return UINT32_MAX without calling vkAcquireNextImageKHR.
bool shouldEarlyOutEmptySwapchainAcquire(const VulkanSwapchain* swapchain, const FrameManager* frameManager);

} // namespace fuse::renderer
