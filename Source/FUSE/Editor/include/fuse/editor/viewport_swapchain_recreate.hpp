#pragma once

#include <fuse/types.hpp>

#include <memory>

namespace fuse::renderer {
class PresentPath;
class RhiContext;
} // namespace fuse::renderer

namespace fuse::editor {

/// Result of a headless-safe viewport swapchain resize / recreate toward U6 real display.
struct ViewportSwapchainRecreateResult {
    bool attempted = false;
    bool recreated = false;
    bool deferred = false;
    bool headlessHonest = false;
    u32 width = 0;
    u32 height = 0;
    u32 swapchainRecreateCount = 0;
    const char* note = nullptr;
};

/// Drain in-flight GPU work before viewport `RhiContext` teardown (Lavapipe flake reduction).
void drainViewportGpuContext(fuse::renderer::RhiContext& context);

/// Queue swapchain dimensions for recreate on the viewport present-path stub.
[[nodiscard]] ViewportSwapchainRecreateResult requestViewportSwapchainRecreate(
    fuse::renderer::RhiContext& context, std::unique_ptr<fuse::renderer::PresentPath>& presentPath,
    u32 width, u32 height);

/// Apply a queued viewport resize immediately (fence-waits first). Headless-honest on CI.
[[nodiscard]] ViewportSwapchainRecreateResult applyViewportPendingSwapchainRecreate(
    fuse::renderer::RhiContext& context, std::unique_ptr<fuse::renderer::PresentPath>& presentPath);

} // namespace fuse::editor
