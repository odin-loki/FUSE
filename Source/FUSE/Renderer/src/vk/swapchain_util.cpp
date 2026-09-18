#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

namespace fuse::renderer {

bool isSwapchainPresentable(const VulkanSwapchain& swapchain) {
    return swapchain.isReady() && swapchain.nativeHandle() != nullptr && swapchain.hasImages();
}

bool isSwapchainEmpty(const VulkanSwapchain& swapchain) {
    return !swapchain.isReady() || swapchain.nativeHandle() == nullptr || !swapchain.hasImages();
}

EmptyPresentSkipReason classifyEmptyPresentSkip(const VulkanSwapchain* swapchain,
                                                u32 imageIndex,
                                                const FrameManager* frameManager) {
    if (swapchain == nullptr) {
        return EmptyPresentSkipReason::NullSwapchain;
    }
    if (isSwapchainEmpty(*swapchain)) {
        return EmptyPresentSkipReason::EmptySwapchain;
    }
    if (isEmptyAcquireResult(imageIndex)) {
        return EmptyPresentSkipReason::EmptyImageIndex;
    }
    if (frameManager == nullptr || !frameManager->isReady()) {
        return EmptyPresentSkipReason::FrameManagerNotReady;
    }
    return EmptyPresentSkipReason::None;
}

EmptyAcquireSkipReason classifyEmptyAcquireSkip(const VulkanSwapchain* swapchain) {
    if (swapchain == nullptr) {
        return EmptyAcquireSkipReason::NullSwapchain;
    }
    if (isSwapchainEmpty(*swapchain)) {
        return EmptyAcquireSkipReason::EmptySwapchain;
    }
    return EmptyAcquireSkipReason::None;
}

const char* emptyPresentSkipReasonLabel(EmptyPresentSkipReason reason) {
    switch (reason) {
    case EmptyPresentSkipReason::None:
        return "none";
    case EmptyPresentSkipReason::NullSwapchain:
        return "null_swapchain";
    case EmptyPresentSkipReason::EmptySwapchain:
        return "empty_swapchain";
    case EmptyPresentSkipReason::EmptyImageIndex:
        return "empty_image_index";
    case EmptyPresentSkipReason::FrameManagerNotReady:
        return "frame_manager_not_ready";
    }
    return "unknown";
}

const char* emptyAcquireSkipReasonLabel(EmptyAcquireSkipReason reason) {
    switch (reason) {
    case EmptyAcquireSkipReason::None:
        return "none";
    case EmptyAcquireSkipReason::NullSwapchain:
        return "null_swapchain";
    case EmptyAcquireSkipReason::EmptySwapchain:
        return "empty_swapchain";
    }
    return "unknown";
}

const char* resizeRequestOutcomeLabel(ResizeRequestOutcome outcome) {
    switch (outcome) {
    case ResizeRequestOutcome::Queued:
        return "queued";
    case ResizeRequestOutcome::RejectedInvalidExtent:
        return "rejected_invalid_extent";
    case ResizeRequestOutcome::DeferredDuringPresent:
        return "deferred_during_present";
    case ResizeRequestOutcome::DuplicateIgnored:
        return "duplicate_ignored";
    case ResizeRequestOutcome::Coalesced:
        return "coalesced";
    case ResizeRequestOutcome::NoOpMatchesCurrent:
        return "noop_matches_current";
    }
    return "unknown";
}

} // namespace fuse::renderer
