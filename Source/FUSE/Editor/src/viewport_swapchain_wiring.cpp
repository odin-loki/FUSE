#include <fuse/editor/viewport_swapchain_wiring.hpp>

#include <fuse/log/logger.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>
#endif

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#endif

namespace fuse::editor {

ViewportSwapchainWiringResult wireExternalSwapchainFromHandoff(fuse::renderer::RhiContext& context,
                                                               ViewportSwapchainHandoff& handoff) {
    ViewportSwapchainWiringResult result{};
    result.usedQtStubSurface = handoff.qtStubSurface;

#if defined(FUSE_VULKAN_BACKEND)
    if (handoff.nativeSurface == nullptr) {
        result.attempted = true;
        result.fellBackToHeadless = true;
        result.note = "null external surface — headless swapchain retained";
        handoff.pending = false;
        handoff.consumed = true;
        return result;
    }

    if (handoff.qtStubSurface) {
        result.attempted = true;
        result.fellBackToHeadless = true;
        result.note = handoff.handoffSource != nullptr ? handoff.handoffSource : "qt_winid_stub_not_vk_surface";
        handoff.pending = false;
        handoff.consumed = true;
        return result;
    }

    fuse::renderer::SwapchainDesc desc = handoff.swapchainDesc;
    if (desc.width == 0) {
        desc.width = handoff.width > 0 ? handoff.width : 640u;
    }
    if (desc.height == 0) {
        desc.height = handoff.height > 0 ? handoff.height : 480u;
    }
    desc.surface.kind = fuse::renderer::SurfaceKind::External;
    desc.surface.nativeSurface = handoff.nativeSurface;

    result.attempted = true;
    const bool ready = context.bootstrap().ensureSwapchain(desc);
    const fuse::renderer::VulkanSwapchain* swapchain = context.bootstrap().swapchain();
    result.swapchainReady =
        ready && swapchain != nullptr && fuse::renderer::isSwapchainPresentable(*swapchain);
    result.fellBackToHeadless = !result.swapchainReady;
    result.note = result.swapchainReady
                      ? (handoff.qtStubSurface ? "qt_stub_surface_wired" : "external_surface_wired")
                      : "external_surface_swapchain_unavailable";

    if (!result.swapchainReady) {
        fuse::log::warn("RuntimeViewportHook: external swapchain wiring failed (%s) — headless path",
                        context.bootstrap().status().message.c_str());
    }

    handoff.pending = false;
    handoff.consumed = true;
#else
    (void)context;
    result.attempted = true;
    result.fellBackToHeadless = true;
    result.note = "vulkan backend disabled";
    handoff.pending = false;
    handoff.consumed = true;
#endif

    return result;
}

#if defined(FUSE_HAS_VULKAN_RHI)
void syncHybridBootstrapFromConsumedHandoff(fuse::hybrid::HybridRendererBootstrap& hybrid,
                                            const ViewportSwapchainHandoff& handoff) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!handoff.consumed || handoff.nativeSurface == nullptr || handoff.qtStubSurface) {
        return;
    }

    fuse::renderer::RhiContext* context = hybrid.rendererBootstrap().rhiContext();
    if (context == nullptr) {
        return;
    }

    fuse::renderer::SwapchainDesc desc = handoff.swapchainDesc;
    if (desc.width == 0) {
        desc.width = handoff.width > 0 ? handoff.width : 640u;
    }
    if (desc.height == 0) {
        desc.height = handoff.height > 0 ? handoff.height : 480u;
    }
    desc.surface.kind = fuse::renderer::SurfaceKind::External;
    desc.surface.nativeSurface = handoff.nativeSurface;
    context->bootstrap().ensureSwapchain(desc);
#else
    (void)hybrid;
    (void)handoff;
#endif
}
#endif

} // namespace fuse::editor
