#pragma once

#include <cmath>

namespace fuse::renderer {

/// True only when `ev` is finite and lies in [-8, 8].
[[nodiscard]] inline bool exposure_ev_ok(float ev) {
    return std::isfinite(ev) && ev >= -8.f && ev <= 8.f;
}

} // namespace fuse::renderer
