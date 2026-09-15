#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// God-ray / light shaft parameters (B5.11 — P5 §5.11 volumetric lighting).
struct LightShaftsParams {
    math::Vec3 sun_direction{0.f, 1.f, 0.f};
    math::Vec3 sun_color{1.f, 0.95f, 0.85f};
    f32 intensity = 1.f;
    f32 exposure = 0.5f;
    u32 ray_march_steps = 16;
    f32 edge_softness = 0.05f;
    bool enabled = true;
};

struct LightShaftsPassStats {
    bool ready = false;
    u32 framesRecorded = 0;
    f32 lastOcclusion = 0.f;
};

/// CPU stub — screen-space shaft occlusion from depth delta.
f32 evaluate_light_shaft_occlusion(f32 ray_depth, f32 scene_depth, f32 edge_softness);

/// Records logical light-shaft work for the frame; returns false when disabled.
bool record_light_shafts_pass(const LightShaftsParams& params, LightShaftsPassStats& stats);

/// Render-graph hook — inserts the light-shafts pass after atmosphere/sky.
void resetLightShaftsPassGraphStorage();
void addLightShaftsPassToGraph(RenderGraph& graph, RGTextureRef output);

} // namespace fuse::renderer
