#pragma once

#include <fuse/renderer/vk/frame.hpp>
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

/// True when acquire should short-circuit without calling vkAcquireNextImageKHR.
inline bool shouldSkipAcquireForEmptySwapchain(const VulkanSwapchain* swapchain) {
    return swapchain == nullptr || isSwapchainEmpty(*swapchain);
}

/// Compile-time gate: desktop GLFW `vkQueuePresentKHR` path (OFF in headless CI by default).
bool desktopGlfwPresentEnabled();

/// Compile-time gate: desktop Qt `vkQueuePresentKHR` path (OFF in headless CI by default).
bool desktopQtPresentEnabled();

/// Runtime: display + GLFW WSI available and desktop present gate is ON.
bool desktopGlfwPresentRuntimeReady();

/// Runtime: display available and Qt present gate is ON (editor QVulkan surface path).
bool desktopQtPresentRuntimeReady();

/// Runtime: either GLFW or Qt desktop present path is eligible.
bool desktopPresentRuntimeReady();

/// True when a real `vkQueuePresentKHR` call may proceed for the current swapchain acquire.
bool realPresentEligible(const VulkanSwapchain* swapchain, u32 imageIndex, const FrameManager* frameManager);

/// True when Qt gate + display + swapchain can call `vkQueuePresentKHR` (excludes GLFW-only path).
bool realQtPresentEligible(const VulkanSwapchain* swapchain, u32 imageIndex,
                           const FrameManager* frameManager);

/// True when present should succeed without calling vkQueuePresentKHR.
inline bool shouldEarlyOutEmptyPresent(const VulkanSwapchain* swapchain,
                                       u32 imageIndex,
                                       const FrameManager* frameManager) {
    return !realPresentEligible(swapchain, imageIndex, frameManager);
}

/// Returns true when a resize request matches already-queued pending dimensions.
inline bool isDuplicatePendingResizeExtent(u32 pendingWidth,
                                           u32 pendingHeight,
                                           u32 requestedWidth,
                                           u32 requestedHeight) {
    return pendingWidth == requestedWidth && pendingHeight == requestedHeight;
}

/// Returns true when pending resize matches current swapchain extent (recreate would be a no-op).
inline bool pendingResizeMatchesCurrentExtent(u32 currentWidth,
                                              u32 currentHeight,
                                              u32 pendingWidth,
                                              u32 pendingHeight) {
    return isValidSwapchainExtent(pendingWidth, pendingHeight) && currentWidth == pendingWidth &&
           currentHeight == pendingHeight;
}

/// Returns true when a new resize replaces an already-pending extent (coalesce candidate).
inline bool isResizeCoalesceRequest(bool resizePending,
                                    u32 pendingWidth,
                                    u32 pendingHeight,
                                    u32 requestedWidth,
                                    u32 requestedHeight) {
    return resizePending &&
           !isDuplicatePendingResizeExtent(pendingWidth, pendingHeight, requestedWidth, requestedHeight);
}

} // namespace fuse::renderer
