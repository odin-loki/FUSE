#pragma once

#include <cmath>

namespace fuse::audio {

/// True when \p db is finite and in [-24, 0].
[[nodiscard]] inline bool duck_db_ok(float db) {
    return std::isfinite(db) && db >= -24.f && db <= 0.f;
}

} // namespace fuse::audio
