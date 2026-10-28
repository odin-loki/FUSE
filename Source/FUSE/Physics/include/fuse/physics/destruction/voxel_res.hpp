#pragma once

namespace fuse::physics {

[[nodiscard]] inline bool voxel_res_ok(int resolution) {
    if (resolution < 8 || resolution > 128) {
        return false;
    }
    return (resolution & (resolution - 1)) == 0;
}

} // namespace fuse::physics
