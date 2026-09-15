#pragma once

#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <memory>

namespace fuse::renderer {

/// Headless RHI context for Track B bootstrap — accepts command lists on the render thread.
class RhiContext {
public:
    struct Desc {
        VulkanBootstrapDesc bootstrap{};
    };

    static std::unique_ptr<RhiContext> create(const Desc& desc);
    ~RhiContext();

    RhiContext(const RhiContext&) = delete;
    RhiContext& operator=(const RhiContext&) = delete;

    const VulkanBootstrap& bootstrap() const { return *m_bootstrap; }
    VulkanBootstrap& bootstrap() { return *m_bootstrap; }

    /// Records commands for a future B2.2 swapchain submit. Returns false off render thread.
    bool submitFrame(const RenderCommandList& commands);

    u32 submittedFrameCount() const { return m_submittedFrames; }
    u32 lastSubmittedCommandCount() const { return m_lastCommandCount; }

private:
    explicit RhiContext(std::unique_ptr<VulkanBootstrap> bootstrap);

    std::unique_ptr<VulkanBootstrap> m_bootstrap;
    u32 m_submittedFrames = 0;
    u32 m_lastCommandCount = 0;
};

} // namespace fuse::renderer
