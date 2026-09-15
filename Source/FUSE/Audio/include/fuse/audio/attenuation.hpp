#pragma once

#include <fuse/types.hpp>

namespace fuse::audio {

/// Distance attenuation curve — mirrors Torque SFXDistanceModel semantics.
enum class AttenuationCurve : u8 {
    Linear = 0,
    Logarithmic = 1,
    Exponential = 2,
    /// Pure inverse-distance: `min_dist / distance` (clamped at max_distance).
    Inverse = 3,
    /// Piecewise-linear gain sampled from sorted distance keypoints.
    Custom = 4,
};

struct AttenuationKeypoint {
    float distance = 0.f;
    float gain = 1.f;
};

struct AttenuationParams {
    static constexpr u32 max_keypoints = 8;

    AttenuationCurve curve = AttenuationCurve::Linear;
    float min_dist = 1.f;
    float max_dist = 50.f;
    float rolloff = 1.f;
    AttenuationKeypoint keypoints[max_keypoints] = {};
    u32 keypoint_count = 0;
};

/// OpenAL-style linear distance attenuation (legacy overload).
float compute_attenuation(float distance, float min_dist, float max_dist);

/// Distance attenuation with selectable curve and rolloff factor.
float compute_attenuation(float distance, const AttenuationParams& params);

/// Sample a curve at \p distance without min/max clamping (for unit tests and tooling).
float sample_attenuation_curve(float distance, const AttenuationParams& params);

} // namespace fuse::audio
