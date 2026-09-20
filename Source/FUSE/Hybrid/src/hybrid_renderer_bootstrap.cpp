#if defined(FUSE_HAS_VULKAN_RHI)

#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>

#include <fuse/platform/window_wsi.hpp>
#include <fuse/renderer/vk/fence_wait.hpp>

#include <cstdint>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

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
    renderer::SwapchainDesc swapDesc = m_presentable->swapchainDesc();
    rendererDesc.rhi.bootstrap.swapchain = swapDesc;

    const std::vector<const char*>& wsiExtensions = m_presentable->requiredInstanceExtensions();
    if (!wsiExtensions.empty()) {
        rendererDesc.rhi.bootstrap.instance.extraExtensions = wsiExtensions.data();
        rendererDesc.rhi.bootstrap.instance.extraExtensionCount =
            static_cast<u32>(wsiExtensions.size());
    }

    const bool gameWindowBackend =
        m_desc.presentable.backend == PresentableBackend::GameWindow;
    const renderer::SurfaceDesc presentableSurface = m_presentable->surfaceDesc();
    const bool deferSwapchain =
        gameWindowBackend && presentableSurface.kind == renderer::SurfaceKind::Headless;
    if (deferSwapchain) {
        rendererDesc.rhi.bootstrap.createSwapchain = false;
    }

    // Device is created inside RendererBootstrap before deferSwapchain can call
    // createVulkanSurface. Enable swapchain on GameWindow+WSI now; Headless CI stays off.
    renderer::VulkanDeviceDesc& deviceDesc = rendererDesc.rhi.bootstrap.device;
    if (gameWindowBackend && platform::windowWsiAvailable()) {
        deviceDesc.requirePresentation = true;
    }
    if (presentableSurface.kind == renderer::SurfaceKind::External) {
        deviceDesc.requirePresentation = true;
        deviceDesc.presentSurface = presentableSurface.nativeSurface;
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

    renderer::PresentPathDesc presentDesc{};
    presentDesc.vsyncMode = m_presentable->vsyncMode();
    m_presentPath = renderer::PresentPath::create(m_rendererBootstrap->rhiContext()->bootstrap(), presentDesc);

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

    if (m_rendererBootstrap != nullptr) {
        fuse::renderer::FrameManager* frameManager = m_rendererBootstrap->frameManager();
        if (frameManager != nullptr && frameManager->isReady()) {
            waitAllInFlightFences(*frameManager);
        }

#if defined(FUSE_VULKAN_BACKEND)
        fuse::renderer::VulkanDevice* device =
            m_rendererBootstrap->rhiContext()->bootstrap().device();
        if (device != nullptr && device->isValid()) {
            vkDeviceWaitIdle(static_cast<VkDevice>(device->nativeHandle()));
        }
#endif
    }

    m_composer.setSharedRhiContext(nullptr);
    m_presentPath.reset();
    m_rendererBootstrap.reset();
    m_presentable.reset();
    m_status = HybridRendererBootstrapStatus{};
}

void HybridRendererBootstrap::tick(frame::FrameCtx& ctx) {
    m_composer.tick(ctx);
}

void HybridRendererBootstrap::render(frame::FrameCtx& ctx) {
    if (m_presentable != nullptr && m_presentPath != nullptr && m_presentable->needsResizeRecreate()) {
        const VulkanPresentableStatus& presentableStatus = m_presentable->status();
        m_presentPath->requestResize(presentableStatus.pendingResizeWidth,
                                     presentableStatus.pendingResizeHeight);
    }

    // B2.2 / WP-06c — fence wait → acquire → RHI record + vkQueueSubmit → present (WSI or headless sink).
    u32 acquiredImageIndex = UINT32_MAX;
    if (m_presentPath != nullptr) {
        if (!m_presentPath->waitInFlightFence()) {
            return;
        }
        acquiredImageIndex = m_presentPath->acquireImage();
    }

    if (m_rendererBootstrap != nullptr && m_rendererBootstrap->rhiContext() != nullptr) {
        m_rendererBootstrap->rhiContext()->setAcquiredSwapchainImage(acquiredImageIndex);
    }

    m_composer.render(ctx);

    if (m_presentPath != nullptr) {
        if (m_rendererBootstrap != nullptr && m_rendererBootstrap->rhiContext() != nullptr) {
            const renderer::GraphicsQueueSubmitResult& submit =
                m_rendererBootstrap->rhiContext()->lastQueueSubmit();
            m_presentPath->noteQueueSubmit(submit.ok, submit.submitted, submit.headless);
        }
        m_presentPath->markReadyToPresent();
        m_presentPath->presentImage();
    }
}

void HybridRendererBootstrap::runFrame(frame::FrameCtx& ctx) {
    tick(ctx);
    render(ctx);
}

} // namespace fuse::hybrid

#endif // defined(FUSE_HAS_VULKAN_RHI)
