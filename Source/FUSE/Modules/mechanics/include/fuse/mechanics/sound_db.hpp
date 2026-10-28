#pragma once

#include <cmath>

namespace fuse::mechanics {

/// True when `db` is finite and in [-80, 12].
[[nodiscard]] inline bool sound_db_ok(float db) {
    return std::isfinite(db) && db >= -80.f && db <= 12.f;
}

} // namespace fuse::mechanics
