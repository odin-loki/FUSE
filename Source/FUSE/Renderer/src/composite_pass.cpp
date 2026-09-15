#include <fuse/renderer/composite_pass.hpp>

#include <fuse/renderer/command_buffer.hpp>

namespace fuse::renderer {
namespace {

struct CompositePassUserData {
    float blend = 0.5f;
};

void executeCompositePass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    const auto* composite = static_cast<const CompositePassUserData*>(userData);
    if (recorder == nullptr || composite == nullptr) {
        return;
    }
    recorder->composite(composite->blend);
}

CompositePassUserData g_compositePasses[RenderGraph::kMaxPassesPerFrame]{};
RGTextureAccess g_compositeAccesses[3]{};
u32 g_compositePassCount = 0;

} // namespace

std::unique_ptr<CompositePass> CompositePass::create(const CompositePassDesc& desc) {
    return std::unique_ptr<CompositePass>(new CompositePass(desc));
}

CompositePass::CompositePass(const CompositePassDesc& desc) : m_desc(desc) {
    m_stats.ready = true;
    m_stats.message = "composite pass scaffold ready";
}

float CompositePass::blendForFrame(const RenderCommandList& /*commands*/) const {
    return m_desc.defaultBlend;
}

bool CompositePass::recordFrame(const RenderCommandList& commands) {
    if (!m_stats.ready) {
        m_stats.message = "composite pass not ready";
        return false;
    }

    m_stats.lastBlend = blendForFrame(commands);
    ++m_stats.framesRecorded;
    m_stats.message = "composite frame recorded (stub)";
    return true;
}

void resetCompositePassGraphStorage() {
    g_compositePassCount = 0;
}

void addCompositePassToGraph(RenderGraph& graph, float blend) {
    if (g_compositePassCount >= RenderGraph::kMaxPassesPerFrame) {
        return;
    }

    const RGTextureRef rasterTexture = graph.createTransient({});
    const RGTextureRef cudaTexture = graph.createTransient({});

    g_compositeAccesses[0].texture = rasterTexture;
    g_compositeAccesses[0].access = RGResourceAccess::ShaderRead;
    g_compositeAccesses[1].texture = cudaTexture;
    g_compositeAccesses[1].access = RGResourceAccess::CUDARead;
    g_compositeAccesses[2].texture = {RenderGraph::kBackbufferTextureId};
    g_compositeAccesses[2].access = RGResourceAccess::ShaderWrite;

    CompositePassUserData& composite = g_compositePasses[g_compositePassCount++];
    composite.blend = blend;

    RGPassDesc pass{};
    pass.name = "composite";
    pass.execute = executeCompositePass;
    pass.userData = &composite;
    pass.textureAccesses = g_compositeAccesses;
    pass.textureAccessCount = 3;
    graph.addPass(pass);
}

} // namespace fuse::renderer
