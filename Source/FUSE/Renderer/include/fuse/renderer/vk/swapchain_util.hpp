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

/// Returns true when width/height pairs match.
inline bool swapchainExtentsMatch(u32 widthA, u32 heightA, u32 widthB, u32 heightB) {
    return widthA == widthB && heightA == heightB;
}

/// True when acquire should be skipped (empty swapchain — caller uses UINT32_MAX stub).
inline bool shouldSkipSwapchainAcquire(const VulkanSwapchain& swapchain) {
    return isSwapchainEmpty(swapchain);
}

} // namespace fuse::renderer
