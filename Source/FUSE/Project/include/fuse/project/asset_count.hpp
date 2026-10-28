#pragma once

namespace fuse::project {

/// True for asset counts 1 through 4096 inclusive.
[[nodiscard]] inline bool asset_count_ok(int count) {
    return count >= 1 && count <= 4096;
}

} // namespace fuse::project
