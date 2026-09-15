#include <fuse/renderer/postprocess/lens_flare.hpp>

#include <fuse/renderer/command_buffer.hpp>

#include <cmath>

namespace fuse::renderer {
namespace {

struct LensFlarePassUserData {
    LensFlareParams params{};
    math::Vec2 sun_screen_pos{};
    f32 sun_intensity = 1.f;
    math::Vec3 sun_color{1.f, 1.f, 1.f};
    f32 occlusion = 1.f;
    LensFlarePassStats stats{};
};

void executeLensFlarePass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    auto* pass = static_cast<LensFlarePassUserData*>(userData);
    if (recorder == nullptr || pass == nullptr) {
        return;
    }

    recorder->beginPass("lens_flare");
    record_lens_flare_pass(pass->params,
                           pass->sun_screen_pos,
                           pass->sun_intensity,
                           pass->sun_color,
                           pass->occlusion,
                           pass->stats);
    recorder->endPass();
}

LensFlarePassUserData g_lensFlarePasses[RenderGraph::kMaxPassesPerFrame]{};
RGTextureAccess g_lensFlareAccesses[RenderGraph::kMaxPassesPerFrame]{};
u32 g_lensFlarePassCount = 0;

} // namespace

void generate_lens_flare(math::Vec2 sun_screen_pos,
                       f32 sun_intensity,
                       math::Vec3 sun_color,
                       f32 occlusion,
                       const LensFlareParams& params,
                       std::vector<LensFlareSample>& out_elements) {
    out_elements.clear();
    if (!params.enabled || occlusion <= 0.f || sun_intensity <= 0.f || params.ghost_count == 0u) {
        return;
    }

    const math::Vec2 centre{0.f, 0.f};
    const math::Vec2 axis = {centre.x - sun_screen_pos.x, centre.y - sun_screen_pos.y};

    for (u32 i = 0; i < params.ghost_count; ++i) {
        const f32 t = static_cast<f32>(i + 1u) / static_cast<f32>(params.ghost_count + 1u);
        LensFlareSample sample{};
        sample.position = {sun_screen_pos.x + axis.x * t, sun_screen_pos.y + axis.y * t};
        sample.size = 0.04f + 0.02f * static_cast<f32>(i);
        sample.color = sun_color;
        sample.intensity = sun_intensity * occlusion * (1.f - t * 0.5f);
        out_elements.push_back(sample);
    }

    LensFlareSample halo{};
    halo.position = centre;
    halo.size = 0.25f * params.halo_scale;
    halo.color = sun_color;
    halo.intensity = sun_intensity * occlusion * 0.35f;
    out_elements.push_back(halo);

    const u32 starburst_arms = 6u;
    for (u32 arm = 0; arm < starburst_arms; ++arm) {
        const f32 angle = static_cast<f32>(arm) * (2.f * 3.14159265f / static_cast<f32>(starburst_arms));
        LensFlareSample burst{};
        burst.position = {sun_screen_pos.x + std::cos(angle) * params.starburst_spread,
                          sun_screen_pos.y + std::sin(angle) * params.starburst_spread};
        burst.size = 0.02f;
        burst.color = sun_color;
        burst.intensity = sun_intensity * occlusion * 0.2f;
        out_elements.push_back(burst);
    }
}

bool record_lens_flare_pass(const LensFlareParams& params,
                            math::Vec2 sun_screen_pos,
                            f32 sun_intensity,
                            math::Vec3 sun_color,
                            f32 occlusion,
                            LensFlarePassStats& stats) {
    if (!params.enabled) {
        stats.ready = false;
        return false;
    }

    std::vector<LensFlareSample> elements;
    generate_lens_flare(sun_screen_pos, sun_intensity, sun_color, occlusion, params, elements);

    stats.ready = !elements.empty();
    stats.lastElementCount = static_cast<u32>(elements.size());
    ++stats.framesRecorded;
    return stats.ready;
}

void resetLensFlarePassGraphStorage() {
    g_lensFlarePassCount = 0;
}

void addLensFlarePassToGraph(RenderGraph& graph, RGTextureRef output) {
    if (g_lensFlarePassCount >= RenderGraph::kMaxPassesPerFrame) {
        return;
    }

    const u32 passIndex = g_lensFlarePassCount++;
    LensFlarePassUserData& userData = g_lensFlarePasses[passIndex];
    userData.params = LensFlareParams{};
    userData.sun_screen_pos = {0.2f, 0.3f};
    userData.sun_intensity = 1.f;
    userData.sun_color = {1.f, 0.95f, 0.85f};
    userData.occlusion = 1.f;
    userData.stats = {};

    g_lensFlareAccesses[passIndex].texture = output;
    g_lensFlareAccesses[passIndex].access = RGResourceAccess::ColorAttachmentWrite;

    RGPassDesc pass{};
    pass.name = "lens_flare";
    pass.execute = executeLensFlarePass;
    pass.userData = &userData;
    pass.textureAccesses = &g_lensFlareAccesses[passIndex];
    pass.textureAccessCount = 1u;
    graph.addPass(pass);
}

} // namespace fuse::renderer
