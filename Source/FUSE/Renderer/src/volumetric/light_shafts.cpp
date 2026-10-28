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

namespace {

constexpr f32 kLightShaftRayDepth = 0.5f;
constexpr f32 kLightShaftScalarSceneDepth = 0.55f;

f32 scalarLightShaftOcclusion(f32 edge_softness) {
    return evaluate_light_shaft_occlusion(kLightShaftRayDepth, kLightShaftScalarSceneDepth, edge_softness);
}

bool sceneDepthImageUsable(const LightShaftsParams& params) {
    if (params.scene_depths == nullptr || params.depth_width == 0u || params.depth_height == 0u) {
        return false;
    }
    return params.depth_stride == 0u || params.depth_stride >= params.depth_width;
}

// Center pixel, then one pixel per step along normalized sun_direction.xy.
// Out-of-bounds positions stop the march. Ray depth matches the scalar stub.
f32 averageLightShaftOcclusion(const LightShaftsParams& params) {
    const u32 width = params.depth_width;
    const u32 height = params.depth_height;
    const u32 stride = params.depth_stride == 0u ? width : params.depth_stride;

    const math::Vec2 sunXy{params.sun_direction.x, params.sun_direction.y};
    const f32 sunLen = sunXy.length();
    math::Vec2 march{};
    if (sunLen > 1e-8f) {
        march = sunXy * (1.f / sunLen);
    }

    f32 x = static_cast<f32>((width - 1u) / 2u);
    f32 y = static_cast<f32>((height - 1u) / 2u);
    const f32 limitX = static_cast<f32>(width);
    const f32 limitY = static_cast<f32>(height);

    f32 sum = 0.f;
    u32 count = 0u;
    for (u32 stepIndex = 0u; stepIndex < params.ray_march_steps; ++stepIndex) {
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0.f || y < 0.f || x >= limitX ||
            y >= limitY) {
            break;
        }
        // Nearest texel, so a unit step that lands 1 ulp off an integer still hits that pixel.
        const f32 roundedX = std::floor(x + 0.5f);
        const f32 roundedY = std::floor(y + 0.5f);
        if (roundedX < 0.f || roundedY < 0.f || roundedX >= limitX || roundedY >= limitY) {
            break;
        }
        const u32 ix = static_cast<u32>(roundedX);
        const u32 iy = static_cast<u32>(roundedY);
        if (ix >= width || iy >= height) {
            break;
        }

        const usize index = static_cast<usize>(iy) * static_cast<usize>(stride) + static_cast<usize>(ix);
        const f32 sample =
            evaluate_light_shaft_occlusion(kLightShaftRayDepth, params.scene_depths[index], params.edge_softness);
        if (!std::isfinite(sample)) {
            break;
        }
        sum += sample;
        ++count;
        x += march.x;
        y += march.y;
    }

    if (count == 0u || !std::isfinite(sum)) {
        return scalarLightShaftOcclusion(params.edge_softness);
    }
    return sum / static_cast<f32>(count);
}

} // namespace

bool record_light_shafts_pass(const LightShaftsParams& params, LightShaftsPassStats& stats) {
    if (!params.enabled || params.ray_march_steps == 0u) {
        stats.ready = false;
        return false;
    }

    const f32 occlusion = sceneDepthImageUsable(params) ? averageLightShaftOcclusion(params)
                                                        : scalarLightShaftOcclusion(params.edge_softness);
    stats.ready = true;
    stats.lastOcclusion = occlusion * params.intensity;
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
