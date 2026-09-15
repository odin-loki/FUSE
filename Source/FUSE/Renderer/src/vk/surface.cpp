#include <fuse/renderer/vk/surface.hpp>

namespace fuse::renderer {

VulkanSurface VulkanSurface::fromDesc(const SurfaceDesc& desc) {
    VulkanSurface surface;
    surface.m_info.kind = desc.kind;

    if (desc.kind == SurfaceKind::Headless) {
        surface.m_info.valid = true;
        surface.m_info.message = "Headless surface — swapchain uses stub path (no VkSurfaceKHR)";
        return surface;
    }

    if (desc.nativeSurface == nullptr) {
        surface.m_info.valid = false;
        surface.m_info.message = "External surface kind requires nativeSurface handle";
        return surface;
    }

    surface.m_nativeSurface = desc.nativeSurface;
    surface.m_info.valid = true;
    surface.m_info.message = "External VkSurfaceKHR registered";
    return surface;
}

} // namespace fuse::renderer
