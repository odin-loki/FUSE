#pragma once

namespace fuse::renderer {

/// True only for G-buffer pass indexes 0 through 3 inclusive.
[[nodiscard]] inline bool gbuffer_pass_ok(int index) {
    return index >= 0 && index <= 3;
}

} // namespace fuse::renderer
