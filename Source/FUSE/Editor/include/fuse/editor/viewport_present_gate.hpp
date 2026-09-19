#pragma once

#include <fuse/editor/viewport_swapchain_handoff.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

namespace fuse::editor {

/// True when editor embed may call real `vkQueuePresentKHR` (display + Qt gate + real surface).
inline bool viewportQtPresentEligible(const ViewportSwapchainHandoff& handoff) {
    return fuse::renderer::desktopQtPresentRuntimeReady() && handoff.consumed && handoff.qtRealSurface &&
           !handoff.qtStubSurface && handoff.nativeSurface != nullptr;
}

/// Preconditions satisfied for Qt display present (gate may still be OFF on headless CI).
inline bool viewportQtPresentPathReady(const ViewportSwapchainHandoff& handoff, bool swapchainPresentable) {
    return handoff.consumed && handoff.qtRealSurface && !handoff.qtStubSurface &&
           handoff.nativeSurface != nullptr && swapchainPresentable;
}

/// Full Qt present eligibility including compile-time gate, display server, and wired swapchain.
inline bool viewportQtPresentPathEligible(const ViewportSwapchainHandoff& handoff,
                                          bool swapchainPresentable) {
    return viewportQtPresentPathReady(handoff, swapchainPresentable) && viewportQtPresentEligible(handoff);
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
    if (viewportQtPresentPathReady(handoff, externalSwapchainWired)) {
        return true;
    }
    if (externalSwapchainWired && handoff.qtRealSurface) {
        return true;
    }
    return handoff.nativeSurface != nullptr;
}

} // namespace fuse::editor
