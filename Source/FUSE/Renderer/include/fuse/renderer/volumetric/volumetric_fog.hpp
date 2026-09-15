#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Volumetric fog parameters (B5.11 — P5 §5.11).
struct VolumetricFogParams {
    f32 density = 0.02f;
    f32 anisotropy = 0.3f;
    math::Vec3 fog_color{0.8f, 0.85f, 0.9f};
    f32 height_falloff = 0.2f;
    f32 base_height = 0.f;
    u32 march_steps = 32;
    bool receive_shadows = true;
};

struct VolumetricFogPassStats {
    bool ready = false;
    u32 framesRecorded = 0;
    f32 lastDensity = 0.f;
};

/// CPU stub — exponential height falloff density sample (P5 acceptance reference).
f32 sample_volumetric_fog_density(const VolumetricFogParams& params, const math::Vec3& world_pos);

/// Records logical volumetric fog work for the frame; returns false when disabled.
bool record_volumetric_fog_pass(const VolumetricFogParams& params, VolumetricFogPassStats& stats);

/// Render-graph hook — inserts the volumetric fog CUDA pass after screen-space AO.
void resetVolumetricFogPassGraphStorage();
void addVolumetricFogPassToGraph(RenderGraph& graph, const RGTextureAccess* depth_read, u32 access_count);

} // namespace fuse::renderer
