#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/rhi_context.hpp>

namespace fuse::renderer {

RhiContext::RhiContext(std::unique_ptr<VulkanBootstrap> bootstrap)
    : m_bootstrap(std::move(bootstrap)) {}

RhiContext::~RhiContext() = default;

std::unique_ptr<RhiContext> RhiContext::create(const Desc& desc) {
    auto bootstrap = VulkanBootstrap::create(desc.bootstrap);
    if (!bootstrap) {
        return nullptr;
    }

    return std::unique_ptr<RhiContext>(new RhiContext(std::move(bootstrap)));
}

bool RhiContext::submitFrame(const RenderCommandList& commands) {
    if (!platform::requireGpuContextThread()) {
        return false;
    }

    if (!m_bootstrap || !m_bootstrap->status().deviceReady) {
        return false;
    }

    m_lastCommandCount = commands.commandCount();
    ++m_submittedFrames;
    return true;
}

} // namespace fuse::renderer
