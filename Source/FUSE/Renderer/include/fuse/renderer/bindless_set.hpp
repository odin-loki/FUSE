#pragma once

namespace fuse::renderer {

/// True only for sets 0 through 7 inclusive.
[[nodiscard]] inline bool bindless_set_ok(int set) {
    return set >= 0 && set <= 7;
}

} // namespace fuse::renderer
