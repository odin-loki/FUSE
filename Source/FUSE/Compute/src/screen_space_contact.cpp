#include <fuse/compute/screen_space_contact.hpp>
#include <fuse/compute/screen_space_kernels.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::compute {

namespace {

f32 clamp01(f32 value) {
    return std::clamp(value, 0.f, 1.f);
}

f32 saturate(f32 value) {
    return clamp01(value);
}

} // namespace

f32 ssr_contact_harden_roughness(f32 ray_hit_distance, f32 material_roughness, const SSRParams& params) {
    // Single-source: the SSR kernel body applies the same function (fuse/ssfx/ssr_kernel.hpp).
    return ssfx::ssr_kernel::contact_harden_roughness(ray_hit_distance, material_roughness,
                                                      screen_space_kernels::to_contact(params));
}

f32 ssao_blur_weight(f32 center_depth,
                     f32 neighbor_depth,
                     f32 center_normal_z,
                     f32 neighbor_normal_z,
                     const SSAOParams& params) {
    // Single-source: the "screen_space_ao_blur" kernel body uses the same weight.
    return screen_space_kernels::ao_blur::weight(center_depth, neighbor_depth, center_normal_z, neighbor_normal_z,
                                                 screen_space_kernels::ao_blur::thresholds(params));
}

f32 ssr_screen_edge_fade(f32 uv_x, f32 uv_y, const SSRParams& params) {
    if (params.fade_screen_edge <= 0.f) {
        return 1.f;
    }

    const f32 edgeDistance =
        std::min(std::min(uv_x, 1.f - uv_x), std::min(uv_y, 1.f - uv_y));
    return saturate(edgeDistance / params.fade_screen_edge);
}

bool validate_ssao_params(const SSAOParams& params) {
    if (params.width == 0 || params.height == 0) {
        return false;
    }
    if (params.directions == 0 || params.steps_per_dir == 0) {
        return false;
    }
    if (params.strength <= 0.f || params.max_radius_px <= 0.f) {
        return false;
    }
    if (params.blur_depth_threshold < 0.f || params.blur_normal_threshold < 0.f ||
        params.blur_normal_threshold > 1.f) {
        return false;
    }
    if (params.contact_depth_scale < 0.f || params.contact_normal_power <= 0.f) {
        return false;
    }
    return true;
}

bool validate_ssr_params(const SSRParams& params) {
    if (params.width == 0 || params.height == 0) {
        return false;
    }
    if (params.max_steps == 0 || params.ray_step_size <= 0.f) {
        return false;
    }
    if (params.thickness <= 0.f || params.max_distance <= 0.f) {
        return false;
    }
    if (params.fade_screen_edge < 0.f) {
        return false;
    }
    if (params.contact_hardening) {
        if (params.contact_distance <= 0.f || params.contact_harden_exponent <= 0.f) {
            return false;
        }
        if (params.contact_roughness_floor < 0.f || params.contact_roughness_floor > 1.f) {
            return false;
        }
    }
    return true;
}

bool validate_ssgi_params(const SSGIParams& params) {
    if (params.width == 0 || params.height == 0) {
        return false;
    }
    if (params.sample_sqrt == 0 || params.max_steps == 0 || params.ray_step_size <= 0.f) {
        return false;
    }
    if (params.thickness <= 0.f || params.max_distance <= 0.f || params.intensity < 0.f) {
        return false;
    }
    return true;
}

f32 ssao_contact_ao_weight(f32 depth_delta, f32 normal_similarity, const SSAOParams& params) {
    if (params.contact_depth_scale <= 0.f) {
        return 1.f;
    }

    const f32 depthFactor = clamp01(1.f - depth_delta / params.contact_depth_scale);
    const f32 normalFactor = std::pow(clamp01(normal_similarity), params.contact_normal_power);
    return clamp01(0.5f + 0.5f * depthFactor * normalFactor);
}

} // namespace fuse::compute
