#pragma once

#include <fuse/types.hpp>

namespace fuse::editor {

/// Result of a winId → Vulkan surface bootstrap (WP-06i; the real path belonged to the former Qt shell).
/// Headless CI uses `stubPath=true` with opaque winId as `vkSurface`.
struct ViewportVulkanSurfaceResult {
    void* vkInstance = nullptr;
    void* vkSurface = nullptr;
    bool valid = false;
    bool stubPath = true;
    const char* message = nullptr;
};

/// Returns an honest winId stub (WP-06h compatibility). The FUSE UI editor presents its viewport as a zero-copy
/// external texture, so there is no toolkit surface to create.
[[nodiscard]] ViewportVulkanSurfaceResult createViewportVulkanSurfaceFromWinId(u64 winId, u32 width,
                                                                               u32 height);

/// Release any resources owned by a prior bootstrap (no-op for winId stubs).
void destroyViewportVulkanSurface(ViewportVulkanSurfaceResult& result);

/// Headless-safe embed bootstrap + teardown stress (WP-06j).
struct ViewportVulkanBootstrapStressResult {
    u32 cyclesAttempted = 0;
    u32 cyclesCompleted = 0;
    u32 stubPathCycles = 0;
    u32 realSurfaceCycles = 0;
};

[[nodiscard]] ViewportVulkanBootstrapStressResult stressViewportVulkanBootstrapTeardown(u32 cycles);

/// `QVulkanWindow` WSI probe of the former Qt shell (wave 9): always reports `headlessSkipped` now that the build has
/// no Qt; kept so the embed session keeps its probe counters.
struct QVulkanWindowWsiProbeResult {
    bool attempted = false;
    bool instanceReady = false;
    bool extensionsProbed = false;
    bool surfaceReady = false;
    bool headlessSkipped = false;
    bool windowCreated = false;
    bool windowDestroyed = false;
    u32 supportedExtensionCount = 0;
    u32 instanceVersionMajor = 0;
    u32 instanceVersionMinor = 0;
    const char* note = nullptr;
};

[[nodiscard]] QVulkanWindowWsiProbeResult probeQVulkanWindowWsi();

} // namespace fuse::editor
