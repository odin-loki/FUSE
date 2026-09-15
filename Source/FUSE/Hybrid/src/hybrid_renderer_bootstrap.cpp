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

    m_presentable = VulkanPresentable::create(m_desc.presentable);
    if (!m_presentable) {
        m_status.message = "VulkanPresentable allocation failed";
        return false;
    }
    m_status.presentableReady = true;

    renderer::RendererBootstrapDesc rendererDesc = m_desc.renderer;
    renderer::SwapchainDesc& swapDesc = rendererDesc.rhi.bootstrap.swapchain;
    swapDesc.width = m_presentable->swapchainWidth();
    swapDesc.height = m_presentable->swapchainHeight();
    swapDesc.surface = m_presentable->surfaceDesc();

    const std::vector<const char*>& wsiExtensions = m_presentable->requiredInstanceExtensions();
    if (!wsiExtensions.empty()) {
        rendererDesc.rhi.bootstrap.instance.extraExtensions = wsiExtensions.data();
        rendererDesc.rhi.bootstrap.instance.extraExtensionCount =
            static_cast<u32>(wsiExtensions.size());
    }

    const bool deferSwapchain =
        m_desc.presentable.backend == PresentableBackend::GameWindow &&
        m_presentable->surfaceDesc().kind == renderer::SurfaceKind::Headless;
    if (deferSwapchain) {
        rendererDesc.rhi.bootstrap.createSwapchain = false;
    }

    m_rendererBootstrap = renderer::RendererBootstrap::create(rendererDesc);
    if (!m_rendererBootstrap || !m_rendererBootstrap->isReady()) {
        m_status.message = m_rendererBootstrap ? m_rendererBootstrap->status().message
                                               : "RendererBootstrap allocation failed";
        return false;
    }

    if (deferSwapchain) {
        renderer::VulkanInstance* instance = m_rendererBootstrap->rhiContext()->bootstrap().instance();
        if (instance != nullptr && instance->isValid()) {
            if (m_presentable->createVulkanSurface(instance->nativeHandle())) {
                swapDesc.surface = m_presentable->surfaceDesc();
                swapDesc.width = m_presentable->swapchainWidth();
                swapDesc.height = m_presentable->swapchainHeight();
                m_rendererBootstrap->rhiContext()->bootstrap().ensureSwapchain(swapDesc);
                m_status.presentableSurface = m_presentable->status().presentable;
            }
        }
    } else if (m_presentable->vulkanSurface().isPresentable()) {
        m_status.presentableSurface = true;
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
    m_presentable.reset();
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
