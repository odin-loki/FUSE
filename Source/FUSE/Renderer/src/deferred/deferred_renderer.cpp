#include <fuse/renderer/deferred/deferred_renderer.hpp>

#include <fuse/renderer/command_buffer.hpp>

#include <algorithm>
#include <vector>

namespace fuse::renderer {
namespace {

bool deferredGBufferDrawLess(const DeferredGBufferDraw& a, const DeferredGBufferDraw& b) {
    if (a.materialId != b.materialId) {
        return a.materialId < b.materialId;
    }
    if (a.entity.index != b.entity.index) {
        return a.entity.index < b.entity.index;
    }
    return a.entity.generation < b.entity.generation;
}

} // namespace

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

    // B5.4 grid: 16x9 screen tiles x 24 exponential depth slices, 256 lights per cluster.
    const ClusterDesc clusterDesc{};
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
    // CPU draws survive init/destroy. appendGBufferDrawList({}) is what clears them.
}

bool DeferredRenderer::buildFrameGraph(RenderGraph& graph, u32 backbufferIndex) {
    if (!m_stats.ready) {
        return false;
    }

    graph.beginFrame(backbufferIndex);
    m_pipeline.buildGraph(graph, m_gbuffer, m_lightCuller.isReady() ? &m_lightCuller : nullptr,
                          &m_gbufferDraws);
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

void DeferredRenderer::appendGBufferDrawList(std::span<const DeferredGBufferDraw> draws) {
    if (draws.empty()) {
        m_gbufferDraws.clear();
        return;
    }

    // Copy first so a span that aliases the stored list cannot invalidate the insert.
    const std::vector<DeferredGBufferDraw> incoming(draws.begin(), draws.end());
    m_gbufferDraws.reserve(m_gbufferDraws.size() + incoming.size());
    m_gbufferDraws.insert(m_gbufferDraws.end(), incoming.begin(), incoming.end());
    std::stable_sort(m_gbufferDraws.begin(), m_gbufferDraws.end(), deferredGBufferDrawLess);
}

} // namespace fuse::renderer
