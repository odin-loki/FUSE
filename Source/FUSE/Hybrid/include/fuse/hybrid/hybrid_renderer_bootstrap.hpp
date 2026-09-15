#pragma once

#if defined(FUSE_HAS_VULKAN_RHI)

#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/hybrid/project_flags.hpp>
#include <fuse/hybrid/vulkan_presentable.hpp>
#include <fuse/renderer/renderer_bootstrap.hpp>
#include <fuse/renderer/vk/present_path.hpp>

#include <memory>

namespace fuse::hybrid {

/// B2.10 — runtime/demo glue: RendererBootstrap + HybridComposer on one init path.
struct HybridRendererBootstrapDesc {
    renderer::RendererBootstrapDesc renderer{};
    VulkanPresentableDesc presentable{};
    DimensionFlags projectFlags{};
};

struct HybridRendererBootstrapStatus {
    bool initialized = false;
    bool rendererReady = false;
    bool composerAttached = false;
    bool presentableReady = false;
    bool presentableSurface = false;
    std::string message;
};

class HybridRendererBootstrap {
public:
    static std::unique_ptr<HybridRendererBootstrap> create(const HybridRendererBootstrapDesc& desc = {});
    ~HybridRendererBootstrap();

    HybridRendererBootstrap(const HybridRendererBootstrap&) = delete;
    HybridRendererBootstrap& operator=(const HybridRendererBootstrap&) = delete;

    const HybridRendererBootstrapStatus& status() const { return m_status; }
    bool isReady() const { return m_status.initialized; }

    renderer::RendererBootstrap& rendererBootstrap() { return *m_rendererBootstrap; }
    const renderer::RendererBootstrap& rendererBootstrap() const { return *m_rendererBootstrap; }

    HybridComposer& composer() { return m_composer; }
    const HybridComposer& composer() const { return m_composer; }

    VulkanPresentable* presentable() { return m_presentable.get(); }
    const VulkanPresentable* presentable() const { return m_presentable.get(); }

    renderer::PresentPath* presentPath() { return m_presentPath.get(); }
    const renderer::PresentPath* presentPath() const { return m_presentPath.get(); }

    /// Tick → render on the registered render thread (main loop glue).
    void runFrame(frame::FrameCtx& ctx);

    void tick(frame::FrameCtx& ctx);
    void render(frame::FrameCtx& ctx);

    /// Idempotent tear-down: detach composer, then destroy renderer stack.
    void shutdown();

private:
    explicit HybridRendererBootstrap(HybridRendererBootstrapDesc desc);

    bool initialize();

    HybridRendererBootstrapDesc m_desc;
    HybridRendererBootstrapStatus m_status;
    std::unique_ptr<VulkanPresentable> m_presentable;
    std::unique_ptr<renderer::PresentPath> m_presentPath;
    std::unique_ptr<renderer::RendererBootstrap> m_rendererBootstrap;
    HybridComposer m_composer;
};

} // namespace fuse::hybrid

#endif // defined(FUSE_HAS_VULKAN_RHI)
