#pragma once

#include <fuse/renderer/vk/frame.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

class VulkanSwapchain;

/// Why present early-outs without calling vkQueuePresentKHR (B2.2 deepen).
enum class EmptyPresentSkipReason : u8 {
    None = 0,
    NullSwapchain,
    EmptySwapchain,
    EmptyImageIndex,
    FrameManagerNotReady,
};

/// Why acquire short-circuits without calling vkAcquireNextImageKHR (B2.2 deepen).
enum class EmptyAcquireSkipReason : u8 {
    None = 0,
    NullSwapchain,
    EmptySwapchain,
};

/// Resize request disposition at queue or apply time (B2.2 deepen).
enum class ResizeRequestOutcome : u8 {
    Queued = 0,
    RejectedInvalidExtent,
    DeferredDuringPresent,
    DuplicateIgnored,
    Coalesced,
    NoOpMatchesCurrent,
};

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

/// Classify why present would early-out without calling vkQueuePresentKHR.
EmptyPresentSkipReason classifyEmptyPresentSkip(const VulkanSwapchain* swapchain,
                                                u32 imageIndex,
                                                const FrameManager* frameManager);

/// Classify why acquire would short-circuit without calling vkAcquireNextImageKHR.
EmptyAcquireSkipReason classifyEmptyAcquireSkip(const VulkanSwapchain* swapchain);

const char* emptyPresentSkipReasonLabel(EmptyPresentSkipReason reason);
const char* emptyAcquireSkipReasonLabel(EmptyAcquireSkipReason reason);
const char* resizeRequestOutcomeLabel(ResizeRequestOutcome outcome);

/// True when present should succeed without calling vkQueuePresentKHR.
inline bool shouldEarlyOutEmptyPresent(const VulkanSwapchain* swapchain,
                                       u32 imageIndex,
                                       const FrameManager* frameManager) {
    return classifyEmptyPresentSkip(swapchain, imageIndex, frameManager) != EmptyPresentSkipReason::None;
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

/// Classify whether a pending resize would be a no-op against the current extent.
inline ResizeRequestOutcome classifyResizeApplyOutcome(u32 currentWidth,
                                                       u32 currentHeight,
                                                       u32 pendingWidth,
                                                       u32 pendingHeight) {
    if (pendingResizeMatchesCurrentExtent(currentWidth, currentHeight, pendingWidth, pendingHeight)) {
        return ResizeRequestOutcome::NoOpMatchesCurrent;
    }
    return ResizeRequestOutcome::Queued;
}

} // namespace fuse::renderer
