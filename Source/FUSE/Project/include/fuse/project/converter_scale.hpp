#pragma once

#include <cmath>

namespace fuse::project {

/// True when `scale` is finite and lies in (0, 100].
[[nodiscard]] inline bool converter_scale_ok(float scale) {
    return std::isfinite(scale) && scale > 0.f && scale <= 100.f;
}

} // namespace fuse::project
