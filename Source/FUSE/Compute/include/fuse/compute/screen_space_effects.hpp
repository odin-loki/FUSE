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

// CPU reference surfaces (`launch_*_cpu`, `*_center_sample`, and `launch_*` without CUDA): every `*_surface` is a
// tightly packed host array of `width * height` elements, row 0 = top of the image:
//   depth_surface         const f32*         linear view depth (> 0; <= 0 = sky / no geometry)
//   normal_surface        const math::Vec3*  unit view-space normals, engine view convention (right-handed, +Y up,
//                                            -Z forward as built by math::lookAt); null = reconstruct from depth
//   scene_color_surface   const math::Vec3*  linear RGB radiance
//   albedo_surface        const math::Vec3*  linear diffuse albedo (null = 1)
//   roughness_surface     const f32*         perceptual roughness (null = mirror)
//   ao_out_surface        f32*               visibility in [0, 1] (1 = unoccluded)
//   ssr_out_surface       math::Vec4*        rgb = reflected radiance (unfaded), w = confidence in [0, 1]
//   ssgi_out_surface      math::Vec3*        outgoing indirect radiance
// `proj` is a column-major perspective projection as built by math::perspective (Vulkan depth range); only the
// intrinsics are used (focal lengths, principal point, near plane). The CPU path is the fuse_ssfx reference
// (HBAO: GTAO-form horizon integral; SSR: perspective-correct screen-space march + bisection; SSGI: stratified
// cosine-weighted gather traced with the SSR march), shared with the renderer's gate-tested references.

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
    /// Per-frame slice rotation seed for the GPU kernel; the CPU reference uses fixed slices (deterministic).
    u64 frame_seed = 0;

    /// Cross-bilateral blur (P5 §5.7 `hbao_blur_kernel`). CPU reference: 5x5 taps weighted by
    /// `ssao_blur_weight` with the neighbour's distance from the centre tangent plane relative to the centre depth
    /// as the depth delta and the normal dot product as the normal similarity.
    bool enable_blur = true;
    f32 blur_depth_threshold = 0.001f;
    f32 blur_normal_threshold = 0.95f;

    /// Contact-aware AO shaping — boosts occlusion when depth/normal agree (concave contact). Artistic helper
    /// (`ssao_contact_ao_weight`); not applied by the CPU reference, which stays the physically based integral.
    f32 contact_depth_scale = 0.05f;
    f32 contact_normal_power = 2.f;
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
    /// Minimum screen-space march stride in pixels (grows when the ray needs more than `max_steps`).
    f32 ray_step_size = 1.f;
    f32 thickness = 0.5f;
    f32 max_distance = 20.f;
    f32 fade_screen_edge = 0.1f;
    /// Bisection iterations refining the hit between the last two march samples.
    u32 refine_steps = 8;
    /// Hi-Z acceleration for the GPU kernel; the CPU reference marches every stride (same hits, slower).
    bool use_hiz = true;

    /// Contact hardening — sharpens reflections as the hit distance shrinks. CPU reference: the confidence is
    /// scaled by `1 - ssr_contact_harden_roughness(hit distance, roughness)` (a mirror trace only stands in for
    /// the glossy lobe while the hardened roughness is low; roughness at or below the floor is kept as is, so a
    /// mirror keeps full confidence); pixels with roughness >= 1 are not traced. No roughness surface = mirror.
    bool contact_hardening = true;
    f32 contact_distance = 0.5f;
    f32 contact_roughness_floor = 0.02f;
    f32 contact_harden_exponent = 2.f;
};

/// Screen-space global illumination (B5.7+ quality tier): stratified cosine-weighted diffuse gather.
struct SSGIParams {
    void* depth_surface = nullptr;
    void* normal_surface = nullptr;
    void* albedo_surface = nullptr;
    /// Lit radiance the gather re-uses as the first-bounce light source.
    void* scene_color_surface = nullptr;
    void* ssgi_out_surface = nullptr;
    u32 width = 0;
    u32 height = 0;

    f32 view_proj[16]{};
    f32 inv_view_proj[16]{};
    f32 proj[16]{};
    math::Vec3 cam_pos{};

    /// Diffuse bounces; bounce k re-lights the frame with `scene_color + indirect_(k-1)`.
    u32 max_bounces = 1;
    /// Square root of the rays gathered per pixel (`sample_sqrt^2` stratified cosine-weighted rays).
    u32 sample_sqrt = 4;
    u32 max_steps = 64;
    /// Minimum screen-space march stride in pixels.
    f32 ray_step_size = 1.f;
    f32 thickness = 0.5f;
    f32 max_distance = 10.f;
    /// Scale on the gathered indirect light (1 = physically based).
    f32 intensity = 1.f;
};

/// Host launchers — CUDA path when `FUSE_HAS_CUDA=1`, CPU reference otherwise.
bool launch_ssao(const SSAOParams& params, void* stream = nullptr);
bool launch_ssr(const SSRParams& params, void* stream = nullptr);
bool launch_ssgi(const SSGIParams& params, void* stream = nullptr);

/// CPU reference passes on host surfaces (layout above); always available. False when a parameter is invalid,
/// `proj` is not a perspective projection, or a required surface (depth, output, and scene colour for SSR/SSGI)
/// is null.
bool launch_ssao_cpu(const SSAOParams& params);
bool launch_ssr_cpu(const SSRParams& params);
bool launch_ssgi_cpu(const SSGIParams& params);

/// CPU reference values at the centre pixel `(width / 2, height / 2)` on the host surfaces, for unit tests:
/// SSAO — unblurred HBAO visibility (1 when there is no depth surface or the pixel is sky);
/// SSR — Rec. 709 luminance of the reflected radiance times its confidence (0 on a miss);
/// SSGI — Rec. 709 luminance of the outgoing indirect radiance after `max_bounces` bounces.
f32 ssao_center_sample(const SSAOParams& params);
f32 ssr_center_sample(const SSRParams& params);
f32 ssgi_center_sample(const SSGIParams& params);

} // namespace fuse::compute
