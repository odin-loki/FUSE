#pragma once

#include <fuse/editor/viewport_swapchain_handoff.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class RhiContext;
}

namespace fuse::editor {

/// Result of attempting to wire an editor viewport surface into Track B swapchain bootstrap.
struct ViewportSwapchainWiringResult {
    bool attempted = false;
    bool swapchainReady = false;
    bool usedQtStubSurface = false;
    bool fellBackToHeadless = false;
    const char* note = nullptr;
};

/// Consumes a pending `ViewportSwapchainHandoff` and calls `VulkanBootstrap::ensureSwapchain`.
/// Headless-safe: invalid/null surfaces fall back without crashing.
ViewportSwapchainWiringResult wireExternalSwapchainFromHandoff(fuse::renderer::RhiContext& context,
                                                               ViewportSwapchainHandoff& handoff);

} // namespace fuse::editor
