#pragma once

namespace fuse::renderer {

/// True when `level` is in the inclusive range 0..6.
[[nodiscard]] inline bool stream_lod_ok(int level) {
    return level >= 0 && level <= 6;
}

} // namespace fuse::renderer
