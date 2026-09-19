#include <fuse/editor/viewport_vulkan_surface.hpp>

namespace fuse::editor {

ViewportVulkanSurfaceResult createViewportVulkanSurfaceFromWinId(u64 winId, u32 /*width*/,
                                                                 u32 /*height*/) {
    ViewportVulkanSurfaceResult result{};
    if (winId == 0u) {
        result.message = "invalid Qt winId";
        return result;
    }

#if defined(FUSE_HAS_QT_VULKAN)
    // Real QVulkanInstance path — compiled when Qt6 Gui is linked into fuse_editor_api.
    extern ViewportVulkanSurfaceResult createViewportVulkanSurfaceFromWinIdQt(u64 winId, u32 width,
                                                                                u32 height);
    return createViewportVulkanSurfaceFromWinIdQt(winId, width, height);
#else
    result.vkSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(winId));
    result.valid = true;
    result.stubPath = true;
    result.message = "Qt Vulkan unavailable — winId stub handoff";
    return result;
#endif
}

void destroyViewportVulkanSurface(ViewportVulkanSurfaceResult& result) {
#if defined(FUSE_HAS_QT_VULKAN)
    extern void destroyViewportVulkanSurfaceQt(ViewportVulkanSurfaceResult& result);
    destroyViewportVulkanSurfaceQt(result);
#else
    (void)result;
#endif
    result = {};
}

} // namespace fuse::editor
