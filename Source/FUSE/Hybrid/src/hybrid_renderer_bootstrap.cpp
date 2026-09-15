#if defined(FUSE_HAS_VULKAN_RHI)

#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>

namespace fuse::hybrid {

HybridRendererBootstrap::HybridRendererBootstrap(HybridRendererBootstrapDesc desc)
    : m_desc(desc) {}

HybridRendererBootstrap::~HybridRendererBootstrap() {
    shutdown();
}

std::unique_ptr<HybridRendererBootstrap> HybridRendererBootstrap::create(
    const HybridRendererBootstrapDesc& desc) {
    auto runtime = std::unique_ptr<HybridRendererBootstrap>(new HybridRendererBootstrap(desc));
    if (!runtime->initialize()) {
        return runtime;
    }
    return runtime;
}

bool HybridRendererBootstrap::initialize() {
    if (m_status.initialized) {
        return true;
    }

    m_rendererBootstrap = renderer::RendererBootstrap::create(m_desc.renderer);
    if (!m_rendererBootstrap || !m_rendererBootstrap->isReady()) {
        m_status.message = m_rendererBootstrap
                               ? m_rendererBootstrap->status().message
                               : "RendererBootstrap allocation failed";
        return false;
    }

    m_composer.setProjectFlags(m_desc.projectFlags);
    m_composer.setSharedRhiContext(m_rendererBootstrap->rhiContext());
    m_status.rendererReady = true;
    m_status.composerAttached = true;
    m_status.initialized = true;
    m_status.message = m_rendererBootstrap->status().message;
    return true;
}

void HybridRendererBootstrap::shutdown() {
    if (!m_status.initialized && !m_rendererBootstrap) {
        return;
    }

    m_composer.setSharedRhiContext(nullptr);
    m_rendererBootstrap.reset();
    m_status = HybridRendererBootstrapStatus{};
}

void HybridRendererBootstrap::tick(frame::FrameCtx& ctx) {
    m_composer.tick(ctx);
}

void HybridRendererBootstrap::render(frame::FrameCtx& ctx) {
    m_composer.render(ctx);
}

void HybridRendererBootstrap::runFrame(frame::FrameCtx& ctx) {
    tick(ctx);
    render(ctx);
}

} // namespace fuse::hybrid

#endif // defined(FUSE_HAS_VULKAN_RHI)
