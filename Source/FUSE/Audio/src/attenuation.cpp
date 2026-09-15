#include <fuse/audio/attenuation.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

float compute_attenuation(float distance, float min_dist, float max_dist) {
    AttenuationParams params;
    params.curve = AttenuationCurve::Linear;
    params.min_dist = min_dist;
    params.max_dist = max_dist;
    params.rolloff = 1.f;
    return compute_attenuation(distance, params);
}

float compute_attenuation(float distance, const AttenuationParams& params) {
    if (distance <= params.min_dist) {
        return 1.f;
    }
    if (distance >= params.max_dist) {
        return 0.f;
    }

    const float clamped = std::max(distance, params.min_dist);
    float gain = 1.f;

    switch (params.curve) {
    case AttenuationCurve::Linear:
        gain = params.min_dist / (params.min_dist + (clamped - params.min_dist));
        break;

    case AttenuationCurve::Logarithmic:
        gain = params.min_dist
            / (params.min_dist + params.rolloff * (clamped - params.min_dist));
        break;

    case AttenuationCurve::Exponential:
        gain = std::pow(clamped / params.min_dist, -params.rolloff);
        break;
    }

    return std::clamp(gain, 0.f, 1.f);
}

} // namespace fuse::audio
