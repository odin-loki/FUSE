#pragma once

#include <cmath>

namespace fuse::adventure {

/// True when a door's normalized open amount is at least half open.
/// A non-finite `openAmount` returns false.
[[nodiscard]] inline bool door_is_open(float openAmount) {
    if (!std::isfinite(openAmount)) {
        return false;
    }
    return openAmount >= 0.5f;
}

} // namespace fuse::adventure
