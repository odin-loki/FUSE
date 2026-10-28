#pragma once

namespace fuse::renderer {

/// True only for target counts 1 through 8 inclusive.
[[nodiscard]] inline bool deferred_target_ok(int targets) {
    return targets >= 1 && targets <= 8;
}

} // namespace fuse::renderer
