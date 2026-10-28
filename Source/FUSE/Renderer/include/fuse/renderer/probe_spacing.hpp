#pragma once

#include <cmath>

namespace fuse::renderer {

/// True only when `meters` is finite and in (0, 64].
[[nodiscard]] inline bool probe_spacing_ok(float meters) {
    return std::isfinite(meters) && meters > 0.f && meters <= 64.f;
}

} // namespace fuse::renderer
