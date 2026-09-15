#include <fuse/renderer/deferred/deferred_renderer.hpp>

#include <fuse/renderer/command_buffer.hpp>

namespace fuse::renderer {

std::unique_ptr<DeferredRenderer> DeferredRenderer::create(const DeferredRendererDesc& desc) {
    return std::unique_ptr<DeferredRenderer>(new DeferredRenderer(desc));
}

DeferredRenderer::DeferredRenderer(const DeferredRendererDesc& desc)
    : m_desc(desc), m_pipeline(desc.pipeline) {}

bool DeferredRenderer::init(ResourceManager& resources) {
    destroy();
    m_resources = &resources;

    GBufferDesc gbufferDesc{};
    gbufferDesc.width = m_desc.pipeline.width;
    gbufferDesc.height = m_desc.pipeline.height;
    gbufferDesc.reversedZ = m_desc.pipeline.reversedZ;

    if (!m_gbuffer.init(resources, gbufferDesc)) {
        destroy();
        return false;
    }

    m_materials.init(resources);

    ClusterDesc clusterDesc{};
    clusterDesc.tilesX = 4;
    clusterDesc.tilesY = 4;
    clusterDesc.slicesZ = 4;
    clusterDesc.maxLightsPerCluster = 64;
    m_lightCuller.init(clusterDesc, resources);

    m_stats.ready = true;
    return true;
}

void DeferredRenderer::destroy() {
    m_lightCuller.destroy();
    m_gbuffer.destroy();
    m_materials.destroy();
    m_resources = nullptr;
    m_stats = {};
}

bool DeferredRenderer::buildFrameGraph(RenderGraph& graph, u32 backbufferIndex) {
    if (!m_stats.ready) {
        return false;
    }

    graph.beginFrame(backbufferIndex);
    m_pipeline.buildGraph(graph, m_gbuffer, m_lightCuller.isReady() ? &m_lightCuller : nullptr);
    graph.compile();

    ++m_stats.framesBuilt;
    m_stats.lastBarrierCount = graph.compileInfo().barrierCount;
    return graph.compileInfo().compiled;
}

RenderGraphExecuteInfo DeferredRenderer::executeFrame(VulkanDevice& device,
                                                      FrameManager& frames,
                                                      CommandBufferRecorder& recorder,
                                                      u32 backbufferIndex) {
    RenderGraph graph;
    if (!buildFrameGraph(graph, backbufferIndex)) {
        return {};
    }

    const RenderGraphExecuteInfo info = graph.execute(device, frames, recorder);
    m_stats.lastExecutedPassCount = info.executedPassCount;
    return info;
}

} // namespace fuse::renderer
