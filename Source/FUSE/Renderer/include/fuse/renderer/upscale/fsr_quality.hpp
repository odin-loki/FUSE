#pragma once

namespace fuse::renderer {

/// True when `level` is in the inclusive range 0..3.
[[nodiscard]] inline bool fsr_quality_ok(int level) {
    return level >= 0 && level <= 3;
}

} // namespace fuse::renderer
