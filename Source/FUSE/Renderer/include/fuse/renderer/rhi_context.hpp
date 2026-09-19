#pragma once

#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/composite_pass.hpp>
#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/queue_submit.hpp>
#include <fuse/renderer/vk/composite_gpu_path.hpp>
#include <fuse/renderer/vk/raster_path.hpp>
#include <fuse/renderer/cuda/vk_sync.hpp>

#include <cstdint>
#include <memory>

namespace fuse::renderer {

/// Headless RHI context for Track B — accepts command lists on the render thread.
class RhiContext {
public:
    struct Desc {
        VulkanBootstrapDesc bootstrap{};
        RasterPathDesc raster{};
        CompositePassDesc composite{};
        bool enableRasterPath = true;
        bool enableCompositePass = true;
    };

    static std::unique_ptr<RhiContext> create(const Desc& desc);
    ~RhiContext();

    RhiContext(const RhiContext&) = delete;
    RhiContext& operator=(const RhiContext&) = delete;

    const VulkanBootstrap& bootstrap() const { return *m_bootstrap; }
    VulkanBootstrap& bootstrap() { return *m_bootstrap; }

    const RasterPath* rasterPath() const { return m_rasterPath.get(); }
    const CompositePass* compositePass() const { return m_compositePass.get(); }
    const CompositeGpuPath* compositeGpuPath() const { return m_compositeGpuPath.get(); }

    /// Begin frame slot after tick barrier. Returns false off render thread.
    bool beginFrame(u32 frameIndex);

    /// Records commands, issues `vkQueueSubmit` on the frame slot, advances frame ring.
    /// Returns false off render thread.
    bool submitFrame(const RenderCommandList& commands, u32 frameIndex = 0);

    /// Swapchain image from `PresentPath::acquireImage` — UINT32_MAX on headless CI.
    void setAcquiredSwapchainImage(u32 imageIndex) { m_acquiredSwapchainImage = imageIndex; }
    u32 acquiredSwapchainImage() const { return m_acquiredSwapchainImage; }

    u32 submittedFrameCount() const { return m_submittedFrames; }
    u32 queueSubmitCount() const { return m_queueSubmitCount; }
    bool lastQueueSubmitOk() const { return m_lastQueueSubmit.ok; }
    const GraphicsQueueSubmitResult& lastQueueSubmit() const { return m_lastQueueSubmit; }
    u32 lastSubmittedCommandCount() const { return m_lastCommandCount; }
    u32 lastGraphPassCount() const { return m_lastGraphPassCount; }
    u32 lastGraphBarrierCount() const { return m_lastGraphBarrierCount; }
    u32 lastRecordedCommandCount() const { return m_lastRecordedCommands; }
    const RenderGraph& renderGraph() const { return m_renderGraph; }
    const CommandBufferRecorder& commandRecorder() const { return m_commandRecorder; }
    u32 currentFrameSlot() const;
    const RasterPathStats& lastRasterStats() const { return m_lastRasterStats; }
    const CompositePassStats& lastCompositeStats() const { return m_lastCompositeStats; }
    const CompositeGpuPathStats& lastCompositeGpuStats() const { return m_lastCompositeGpuStats; }
    const fuse::renderer::cuda::FrameSyncProgress& lastFrameSyncProgress() const {
        return m_frameSync.lastProgress();
    }
    bool frameSyncDriverWired() const { return m_frameSync.driverWired(); }

private:
    explicit RhiContext(std::unique_ptr<VulkanBootstrap> bootstrap, const Desc& desc);

    void ensureRasterPath();
    void ensureCompositePass();
    void ensureCompositeGpuPath();
    void ensureFrameSyncPair();

    std::unique_ptr<VulkanBootstrap> m_bootstrap;
    Desc m_desc;
    std::unique_ptr<RasterPath> m_rasterPath;
    std::unique_ptr<CompositePass> m_compositePass;
    std::unique_ptr<CompositeGpuPath> m_compositeGpuPath;
    RasterPathStats m_lastRasterStats{};
    CompositePassStats m_lastCompositeStats{};
    CompositeGpuPathStats m_lastCompositeGpuStats{};
    fuse::renderer::cuda::FrameSyncPair m_frameSync{};
    bool m_frameSyncInitialized = false;
    RenderGraph m_renderGraph;
    CommandBufferRecorder m_commandRecorder;
    u32 m_submittedFrames = 0;
    u32 m_lastCommandCount = 0;
    u32 m_lastGraphPassCount = 0;
    u32 m_lastGraphBarrierCount = 0;
    u32 m_lastRecordedCommands = 0;
    u32 m_acquiredSwapchainImage = UINT32_MAX;
    u32 m_queueSubmitCount = 0;
    GraphicsQueueSubmitResult m_lastQueueSubmit{};
};

} // namespace fuse::renderer
