#include <fuse/editor/viewport_vulkan_surface.hpp>

namespace fuse::editor {

ViewportVulkanSurfaceResult createViewportVulkanSurfaceFromWinId(u64 winId, [[maybe_unused]] u32 width,
                                                                 [[maybe_unused]] u32 height) {
    ViewportVulkanSurfaceResult result{};
    if (winId == 0u) {
        result.message = "invalid winId";
        return result;
    }

    // The former Qt shell created a real VkSurfaceKHR here through QVulkanInstance. The FUSE UI shell presents the
    // viewport as a zero-copy external texture (EditorShell/SceneHost), so a winId only ever takes the stub handoff.
    result.vkSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(winId));
    result.valid = true;
    result.stubPath = true;
    result.message = "winId stub handoff";
    return result;
}

void destroyViewportVulkanSurface(ViewportVulkanSurfaceResult& result) {
    result = {};
}

QVulkanWindowWsiProbeResult probeQVulkanWindowWsi() {
    QVulkanWindowWsiProbeResult result{};
    result.headlessSkipped = true;
    result.note = "qt_vulkan_module_unavailable"; // no Qt in the build since the Stage 3 switch
    return result;
}

ViewportVulkanBootstrapStressResult stressViewportVulkanBootstrapTeardown(u32 cycles) {
    ViewportVulkanBootstrapStressResult stress{};
    if (cycles == 0u) {
        return stress;
    }

    for (u32 i = 0; i < cycles; ++i) {
        ++stress.cyclesAttempted;
        const u64 winId = static_cast<u64>(5000u + i);
        ViewportVulkanSurfaceResult surface =
            createViewportVulkanSurfaceFromWinId(winId, 640u, 360u);
        if (!surface.valid) {
            continue;
        }

        if (surface.stubPath) {
            ++stress.stubPathCycles;
        } else {
            ++stress.realSurfaceCycles;
        }

        destroyViewportVulkanSurface(surface);
        if (!surface.valid) {
            ++stress.cyclesCompleted;
        }
    }

    return stress;
}

} // namespace fuse::editor
