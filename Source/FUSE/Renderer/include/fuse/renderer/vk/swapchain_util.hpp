#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer {

class VulkanSwapchain;

/// Returns true when width and height are both non-zero.
inline bool isValidSwapchainExtent(u32 width, u32 height) {
    return width > 0 && height > 0;
}

/// UINT32_MAX is the sentinel for headless, OUT_OF_DATE, or failed acquire.
inline bool isEmptyAcquireResult(u32 imageIndex) {
    return imageIndex == UINT32_MAX;
}

/// True when the swapchain is ready and has a native handle (can acquire/present).
bool isSwapchainPresentable(const VulkanSwapchain& swapchain);

/// True when the swapchain has no backing images (headless stub or not ready).
bool isSwapchainEmpty(const VulkanSwapchain& swapchain);

} // namespace fuse::renderer
