#pragma once

#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/frame.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

/// B2.10 — single init path for fuse_rhi (VulkanBootstrap + FrameManager + RhiContext).
struct RendererBootstrapDesc {
    RhiContext::Desc rhi{};
    /// When true, initialize() fails if fuse::core::initialize() was not called first.
    bool requireCoreInitialized = true;
};

struct RendererBootstrapStatus {
    bool initialized = false;
    bool rhiContextReady = false;
    bool frameManagerReady = false;
    bool deviceReady = false;
    VulkanBackendMode backendMode = VulkanBackendMode::Stub;
    std::string message;
};

class RendererBootstrap {
public:
    static std::unique_ptr<RendererBootstrap> create(const RendererBootstrapDesc& desc = {});
    ~RendererBootstrap();

    RendererBootstrap(const RendererBootstrap&) = delete;
    RendererBootstrap& operator=(const RendererBootstrap&) = delete;

    const RendererBootstrapStatus& status() const { return m_status; }
    bool isReady() const { return m_status.initialized; }

    RhiContext* rhiContext() { return m_rhiContext.get(); }
    const RhiContext* rhiContext() const { return m_rhiContext.get(); }

    FrameManager* frameManager();
    const FrameManager* frameManager() const;

    /// Idempotent tear-down: destroys RhiContext (FrameManager via VulkanBootstrap) in reverse init order.
    void shutdown();

private:
    explicit RendererBootstrap(RendererBootstrapDesc desc);

    bool initialize();

    RendererBootstrapDesc m_desc;
    RendererBootstrapStatus m_status;
    std::unique_ptr<RhiContext> m_rhiContext;
};

} // namespace fuse::renderer
