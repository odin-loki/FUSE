#pragma once

namespace fuse::terrain {

[[nodiscard]] inline bool terrain_lod_ok(int level) {
    return level >= 0 && level <= 4;
}

} // namespace fuse::terrain
