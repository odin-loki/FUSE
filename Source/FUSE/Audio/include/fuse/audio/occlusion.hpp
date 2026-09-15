#pragma once

#include <fuse/audio/math.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

/// Occlusion stub parameters — maps visibility to gain and HF rolloff placeholders.
struct OcclusionParams {
    float min_gain = 0.1f;
    float hf_attenuation = 0.6f;
};

/// Map visibility [0, 1] to a gain multiplier. Fully occluded sources retain `min_gain`.
float evaluate_occlusion_gain(float visibility, const OcclusionParams& params = {});

/// Line-of-sight stub — returns reduced visibility when a blocker AABB intersects the segment.
float compute_blocker_visibility(const Vec3& listener, const Vec3& source, const AABB& blocker);

} // namespace fuse::audio
