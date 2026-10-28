#pragma once

namespace fuse::relight {

/// True when \p spaces is in the closed range 0..8.
[[nodiscard]] inline bool logic_indent_ok(int spaces) {
    return spaces >= 0 && spaces <= 8;
}

} // namespace fuse::relight
