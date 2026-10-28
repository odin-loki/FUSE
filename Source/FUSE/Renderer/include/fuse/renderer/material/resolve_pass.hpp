#pragma once

namespace fuse::renderer {

/// True when `index` is in the inclusive range 0..3.
[[nodiscard]] inline bool resolve_pass_ok(int index) {
    return index >= 0 && index <= 3;
}

} // namespace fuse::renderer
