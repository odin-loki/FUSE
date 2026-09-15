#pragma once

#include <fuse/types.hpp>

namespace fuse::audio {

/// Distance attenuation curve — mirrors Torque SFXDistanceModel semantics.
enum class AttenuationCurve : u8 {
    Linear = 0,
    Logarithmic = 1,
    Exponential = 2,
};

struct AttenuationParams {
    AttenuationCurve curve = AttenuationCurve::Linear;
    float min_dist = 1.f;
    float max_dist = 50.f;
    float rolloff = 1.f;
};

/// OpenAL-style linear distance attenuation (legacy overload).
float compute_attenuation(float distance, float min_dist, float max_dist);

/// Distance attenuation with selectable curve and rolloff factor.
float compute_attenuation(float distance, const AttenuationParams& params);

} // namespace fuse::audio
