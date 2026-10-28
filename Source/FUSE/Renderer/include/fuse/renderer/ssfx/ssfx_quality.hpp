#pragma once

namespace fuse::renderer {

/// True when `level` is in the inclusive range 0..2.
[[nodiscard]] inline bool ssfx_quality_ok(int level) {
    return level >= 0 && level <= 2;
}

} // namespace fuse::renderer
