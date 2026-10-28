// FUSE Relight: photometric intensity in nits. Header-only.
// intensity_ok is true for a finite value in the inclusive range [0, 100000].
#pragma once

#include <cmath>

namespace fuse::relight {

/// True when `nits` is finite and in the inclusive range [0, 100000].
[[nodiscard]] inline bool intensity_ok(float nits) {
    return std::isfinite(nits) && nits >= 0.f && nits <= 100000.f;
}

} // namespace fuse::relight
