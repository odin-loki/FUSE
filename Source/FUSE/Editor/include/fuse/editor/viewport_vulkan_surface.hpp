#pragma once

#include <fuse/types.hpp>

namespace fuse::editor {

/// Result of attempting Qt → Vulkan surface bootstrap (WP-06i).
/// Headless CI uses `stubPath=true` with opaque winId as `vkSurface`.
struct ViewportVulkanSurfaceResult {
    void* vkInstance = nullptr;
    void* vkSurface = nullptr;
    bool valid = false;
    bool stubPath = true;
    const char* message = nullptr;
};

/// Create a viewport `VkSurfaceKHR` from a Qt widget winId when Qt Vulkan is available;
/// otherwise returns an honest winId stub (WP-06h compatibility).
[[nodiscard]] ViewportVulkanSurfaceResult createViewportVulkanSurfaceFromWinId(u64 winId, u32 width,
                                                                               u32 height);

/// Release any resources owned by a prior bootstrap (no-op for winId stubs).
void destroyViewportVulkanSurface(ViewportVulkanSurfaceResult& result);

/// Headless-safe Qt embed bootstrap + teardown stress (WP-06j).
struct ViewportVulkanBootstrapStressResult {
    u32 cyclesAttempted = 0;
    u32 cyclesCompleted = 0;
    u32 stubPathCycles = 0;
    u32 realSurfaceCycles = 0;
};

[[nodiscard]] ViewportVulkanBootstrapStressResult stressViewportVulkanBootstrapTeardown(u32 cycles);

/// Headless-safe `QVulkanWindow` WSI probe (wave 9). Skips when no display server.
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
