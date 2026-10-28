#pragma once

namespace fuse::renderer {

/// True only for cooked asset versions 1 through 16 inclusive.
[[nodiscard]] inline bool cooked_version_ok(int version) {
    return version >= 1 && version <= 16;
}

} // namespace fuse::renderer
