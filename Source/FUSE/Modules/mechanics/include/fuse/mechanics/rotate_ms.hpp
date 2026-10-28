#pragma once

namespace fuse::mechanics {

/// True when a rotate delay in milliseconds is in the inclusive range 0..60000.
[[nodiscard]] inline bool rotate_ms_ok(int ms) {
    return ms >= 0 && ms <= 60000;
}

} // namespace fuse::mechanics
