#include <fuse/editor/viewport_framebuffer.hpp>

#if defined(FUSE_EDITOR_HAS_RHI)
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/raster_path.hpp>
#endif

namespace fuse::editor {

void ViewportFramebuffer::attach(renderer::RasterPath* rasterPath, renderer::VulkanDevice* device) {
#if defined(FUSE_EDITOR_HAS_RHI)
    m_rasterPath = rasterPath;
    m_device = device;
#else
    (void)rasterPath;
    (void)device;
    m_rasterPath = nullptr;
    m_device = nullptr;
#endif
    // Force the next sync to size the (new) targets to the panel.
    m_width = 0;
    m_height = 0;
}

bool ViewportFramebuffer::sync(ViewportPanel& panel) {
    const u32 width = panel.width();
    const u32 height = panel.height();
    if (!panel.needsResize() && width == m_width && height == m_height) {
        return true;
    }

#if defined(FUSE_EDITOR_HAS_RHI)
    if (m_rasterPath != nullptr) {
        if (m_device != nullptr && m_device->isValid()) {
            m_device->waitIdle(); // old targets may still be referenced by a submitted frame
        }
        if (!m_rasterPath->resize(width, height)) {
            ++m_failedRebuildCount;
            return false;
        }
    }
#endif

    if (width != m_width || height != m_height) {
        ++m_rebuildCount;
    }
    m_width = width;
    m_height = height;
    m_builtGeneration = panel.resizeGeneration();
    panel.clearResizeFlag();
    return true;
}

} // namespace fuse::editor
