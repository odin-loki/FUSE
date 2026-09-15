#include <fuse/renderer/atmosphere/sky_pass.hpp>

#include <fuse/renderer/command_buffer.hpp>

namespace fuse::renderer {
namespace {

struct SkyPassUserData {
    bool recorded = false;
};

void executeSkyPass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    auto* sky = static_cast<SkyPassUserData*>(userData);
    if (recorder == nullptr || sky == nullptr) {
        return;
    }
    recorder->beginPass("atmosphere_sky");
    sky->recorded = true;
    recorder->endPass();
}

SkyPassUserData g_skyPasses[RenderGraph::kMaxPassesPerFrame]{};
RGTextureAccess g_skyAccesses[2]{};
u32 g_skyPassCount = 0;

} // namespace

std::unique_ptr<SkyPass> SkyPass::create(const SkyPassDesc& desc) {
    return std::unique_ptr<SkyPass>(new SkyPass(desc));
}

SkyPass::SkyPass(const SkyPassDesc& desc) : m_desc(desc) {
    m_stats.lutReady = m_lut.build(m_desc.lut, m_desc.sun_direction);
    m_stats.ready = m_stats.lutReady;
    m_stats.message = m_stats.ready ? "sky pass scaffold ready" : "sky LUT build failed";
}

bool SkyPass::recordFrame() {
    if (!m_stats.ready) {
        m_stats.message = "sky pass not ready";
        return false;
    }

    ++m_stats.framesRecorded;
    m_stats.message = "sky frame recorded (stub)";
    return true;
}

void resetSkyPassGraphStorage() {
    g_skyPassCount = 0;
}

void addSkyPassToGraph(RenderGraph& graph) {
    if (g_skyPassCount >= RenderGraph::kMaxPassesPerFrame) {
        return;
    }

    const RGTextureRef depthTexture = graph.createTransient({});
    const RGTextureRef skyTexture = graph.createTransient({});

    g_skyAccesses[0].texture = depthTexture;
    g_skyAccesses[0].access = RGResourceAccess::ShaderRead;
    g_skyAccesses[1].texture = skyTexture;
    g_skyAccesses[1].access = RGResourceAccess::ShaderWrite;

    SkyPassUserData& sky = g_skyPasses[g_skyPassCount++];
    sky.recorded = false;

    RGPassDesc pass{};
    pass.name = "atmosphere_sky";
    pass.execute = executeSkyPass;
    pass.userData = &sky;
    pass.textureAccesses = g_skyAccesses;
    pass.textureAccessCount = 2;
    graph.addPass(pass);
}

} // namespace fuse::renderer
