#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `scale` is finite and lies in (1, 4].
[[nodiscard]] inline bool upscale_ref_ok(float scale) {
    return std::isfinite(scale) && scale > 1.f && scale <= 4.f;
}

} // namespace fuse::renderer
