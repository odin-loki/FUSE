#pragma once

#include <fuse/editor/viewport_swapchain_handoff.hpp>
#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/renderer/vk/swapchain_util.hpp>
#endif

namespace fuse::editor {

/// True when editor embed may call real `vkQueuePresentKHR` (display + Qt gate + real surface).
inline bool viewportQtPresentEligible(const ViewportSwapchainHandoff& handoff) {
#if defined(FUSE_VULKAN_BACKEND)
    return fuse::renderer::desktopQtPresentRuntimeReady() && handoff.consumed && handoff.qtRealSurface &&
           !handoff.qtStubSurface && handoff.nativeSurface != nullptr;
#else
    (void)handoff;
    return false;
#endif
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

/// True when headless CI should skip Qt WSI probes (no display server).
inline bool viewportHeadlessWsiProbeSkipped() {
#if defined(FUSE_VULKAN_BACKEND)
    return !fuse::renderer::desktopQtPresentRuntimeReady();
#else
    return true;
#endif
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
    // A consumed surface alone is not enough: until a presentable swapchain is wired, the software
    // placeholder is the only thing that reaches the viewport.
    return externalSwapchainWired && handoff.nativeSurface != nullptr;
}

} // namespace fuse::editor
