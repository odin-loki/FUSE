#pragma once

namespace fuse::script {

/// True when `spaces` is in the closed range [0, 32].
[[nodiscard]] inline bool script_indent_ok(int spaces) {
    return spaces >= 0 && spaces <= 32;
}

} // namespace fuse::script
