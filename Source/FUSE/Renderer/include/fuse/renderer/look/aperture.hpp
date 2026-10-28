#pragma once

#include <cmath>

namespace fuse::renderer {

[[nodiscard]] inline bool aperture_ok(float fStop) {
    return std::isfinite(fStop) && fStop >= 1.f && fStop <= 22.f;
}

} // namespace fuse::renderer
