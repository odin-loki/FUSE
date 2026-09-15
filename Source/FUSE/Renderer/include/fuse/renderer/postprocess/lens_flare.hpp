#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Analytic lens-flare element (B5.11 — P5 §5.11).
struct LensFlareSample {
    math::Vec2 position{};
    f32 size = 0.f;
    math::Vec3 color{};
    f32 intensity = 0.f;
};

struct LensFlareParams {
    u32 ghost_count = 5;
    f32 halo_scale = 1.2f;
    f32 starburst_spread = 0.35f;
    bool enabled = true;
};

struct LensFlarePassStats {
    bool ready = false;
    u32 framesRecorded = 0;
    u32 lastElementCount = 0;
};

/// CPU stub — generates ghost/halo/starburst elements from sun screen position.
void generate_lens_flare(math::Vec2 sun_screen_pos,
                         f32 sun_intensity,
                         math::Vec3 sun_color,
                         f32 occlusion,
                         const LensFlareParams& params,
                         std::vector<LensFlareSample>& out_elements);

/// Records logical lens-flare composite work for the frame; returns false when disabled.
bool record_lens_flare_pass(const LensFlareParams& params,
                            math::Vec2 sun_screen_pos,
                            f32 sun_intensity,
                            math::Vec3 sun_color,
                            f32 occlusion,
                            LensFlarePassStats& stats);

/// Render-graph hook — inserts the lens-flare pass after the post-process stack.
void resetLensFlarePassGraphStorage();
void addLensFlarePassToGraph(RenderGraph& graph, RGTextureRef output);

} // namespace fuse::renderer
