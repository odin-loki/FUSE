#pragma once

// Distance kept for a ray query: a usable hit, or the miss distance.
// A hit whose t is finite and inside [0, tMax] returns that t. A miss, and any
// other hit, returns tMax when tMax is finite and positive, otherwise 0.

#include <cmath>

namespace fuse::renderer::rt {

[[nodiscard]] inline float miss_distance(float tHit, float tMax, bool hit) {
    if (hit && std::isfinite(tHit) && tHit >= 0.f && tHit <= tMax) {
        return tHit;
    }
    if (std::isfinite(tMax) && tMax > 0.f) {
        return tMax;
    }
    return 0.f;
}

} // namespace fuse::renderer::rt
