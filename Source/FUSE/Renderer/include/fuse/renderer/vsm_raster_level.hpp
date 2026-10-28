#pragma once

namespace fuse::renderer {

/// True only for levels 0 through 4 inclusive.
[[nodiscard]] inline bool vsm_raster_level_ok(int level) {
    return level >= 0 && level <= 4;
}

} // namespace fuse::renderer
