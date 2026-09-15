#include <fuse/audio/attenuation.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

namespace {

float sample_custom_keypoints(float distance, const AttenuationParams& params) {
    if (params.keypoint_count == 0) {
        return 1.f;
    }

    const AttenuationKeypoint* points = params.keypoints;
    const u32 count = params.keypoint_count;

    if (distance <= points[0].distance) {
        return points[0].gain;
    }
    if (distance >= points[count - 1].distance) {
        return points[count - 1].gain;
    }

    for (u32 i = 1; i < count; ++i) {
        const float d0 = points[i - 1].distance;
        const float d1 = points[i].distance;
        if (distance > d1) {
            continue;
        }

        const float span = d1 - d0;
        if (span <= 1e-6f) {
            return points[i].gain;
        }

        const float t = (distance - d0) / span;
        return points[i - 1].gain + t * (points[i].gain - points[i - 1].gain);
    }

    return points[count - 1].gain;
}

} // namespace

float compute_attenuation(float distance, float min_dist, float max_dist) {
    AttenuationParams params;
    params.curve = AttenuationCurve::Linear;
    params.min_dist = min_dist;
    params.max_dist = max_dist;
    params.rolloff = 1.f;
    return compute_attenuation(distance, params);
}

float sample_attenuation_curve(float distance, const AttenuationParams& params) {
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

    case AttenuationCurve::Inverse:
        gain = params.min_dist / (params.rolloff * clamped);
        break;

    case AttenuationCurve::Custom:
        gain = sample_custom_keypoints(distance, params);
        break;
    }

    return std::clamp(gain, 0.f, 1.f);
}

float compute_attenuation(float distance, const AttenuationParams& params) {
    if (distance <= params.min_dist) {
        return 1.f;
    }
    if (distance >= params.max_dist) {
        return 0.f;
    }

    return sample_attenuation_curve(distance, params);
}

} // namespace fuse::audio
