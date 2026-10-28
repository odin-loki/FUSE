#pragma once

#include <cmath>

namespace fuse::compute {

/// True when `delta` is finite and lies in (0, 0.01].
[[nodiscard]] inline bool normal_delta_ok(float delta) {
    return std::isfinite(delta) && delta > 0.f && delta <= 0.01f;
}

} // namespace fuse::compute
