#pragma once

namespace fuse::renderer {

/// True only for pass counts 1 through 64 inclusive.
[[nodiscard]] inline bool rg_pass_ok(int count) {
    return count >= 1 && count <= 64;
}

} // namespace fuse::renderer
