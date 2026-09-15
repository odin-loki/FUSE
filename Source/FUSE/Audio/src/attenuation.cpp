#include <fuse/audio/attenuation.hpp>

#include <algorithm>

namespace fuse::audio {

float compute_attenuation(float distance, float min_dist, float max_dist) {
    if (distance <= min_dist) {
        return 1.f;
    }
    if (distance >= max_dist) {
        return 0.f;
    }
    return min_dist / (min_dist + (distance - min_dist));
}

} // namespace fuse::audio
