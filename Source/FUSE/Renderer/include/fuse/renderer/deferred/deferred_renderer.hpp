#pragma once

#include <fuse/renderer/deferred/frame_pipeline.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/material/material_system.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/types.hpp>

#include <memory>

namespace fuse::renderer {

struct DeferredRendererDesc {
    DeferredFramePipelineDesc pipeline{};
};

struct DeferredRendererStats {
    bool ready = false;
    u32 framesBuilt = 0;
    u32 lastExecutedPassCount = 0;
    u32 lastBarrierCount = 0;
};

/// B5.1 deferred renderer scaffold — owns G-buffer, material table, and frame graph wiring.
class DeferredRenderer {
public:
    static std::unique_ptr<DeferredRenderer> create(const DeferredRendererDesc& desc = {});

    bool init(ResourceManager& resources);
    void destroy();

    bool isReady() const { return m_stats.ready; }
    const DeferredRendererStats& stats() const { return m_stats; }

    GBuffer& gbuffer() { return m_gbuffer; }
    const GBuffer& gbuffer() const { return m_gbuffer; }

    MaterialSystem& materials() { return m_materials; }
    const MaterialSystem& materials() const { return m_materials; }

    DeferredFramePipeline& pipeline() { return m_pipeline; }
    const DeferredFramePipeline& pipeline() const { return m_pipeline; }

    ClusteredLightCuller& lightCuller() { return m_lightCuller; }
    const ClusteredLightCuller& lightCuller() const { return m_lightCuller; }

    bool buildFrameGraph(RenderGraph& graph, u32 backbufferIndex);
    RenderGraphExecuteInfo executeFrame(VulkanDevice& device,
                                        FrameManager& frames,
                                        CommandBufferRecorder& recorder,
                                        u32 backbufferIndex);

private:
    explicit DeferredRenderer(const DeferredRendererDesc& desc);

    DeferredRendererDesc m_desc{};
    DeferredRendererStats m_stats{};
    ResourceManager* m_resources = nullptr;
    GBuffer m_gbuffer{};
    MaterialSystem m_materials{};
    DeferredFramePipeline m_pipeline;
    ClusteredLightCuller m_lightCuller{};
};

} // namespace fuse::renderer
