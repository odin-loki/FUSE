#pragma once

#include <fuse/editor/viewport_swapchain_handoff.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

namespace fuse::editor {

/// True when editor embed may call real `vkQueuePresentKHR` (display + Qt gate + real surface).
inline bool viewportQtPresentEligible(const ViewportSwapchainHandoff& handoff) {
    return fuse::renderer::desktopQtPresentRuntimeReady() && handoff.consumed && handoff.qtRealSurface &&
           !handoff.qtStubSurface && handoff.nativeSurface != nullptr;
}

/// Scoped PlaceholderRenderer retirement — RHI mirror remains; software RGBA skipped when safe.
inline bool shouldDisableSoftwarePlaceholderForEmbed(const ViewportSwapchainHandoff& handoff,
                                                     bool externalSwapchainWired) {
    if (!handoff.consumed || handoff.qtStubSurface) {
        return false;
    }
    if (viewportQtPresentEligible(handoff)) {
        return true;
    }
    if (externalSwapchainWired && handoff.qtRealSurface) {
        return true;
    }
    return handoff.nativeSurface != nullptr && !handoff.qtStubSurface;
}

} // namespace fuse::editor
