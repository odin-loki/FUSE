#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/compute/screen_space_contact.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::compute {

namespace {

f32 clamp01(f32 value) {
    if (value < 0.f) {
        return 0.f;
    }
    if (value > 1.f) {
        return 1.f;
    }
    return value;
}

} // namespace

f32 ssao_center_sample(const SSAOParams& params) {
    if (params.width == 0 || params.height == 0) {
        return 0.f;
    }

    // Flat reference scene with contact shaping at the center tap.
    const f32 baseVisibility = clamp01(1.f / params.strength);
    const f32 contactWeight = ssao_contact_ao_weight(0.f, 1.f, params);
    return clamp01(baseVisibility * contactWeight);
}

f32 ssr_center_sample(const SSRParams& params) {
    if (params.width == 0 || params.height == 0) {
        return 0.f;
    }

    // Center pixel: no ray hit, but report edge fade and contact-hardening readiness.
    const f32 edgeFade = ssr_screen_edge_fade(0.5f, 0.5f, params);
    const f32 hardenedRoughness = ssr_contact_harden_roughness(params.max_distance, 0.5f, params);
    (void)params.max_steps;
    (void)params.use_hiz;
    return clamp01(edgeFade * (1.f - hardenedRoughness));
}

f32 ssgi_center_sample(const SSGIParams& params) {
    if (params.width == 0 || params.height == 0) {
        return 0.f;
    }

    // Deterministic indirect stub: scales with bounce budget and intensity.
    const f32 bounceFactor = static_cast<f32>(params.max_bounces) * 0.1f;
    return clamp01(bounceFactor * params.intensity);
}

bool launch_ssao_cpu(const SSAOParams& params) {
    return validate_ssao_params(params);
}

bool launch_ssr_cpu(const SSRParams& params) {
    return validate_ssr_params(params);
}

bool launch_ssgi_cpu(const SSGIParams& params) {
    (void)params;
    return true;
}

} // namespace fuse::compute
