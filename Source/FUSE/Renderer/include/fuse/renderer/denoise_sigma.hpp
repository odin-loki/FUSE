#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `sigma` is finite and lies in (0, 8].
[[nodiscard]] inline bool denoise_sigma_ok(float sigma) {
    return std::isfinite(sigma) && sigma > 0.f && sigma <= 8.f;
}

} // namespace fuse::renderer
