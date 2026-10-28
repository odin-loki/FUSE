#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `jitter` is finite and in the inclusive range [-1, 1].
[[nodiscard]] inline bool taa_jitter_ok(float jitter) {
    return std::isfinite(jitter) && jitter >= -1.f && jitter <= 1.f;
}

} // namespace fuse::renderer
