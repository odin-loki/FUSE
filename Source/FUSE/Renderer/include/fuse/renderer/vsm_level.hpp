#pragma once

namespace fuse::renderer {

/// True only for levels 0 through 7 inclusive.
[[nodiscard]] inline bool vsm_level_ok(int level) {
    return level >= 0 && level <= 7;
}

} // namespace fuse::renderer
