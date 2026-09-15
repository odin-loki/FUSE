#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/renderer_bootstrap.hpp>

namespace fuse::renderer {

RendererBootstrap::RendererBootstrap(RendererBootstrapDesc desc)
    : m_desc(desc) {}

RendererBootstrap::~RendererBootstrap() {
    shutdown();
}

std::unique_ptr<RendererBootstrap> RendererBootstrap::create(const RendererBootstrapDesc& desc) {
    auto bootstrap = std::unique_ptr<RendererBootstrap>(new RendererBootstrap(desc));
    if (!bootstrap->initialize()) {
        return bootstrap;
    }
    return bootstrap;
}

bool RendererBootstrap::initialize() {
    if (m_status.initialized) {
        return true;
    }

    if (m_desc.requireCoreInitialized && !core::isInitialized()) {
        m_status.message = "fuse::core::initialize() must run before RendererBootstrap";
        return false;
    }

    if (!platform::mayTouchGpuContext()) {
        m_status.message = "RendererBootstrap must initialize on the render thread";
        return false;
    }

    m_rhiContext = RhiContext::create(m_desc.rhi);
    if (!m_rhiContext) {
        m_status.message = "RhiContext allocation failed";
        return false;
    }

    const VulkanBootstrapStatus& vkStatus = m_rhiContext->bootstrap().status();
    m_status.backendMode = vkStatus.mode;
    m_status.deviceReady = vkStatus.deviceReady;
    m_status.rhiContextReady = true;
    m_status.frameManagerReady = vkStatus.frameManagerReady;
    m_status.initialized = true;
    m_status.message = vkStatus.message;

    return true;
}

void RendererBootstrap::shutdown() {
    if (!m_status.initialized && !m_rhiContext) {
        return;
    }

    m_rhiContext.reset();
    m_status = RendererBootstrapStatus{};
}

FrameManager* RendererBootstrap::frameManager() {
    if (!m_rhiContext) {
        return nullptr;
    }
    return m_rhiContext->bootstrap().frameManager();
}

const FrameManager* RendererBootstrap::frameManager() const {
    if (!m_rhiContext) {
        return nullptr;
    }
    return m_rhiContext->bootstrap().frameManager();
}

} // namespace fuse::renderer
