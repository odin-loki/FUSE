#pragma once

namespace fuse::renderer {

/// True only for levels 0 through 8 inclusive.
[[nodiscard]] inline bool splat_lod_ok(int level) {
    return level >= 0 && level <= 8;
}

} // namespace fuse::renderer
