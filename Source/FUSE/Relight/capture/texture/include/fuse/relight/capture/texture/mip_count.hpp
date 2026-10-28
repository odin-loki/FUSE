#pragma once

namespace fuse::relight {

[[nodiscard]] inline bool mip_count_ok(int count) {
    return count >= 1 && count <= 16;
}

} // namespace fuse::relight
