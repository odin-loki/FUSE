#include <fuse/compute/screen_space_contact.hpp>

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
    if (!params.contact_hardening || params.contact_distance <= 0.f) {
        return std::clamp(material_roughness, 0.f, 1.f);
    }

    const f32 distance = std::max(ray_hit_distance, 0.f);
    const f32 contactFactor =
        std::pow(saturate(1.f - distance / params.contact_distance), params.contact_harden_exponent);
    const f32 hardenedRoughness =
        material_roughness + (params.contact_roughness_floor - material_roughness) * contactFactor;
    return std::clamp(hardenedRoughness, params.contact_roughness_floor, 1.f);
}

f32 ssao_blur_weight(f32 center_depth,
                     f32 neighbor_depth,
                     f32 center_normal_z,
                     f32 neighbor_normal_z,
                     const SSAOParams& params) {
    if (!params.enable_blur) {
        return 0.f;
    }

    const f32 depthDelta = std::fabs(center_depth - neighbor_depth);
    if (depthDelta > params.blur_depth_threshold) {
        return 0.f;
    }

    const f32 normalSimilarity = center_normal_z * neighbor_normal_z;
    if (normalSimilarity < params.blur_normal_threshold) {
        return 0.f;
    }

    const f32 depthWeight = 1.f - depthDelta / std::max(params.blur_depth_threshold, 1e-6f);
    const f32 normalWeight =
        (normalSimilarity - params.blur_normal_threshold) / std::max(1.f - params.blur_normal_threshold, 1e-6f);
    return clamp01(depthWeight * normalWeight);
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

f32 ssao_contact_ao_weight(f32 depth_delta, f32 normal_similarity, const SSAOParams& params) {
    if (params.contact_depth_scale <= 0.f) {
        return 1.f;
    }

    const f32 depthFactor = clamp01(1.f - depth_delta / params.contact_depth_scale);
    const f32 normalFactor = std::pow(clamp01(normal_similarity), params.contact_normal_power);
    return clamp01(0.5f + 0.5f * depthFactor * normalFactor);
}

} // namespace fuse::compute
