#pragma once

// Particles to emit this step. Non-finite or non-positive rate or dt returns 0.
// Otherwise floor(ratePerSec * dt) clamped to maxEmit.

#include <cmath>
#include <cstdint>

namespace fuse::vfx {

[[nodiscard]] inline std::uint32_t emit_count(float ratePerSec, float dt, std::uint32_t maxEmit) {
    if (!std::isfinite(ratePerSec) || !std::isfinite(dt) || ratePerSec <= 0.f || dt <= 0.f) {
        return 0u;
    }
    const float count = std::floor(ratePerSec * dt);
    // A finite overflow becomes +inf; clamp that to the cap. NaN is not a count.
    if (!std::isfinite(count)) {
        return count > 0.f ? maxEmit : 0u;
    }
    if (count >= static_cast<float>(maxEmit)) {
        return maxEmit;
    }
    if (count <= 0.f) {
        return 0u;
    }
    return static_cast<std::uint32_t>(count);
}

} // namespace fuse::vfx
