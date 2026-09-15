#include <fuse/renderer/shadow/shadow_pass.hpp>

#include <fuse/renderer/command_buffer.hpp>

namespace fuse::renderer {
namespace {

struct ShadowPassUserData {
    bool recordAtlasWrite = true;
};

ShadowPassUserData g_shadowPasses[RenderGraph::kMaxPassesPerFrame]{};
RGTextureAccess g_shadowPassAccesses[RenderGraph::kMaxPassesPerFrame]{};
u32 g_shadowPassCount = 0;

void executeShadowPass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    const auto* pass = static_cast<const ShadowPassUserData*>(userData);
    if (recorder == nullptr || pass == nullptr) {
        return;
    }
    recorder->beginPass("shadow_maps");
    recorder->endPass();
}

} // namespace

std::unique_ptr<ShadowPass> ShadowPass::create(const ShadowPassDesc& desc) {
    return std::unique_ptr<ShadowPass>(new ShadowPass(desc));
}

ShadowPass::ShadowPass(const ShadowPassDesc& desc) : m_desc(desc) {
    m_stats.ready = true;
    m_stats.message = "shadow pass scaffold ready";
}

bool ShadowPass::recordFrame(const DirectionalShadow& shadows, RenderGraph& graph) {
    if (!m_stats.ready) {
        m_stats.message = "shadow pass not ready";
        return false;
    }

    if (!shadows.isReady()) {
        m_stats.message = "directional shadow not ready";
        return false;
    }

    addShadowPassToGraph(graph, shadows);
    ++m_stats.framesRecorded;
    m_stats.message = "shadow pass recorded (stub)";
    return true;
}

void resetShadowPassGraphStorage() {
    g_shadowPassCount = 0;
}

void addShadowPassToGraph(RenderGraph& graph, const DirectionalShadow& shadows) {
    if (g_shadowPassCount >= RenderGraph::kMaxPassesPerFrame) {
        return;
    }

    const u32 passIndex = g_shadowPassCount++;
    ShadowPassUserData& pass = g_shadowPasses[passIndex];
    pass.recordAtlasWrite = true;

    RGTextureRef shadowTarget{};
    if (shadows.atlas().isReady() && shadows.atlas().texture().isValid()) {
        shadowTarget = graph.importTexture(shadows.atlas().texture(), RGImageLayout::Undefined);
    } else {
        shadowTarget = graph.createTransient({});
    }

    g_shadowPassAccesses[passIndex].texture = shadowTarget;
    g_shadowPassAccesses[passIndex].access = RGResourceAccess::DepthAttachmentWrite;

    RGPassDesc passDesc{};
    passDesc.name = "shadow_maps";
    passDesc.execute = executeShadowPass;
    passDesc.userData = &pass;
    passDesc.textureAccesses = &g_shadowPassAccesses[passIndex];
    passDesc.textureAccessCount = 1u;
    graph.addPass(passDesc);
}

} // namespace fuse::renderer
