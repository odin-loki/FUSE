#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::compute {

enum class ScreenSpaceEffectsMode : u8 { Stub, CpuReference, Cuda };

struct ScreenSpaceEffectsInfo {
    bool valid = false;
    ScreenSpaceEffectsMode mode = ScreenSpaceEffectsMode::Stub;
};

ScreenSpaceEffectsInfo screen_space_effects_info();

/// SSAO (HBAO-style) pass parameters — P5 §5.7.
struct SSAOParams {
    void* depth_surface = nullptr;
    void* normal_surface = nullptr;
    void* ao_out_surface = nullptr;
    u32 width = 0;
    u32 height = 0;

    f32 proj[16]{};
    f32 inv_proj[16]{};

    f32 radius = 1.f;
    f32 bias = 0.1f;
    u32 directions = 8;
    u32 steps_per_dir = 4;
    f32 strength = 1.5f;
    f32 max_radius_px = 64.f;
    u64 frame_seed = 0;
};

/// Screen-space reflections — P5 §5.7.
struct SSRParams {
    void* depth_surface = nullptr;
    void* normal_surface = nullptr;
    void* roughness_surface = nullptr;
    void* scene_color_surface = nullptr;
    void* ssr_out_surface = nullptr;
    u32 width = 0;
    u32 height = 0;

    f32 view_proj[16]{};
    f32 inv_view_proj[16]{};
    f32 proj[16]{};
    math::Vec3 cam_pos{};

    u32 max_steps = 64;
    f32 ray_step_size = 0.2f;
    f32 thickness = 0.5f;
    f32 max_distance = 20.f;
    f32 fade_screen_edge = 0.1f;
    bool use_hiz = true;
};

/// Screen-space global illumination stub — reserved for B5.7+ quality tier.
struct SSGIParams {
    void* depth_surface = nullptr;
    void* normal_surface = nullptr;
    void* albedo_surface = nullptr;
    void* ssgi_out_surface = nullptr;
    u32 width = 0;
    u32 height = 0;

    f32 view_proj[16]{};
    f32 inv_view_proj[16]{};
    math::Vec3 cam_pos{};

    u32 max_bounces = 1;
    f32 ray_step_size = 0.25f;
    f32 thickness = 0.5f;
    f32 intensity = 1.f;
};

/// Host launchers — CUDA path when `FUSE_HAS_CUDA=1`, CPU reference otherwise.
bool launch_ssao(const SSAOParams& params, void* stream = nullptr);
bool launch_ssr(const SSRParams& params, void* stream = nullptr);
bool launch_ssgi(const SSGIParams& params, void* stream = nullptr);

/// CPU reference center-pixel samples for unit tests.
f32 ssao_center_sample(const SSAOParams& params);
f32 ssr_center_sample(const SSRParams& params);
f32 ssgi_center_sample(const SSGIParams& params);

} // namespace fuse::compute
