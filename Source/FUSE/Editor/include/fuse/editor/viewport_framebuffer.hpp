#pragma once

#include <fuse/editor/viewport_panel.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class RasterPath;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::editor {

/// Keeps the renderer's viewport framebuffer (offscreen colour + depth targets and VkFramebuffer
/// of a `renderer::RasterPath`) sized to the `ViewportPanel` (B6.3). On a panel resize, `sync`
/// drains the GPU, rebuilds the targets at the new size and clears the panel's resize flag.
/// Without an attached raster path (no Vulkan build / no device) it tracks the size headlessly.
class ViewportFramebuffer {
public:
    /// `device` is used to drain in-flight work before the old targets are destroyed; it may be
    /// null when the caller guarantees the targets are idle.
    void attach(renderer::RasterPath* rasterPath, renderer::VulkanDevice* device);
    void detach() { attach(nullptr, nullptr); }

    /// Rebuild when `panel.needsResize()`; returns false only when a GPU rebuild failed (the
    /// flag then stays raised so the next frame retries).
    bool sync(ViewportPanel& panel);

    [[nodiscard]] bool gpuBacked() const { return m_rasterPath != nullptr; }
    [[nodiscard]] u32 width() const { return m_width; }
    [[nodiscard]] u32 height() const { return m_height; }
    [[nodiscard]] u32 rebuildCount() const { return m_rebuildCount; }
    [[nodiscard]] u32 failedRebuildCount() const { return m_failedRebuildCount; }
    /// `ViewportPanel::resizeGeneration()` the current targets were built for.
    [[nodiscard]] u32 builtGeneration() const { return m_builtGeneration; }

private:
    renderer::RasterPath* m_rasterPath = nullptr;
    renderer::VulkanDevice* m_device = nullptr;
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_rebuildCount = 0;
    u32 m_failedRebuildCount = 0;
    u32 m_builtGeneration = 0;
};

} // namespace fuse::editor
