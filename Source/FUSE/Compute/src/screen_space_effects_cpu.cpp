#include <fuse/compute/screen_space_effects.hpp>

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

    // Flat reference scene: full visibility with strength shaping only.
    return clamp01(1.f / params.strength);
}

f32 ssr_center_sample(const SSRParams& params) {
    if (params.width == 0 || params.height == 0) {
        return 0.f;
    }

    // Stub path reports no screen-space hit.
    (void)params.max_steps;
    return 0.f;
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
    (void)params;
    return true;
}

bool launch_ssr_cpu(const SSRParams& params) {
    (void)params;
    return true;
}

bool launch_ssgi_cpu(const SSGIParams& params) {
    (void)params;
    return true;
}

} // namespace fuse::compute
