#include <fuse/renderer/volumetric/light_shafts.hpp>

#include <fuse/renderer/command_buffer.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

struct LightShaftsPassUserData {
    LightShaftsParams params{};
    LightShaftsPassStats stats{};
};

void executeLightShaftsPass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    auto* pass = static_cast<LightShaftsPassUserData*>(userData);
    if (recorder == nullptr || pass == nullptr) {
        return;
    }

    recorder->beginPass("light_shafts");
    record_light_shafts_pass(pass->params, pass->stats);
    recorder->endPass();
}

LightShaftsPassUserData g_lightShaftsPasses[RenderGraph::kMaxPassesPerFrame]{};
RGTextureAccess g_lightShaftsAccesses[RenderGraph::kMaxPassesPerFrame]{};
u32 g_lightShaftsPassCount = 0;

} // namespace

f32 evaluate_light_shaft_occlusion(f32 ray_depth, f32 scene_depth, f32 edge_softness) {
    const f32 softness = std::max(edge_softness, 1e-4f);
    const f32 delta = scene_depth - ray_depth;
    if (delta <= 0.f) {
        return 0.f;
    }
    return std::clamp(delta / softness, 0.f, 1.f);
}

bool record_light_shafts_pass(const LightShaftsParams& params, LightShaftsPassStats& stats) {
    if (!params.enabled || params.ray_march_steps == 0u) {
        stats.ready = false;
        return false;
    }

    stats.ready = true;
    stats.lastOcclusion = evaluate_light_shaft_occlusion(0.5f, 0.55f, params.edge_softness) * params.intensity;
    ++stats.framesRecorded;
    return true;
}

void resetLightShaftsPassGraphStorage() {
    g_lightShaftsPassCount = 0;
}

void addLightShaftsPassToGraph(RenderGraph& graph, RGTextureRef output) {
    if (g_lightShaftsPassCount >= RenderGraph::kMaxPassesPerFrame) {
        return;
    }

    const u32 passIndex = g_lightShaftsPassCount++;
    LightShaftsPassUserData& userData = g_lightShaftsPasses[passIndex];
    userData.params = LightShaftsParams{};
    userData.stats = {};

    g_lightShaftsAccesses[passIndex].texture = output;
    g_lightShaftsAccesses[passIndex].access = RGResourceAccess::ColorAttachmentWrite;

    RGPassDesc pass{};
    pass.name = "light_shafts";
    pass.execute = executeLightShaftsPass;
    pass.userData = &userData;
    pass.textureAccesses = &g_lightShaftsAccesses[passIndex];
    pass.textureAccessCount = 1u;
    graph.addPass(pass);
}

} // namespace fuse::renderer
