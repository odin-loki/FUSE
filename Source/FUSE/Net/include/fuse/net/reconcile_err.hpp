#pragma once

#include <cmath>

namespace fuse::net {

/// True when `error` is finite and in [0, 10].
[[nodiscard]] inline bool reconcile_err_ok(float error) noexcept {
    return std::isfinite(error) && error >= 0.f && error <= 10.f;
}

} // namespace fuse::net
