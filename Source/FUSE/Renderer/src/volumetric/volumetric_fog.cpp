#include <fuse/renderer/volumetric/volumetric_fog.hpp>

#include <fuse/renderer/command_buffer.hpp>

#include <cmath>

namespace fuse::renderer {
namespace {

struct VolumetricFogPassUserData {
    VolumetricFogParams params{};
    VolumetricFogPassStats stats{};
};

void executeVolumetricFogPass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    auto* pass = static_cast<VolumetricFogPassUserData*>(userData);
    if (recorder == nullptr || pass == nullptr) {
        return;
    }

    recorder->beginPass("volumetric_fog");
    record_volumetric_fog_pass(pass->params, pass->stats);
    recorder->endPass();
}

VolumetricFogPassUserData g_volumetricFogPasses[RenderGraph::kMaxPassesPerFrame]{};
RGTextureAccess g_volumetricFogAccesses[RenderGraph::kMaxPassesPerFrame * 4]{};
u32 g_volumetricFogPassCount = 0;

} // namespace

f32 sample_volumetric_fog_density(const VolumetricFogParams& params, const math::Vec3& world_pos) {
    const f32 height_delta = world_pos.y - params.base_height;
    const f32 height_factor = std::exp(-params.height_falloff * std::max(0.f, height_delta));
    return params.density * height_factor;
}

bool record_volumetric_fog_pass(const VolumetricFogParams& params, VolumetricFogPassStats& stats) {
    if (params.march_steps == 0u) {
        stats.ready = false;
        return false;
    }

    stats.ready = true;
    stats.lastDensity = sample_volumetric_fog_density(params, {0.f, 0.f, 0.f});
    ++stats.framesRecorded;
    return true;
}

void resetVolumetricFogPassGraphStorage() {
    g_volumetricFogPassCount = 0;
}

void addVolumetricFogPassToGraph(RenderGraph& graph, const RGTextureAccess* depth_read, u32 access_count) {
    if (g_volumetricFogPassCount >= RenderGraph::kMaxPassesPerFrame) {
        return;
    }

    const u32 passIndex = g_volumetricFogPassCount++;
    VolumetricFogPassUserData& userData = g_volumetricFogPasses[passIndex];
    userData.params = VolumetricFogParams{};
    userData.stats = {};

    const RGTextureAccess* accesses = nullptr;
    u32 resolvedCount = 0u;
    if (depth_read != nullptr && access_count > 0u) {
        const u32 base = passIndex * 4u;
        for (u32 i = 0; i < access_count; ++i) {
            g_volumetricFogAccesses[base + i] = depth_read[i];
        }
        accesses = &g_volumetricFogAccesses[base];
        resolvedCount = access_count;
    }

    RGPassDesc pass{};
    pass.name = "volumetric_fog";
    pass.execute = executeVolumetricFogPass;
    pass.userData = &userData;
    pass.textureAccesses = accesses;
    pass.textureAccessCount = resolvedCount;
    pass.isCuda = true;
    graph.addPass(pass);
}

} // namespace fuse::renderer
