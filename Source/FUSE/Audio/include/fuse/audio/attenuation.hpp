#pragma once

namespace fuse::audio {

/// OpenAL-style linear distance attenuation.
float compute_attenuation(float distance, float min_dist, float max_dist);

} // namespace fuse::audio
