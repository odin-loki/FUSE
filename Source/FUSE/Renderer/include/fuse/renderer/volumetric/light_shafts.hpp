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
    /// Optional scene-depth row or image, not owned. Null or a zero width/height keeps the
    /// scalar stub. `depth_stride` is the row pitch in floats; 0 packs rows at `depth_width`.
    /// A pitch shorter than the width is ignored and the scalar stub is used.
    const f32* scene_depths = nullptr;
    u32 depth_width = 0;
    u32 depth_height = 0;
    u32 depth_stride = 0;
};

struct LightShaftsPassStats {
    bool ready = false;
    u32 framesRecorded = 0;
    f32 lastOcclusion = 0.f;
};

/// CPU stub — screen-space shaft occlusion from depth delta.
f32 evaluate_light_shaft_occlusion(f32 ray_depth, f32 scene_depth, f32 edge_softness);

/// Records one light-shaft pass. Returns false when disabled or `ray_march_steps` is 0
/// (`ready` is cleared; other stats are left unchanged). A depth image is marched from its
/// center pixel toward normalized `sun_direction.xy`, one pixel per step, for up to
/// `ray_march_steps` samples. A sample outside the image stops the march. Samples average
/// `evaluate_light_shaft_occlusion` at ray depth 0.5, then scale by intensity. No image
/// uses scene depth 0.55 against that same ray depth.
bool record_light_shafts_pass(const LightShaftsParams& params, LightShaftsPassStats& stats);

/// Render-graph hook — inserts the light-shafts pass after atmosphere/sky.
void resetLightShaftsPassGraphStorage();
void addLightShaftsPassToGraph(RenderGraph& graph, RGTextureRef output);

} // namespace fuse::renderer
