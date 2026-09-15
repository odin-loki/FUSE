#pragma once

#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/raster_path.hpp>

#include <memory>

namespace fuse::renderer {

/// Headless RHI context for Track B — accepts command lists on the render thread.
class RhiContext {
public:
    struct Desc {
        VulkanBootstrapDesc bootstrap{};
        RasterPathDesc raster{};
        bool enableRasterPath = true;
    };

    static std::unique_ptr<RhiContext> create(const Desc& desc);
    ~RhiContext();

    RhiContext(const RhiContext&) = delete;
    RhiContext& operator=(const RhiContext&) = delete;

    const VulkanBootstrap& bootstrap() const { return *m_bootstrap; }
    VulkanBootstrap& bootstrap() { return *m_bootstrap; }

    const RasterPath* rasterPath() const { return m_rasterPath.get(); }

    /// Begin frame slot after tick barrier. Returns false off render thread.
    bool beginFrame(u32 frameIndex);

    /// Records commands and advances frame ring. Returns false off render thread.
    bool submitFrame(const RenderCommandList& commands, u32 frameIndex = 0);

    u32 submittedFrameCount() const { return m_submittedFrames; }
    u32 lastSubmittedCommandCount() const { return m_lastCommandCount; }
    u32 currentFrameSlot() const;
    const RasterPathStats& lastRasterStats() const { return m_lastRasterStats; }

private:
    explicit RhiContext(std::unique_ptr<VulkanBootstrap> bootstrap, const Desc& desc);

    void ensureRasterPath();

    std::unique_ptr<VulkanBootstrap> m_bootstrap;
    Desc m_desc;
    std::unique_ptr<RasterPath> m_rasterPath;
    RasterPathStats m_lastRasterStats{};
    u32 m_submittedFrames = 0;
    u32 m_lastCommandCount = 0;
};

} // namespace fuse::renderer
